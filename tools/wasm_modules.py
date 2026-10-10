"""M7's experiment: the WebAssembly modules it runs, assembled here byte by byte.

    python tools/wasm_modules.py   # writes firmware/components/pot_wasm/include/pot/wasm_modules.h

No WebAssembly compiler is installed on the bench PC, and installing one was not part of what the
owner allowed downloading, so the few small modules the experiment needs are encoded by hand from the
spec's binary format (webassembly.github.io/spec/core/binary/, ledger 2026-10-10). The runtime
validates every module it loads, so an encoding mistake here is a load error, never a silent one.

  mc       mc_hits(seed, unit, samples) -> hits: M9's Monte Carlo kernel (pot_actors/src/mc.cpp), the
           same xorshift32 and the same float arithmetic, so its answer can be compared with native's
  spin     spin(): `loop br 0` -- a guest that never returns and never calls anything
  oob      oob() -> i32: a load one byte past the end of its 64 KiB memory
  grow     grow(pages) -> i32: memory.grow, to meet the runtime's memory limit
  deep     deep(n) -> i32: calls itself without end
  pin      poke(): imports env.gpio_set(pin, level), which no Potluck runtime provides
  logger   hello(x): imports potluck.log(x), which the experiment does provide, and calls it
  memimp   imports env.memory -- a memory from outside the sandbox
  globimp  imports env.g, an i32 global from outside the sandbox
"""

from __future__ import annotations

import os
import struct
import sys

I32, F32 = 0x7F, 0x7D


def uleb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def vec(items: list[bytes]) -> bytes:
    return uleb(len(items)) + b"".join(items)


def name(s: str) -> bytes:
    return uleb(len(s)) + s.encode()


def section(sid: int, body: bytes) -> bytes:
    return bytes([sid]) + uleb(len(body)) + body


def functype(params: list[int], results: list[int]) -> bytes:
    return b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])


def i32c(v: int) -> bytes:  # i32.const, v as a signed 32-bit value
    if v >= 1 << 31:
        v -= 1 << 32
    return b"\x41" + sleb(v)


def f32c(v: float) -> bytes:
    return b"\x43" + struct.pack("<f", v)


# opcodes (spec, binary/instructions)
UNREACHABLE, BLOCK, LOOP, IF, ELSE, END, BR, BR_IF, RETURN, CALL, DROP = (
    0x00, 0x02, 0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x1A)
LGET, LSET, LTEE = 0x20, 0x21, 0x22
I32_LOAD, I32_STORE, MEM_GROW = 0x28, 0x36, 0x40
I32_EQZ, I32_EQ, I32_NE, I32_LT_U, F32_LT = 0x45, 0x46, 0x47, 0x49, 0x5D
I32_ADD, I32_SUB, I32_MUL, I32_XOR, I32_SHL, I32_SHR_U = 0x6A, 0x6B, 0x6C, 0x73, 0x74, 0x76
F32_ADD, F32_MUL, F32_CONVERT_I32_U = 0x92, 0x94, 0xB3
EMPTY = 0x40


def op(*xs: int) -> bytes:
    return bytes(xs)


def lget(i: int) -> bytes:
    return op(LGET) + uleb(i)


def lset(i: int) -> bytes:
    return op(LSET) + uleb(i)


def module(types: list[bytes], funcs: list[tuple[int, list[tuple[int, int]], bytes]], exports: list[tuple[str, int, int]],
           imports: list[tuple[str, str, int]] | None = None, memory: tuple[int, int | None] | None = None,
           raw_imports: list[bytes] | None = None) -> bytes:
    """funcs: (type index, locals as (count, type), body without the final end)."""
    out = b"\x00asm" + b"\x01\x00\x00\x00"
    out += section(1, vec(types))
    if imports or raw_imports:
        entries = [name(m) + name(n) + b"\x00" + uleb(t) for m, n, t in (imports or [])]
        out += section(2, vec(entries + (raw_imports or [])))
    out += section(3, vec([uleb(t) for t, _, _ in funcs]))
    if memory is not None:
        lo, hi = memory
        lim = (b"\x00" + uleb(lo)) if hi is None else (b"\x01" + uleb(lo) + uleb(hi))
        out += section(5, vec([lim]))
    out += section(7, vec([name(n) + bytes([kind]) + uleb(idx) for n, kind, idx in exports]))
    codes = []
    for _, locs, body in funcs:
        f = vec([uleb(c) + bytes([t]) for c, t in locs]) + body + op(END)
        codes.append(uleb(len(f)) + f)
    out += section(10, vec(codes))
    return out


def xorshift(s: int) -> bytes:
    # s ^= s << 13; s ^= s >> 17; s ^= s << 5
    b = b""
    for sh, o in ((13, I32_SHL), (17, I32_SHR_U), (5, I32_SHL)):
        b += lget(s) + lget(s) + i32c(sh) + op(o, I32_XOR) + lset(s)
    return b


def mix32_inline(v: int) -> bytes:
    # v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; v ^= v >> 16 (all on local v)
    b = lget(v) + lget(v) + i32c(16) + op(I32_SHR_U, I32_XOR) + lset(v)
    b += lget(v) + i32c(0x7FEB352D) + op(I32_MUL) + lset(v)
    b += lget(v) + lget(v) + i32c(15) + op(I32_SHR_U, I32_XOR) + lset(v)
    b += lget(v) + i32c(0x846CA68B) + op(I32_MUL) + lset(v)
    b += lget(v) + lget(v) + i32c(16) + op(I32_SHR_U, I32_XOR) + lset(v)
    return b


def mc_module() -> bytes:
    # params 0 seed, 1 unit, 2 samples; locals 3 s, 4 i, 5 hits, 6 t (i32); 7 x, 8 y (f32)
    S, I, H, T, X, Y = 3, 4, 5, 6, 7, 8
    scale = 1.0 / 16777216.0
    b = b""
    # t = mix32(unit + 0x9e3779b9)
    b += lget(1) + i32c(0x9E3779B9) + op(I32_ADD) + lset(T) + mix32_inline(T)
    # s = mix32(seed ^ t)
    b += lget(0) + lget(T) + op(I32_XOR) + lset(S) + mix32_inline(S)
    # if (s == 0) s = 1
    b += lget(S) + op(I32_EQZ, IF, EMPTY) + i32c(1) + lset(S) + op(END)
    # block { loop { if !(i < samples) br 1; ...; i++; br 0 } }
    b += op(BLOCK, EMPTY, LOOP, EMPTY)
    b += lget(I) + lget(2) + op(I32_LT_U, I32_EQZ, BR_IF) + uleb(1)
    for dst in (X, Y):
        b += xorshift(S)
        b += lget(S) + i32c(8) + op(I32_SHR_U, F32_CONVERT_I32_U) + f32c(scale) + op(F32_MUL) + lset(dst)
    # if (x*x + y*y < 1.0) hits++
    b += lget(X) + lget(X) + op(F32_MUL) + lget(Y) + lget(Y) + op(F32_MUL, F32_ADD) + f32c(1.0) + op(F32_LT)
    b += op(IF, EMPTY) + lget(H) + i32c(1) + op(I32_ADD) + lset(H) + op(END)
    b += lget(I) + i32c(1) + op(I32_ADD) + lset(I)
    b += op(BR) + uleb(0) + op(END, END)
    b += lget(H)
    t = functype([I32, I32, I32], [I32])
    return module([t], [(0, [(4, I32), (2, F32)], b)], [("mc_hits", 0x00, 0)])


def spin_module() -> bytes:
    b = op(LOOP, EMPTY, BR) + uleb(0) + op(END)
    return module([functype([], [])], [(0, [], b)], [("spin", 0x00, 0)])


def oob_module() -> bytes:
    b = i32c(65536) + op(I32_LOAD) + uleb(2) + uleb(0)  # align 2^2, offset 0: the first byte past the end
    return module([functype([], [I32])], [(0, [], b)], [("oob", 0x00, 0)], memory=(1, None))


def grow_module() -> bytes:
    b = lget(0) + op(MEM_GROW, 0x00)
    return module([functype([I32], [I32])], [(0, [], b)], [("grow", 0x00, 0)], memory=(1, None))


def deep_module() -> bytes:
    b = lget(0) + i32c(1) + op(I32_ADD, CALL) + uleb(0)
    return module([functype([I32], [I32])], [(0, [], b)], [("deep", 0x00, 0)])


def pin_module() -> bytes:
    # import 0 = env.gpio_set(i32, i32); function 1 = poke
    b = i32c(5) + i32c(1) + op(CALL) + uleb(0)
    return module([functype([I32, I32], []), functype([], [])], [(1, [], b)], [("poke", 0x00, 1)],
                  imports=[("env", "gpio_set", 0)])


def logger_module() -> bytes:
    b = lget(0) + op(CALL) + uleb(0)
    return module([functype([I32], []), functype([I32], [])], [(1, [], b)], [("hello", 0x00, 1)],
                  imports=[("potluck", "log", 0)])


def memimp_module() -> bytes:
    # import env.memory: externtype 02, limits 00, min 1
    return module([functype([], [])], [(0, [], b"")], [("f", 0x00, 0)],
                  raw_imports=[name("env") + name("memory") + b"\x02\x00\x01"])


def globimp_module() -> bytes:
    # import env.g: externtype 03, globaltype i32, 00 = immutable
    return module([functype([], [])], [(0, [], b"")], [("f", 0x00, 0)],
                  raw_imports=[name("env") + name("g") + b"\x03" + bytes([I32]) + b"\x00"])


MODULES = {
    "mc": mc_module, "spin": spin_module, "oob": oob_module, "grow": grow_module, "deep": deep_module,
    "pin": pin_module, "logger": logger_module, "memimp": memimp_module, "globimp": globimp_module,
}


def main() -> None:
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "firmware", "components", "pot_wasm", "include",
                                                             "pot", "wasm_modules.h")
    lines = ["// Generated by tools/wasm_modules.py (M7's experiment). Do not edit.", "#pragma once", "",
             "#include <cstddef>", "#include <cstdint>", "", "namespace pot {", "namespace wasm_modules {", ""]
    for n, f in MODULES.items():
        data = f()
        body = ", ".join(f"0x{x:02x}" for x in data)
        lines.append(f"inline constexpr uint8_t k_{n}[] = {{{body}}};")
    lines += ["", "}  // namespace wasm_modules", "}  // namespace pot", ""]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines))
    for n, f in MODULES.items():
        print(f"{n:7s} {len(f()):4d} B")


if __name__ == "__main__":
    main()
