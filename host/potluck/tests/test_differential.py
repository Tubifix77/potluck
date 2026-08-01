"""Differential fuzzing: the two Potluck Frame decoders must agree on every input.

`build/tests/pot_tests.exe --emit-fuzz-corpus <dir>` writes every input the C++
fuzzer generated, together with the C++ parser's verdict -- the rejection reason,
or every decoded field if it accepted. This replays the same bytes through the
Python decoder and requires an identical verdict.

Why this is the most valuable test in the repository:

The golden-byte tests prove the two implementations agree on frames a human
thought to write down. That catches a field at the wrong offset. It does not catch
a *rule* that one implementation enforces and the other does not -- a length check
one side forgot, an edge case at a wrap boundary, a flag combination nobody
considered. Those live in the inputs nobody imagined, which is exactly what a
fuzzer produces.

Two implementations written independently from ARCHITECTURE.md section 5,
agreeing on tens of thousands of adversarial inputs, is a test of the
specification rather than of either codebase. Section 14 asks for the wire format
to be "specified and versioned independently of the implementation"; a
disagreement here means section 5 is ambiguous, which is a documentation bug and
the most useful kind to find.

Regenerate the corpus with:

    build/tests/pot_tests.exe --emit-fuzz-corpus host/potluck/tests/fixtures

The corpus is deterministic -- the C++ fuzzer uses fixed seeds -- so regenerating
it produces the same bytes and a diff means the generator changed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck import frame as fr  # noqa: E402

CORPUS = Path(__file__).parent / "fixtures" / "fuzz_corpus.jsonl"


def _corpus_rows() -> list[dict]:
    if not CORPUS.exists():
        raise AssertionError(
            f"{CORPUS} is missing. Generate it with:\n"
            "  build/tests/pot_tests.exe --emit-fuzz-corpus host/potluck/tests/fixtures"
        )
    rows = []
    with open(CORPUS, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise AssertionError(f"{CORPUS}:{n} is not valid JSON: {exc}") from exc
    return rows


def _python_verdict(raw: bytes, cap: int) -> tuple[str, dict | None]:
    """Run the Python decoder and return (reason, fields) the way the C++ side reports it."""
    try:
        f = fr.parse(raw, cap)
    except fr.FrameError as exc:
        return exc.reason, None
    return "ok", {
        "src": f.src,
        "dst": f.dst,
        "opcode": f.opcode,
        "lclass": f.lclass,
        "priority": f.priority,
        "seq": f.seq,
        "msg_id": f.msg_id,
        "frag_off": f.frag_off,
        "total_len": f.total_len,
        "payload_len": len(f.payload),
        "frag": f.is_frag,
        "ackreq": f.wants_ack,
        "auth": f.has_auth,
    }


FIELDS = (
    "src", "dst", "opcode", "lclass", "priority", "seq", "msg_id",
    "frag_off", "total_len", "payload_len", "frag", "ackreq", "auth",
)


def test_the_corpus_is_substantial():
    rows = _corpus_rows()
    # A corpus that silently shrank to a handful of inputs would pass every other
    # test in this file while testing almost nothing.
    assert len(rows) > 8000, f"only {len(rows)} inputs; expected the full fuzz corpus"

    # And it must exercise both outcomes. A corpus of only-rejections proves the
    # two implementations agree on saying no, which is much easier than agreeing
    # on what a frame means.
    accepted = sum(1 for r in rows if r["verdict"] == "ok")
    assert accepted > 100, f"only {accepted} inputs were accepted by the C++ parser"
    assert accepted < len(rows), "no input was rejected; the fuzzer is not fuzzing"


def test_both_transport_profile_caps_are_exercised():
    caps = {r["cap"] for r in _corpus_rows()}
    # Section 5.3's two ESP-NOW profiles. A corpus run at only one cap would miss
    # a disagreement about where the other one is enforced.
    assert caps == {fr.MAX_PAYLOAD_V1, fr.MAX_PAYLOAD_V2}, caps


def test_the_two_decoders_agree_on_every_input():
    rows = _corpus_rows()
    disagreements: list[str] = []

    for i, row in enumerate(rows):
        raw = bytes.fromhex(row["raw"])
        cap = int(row["cap"])
        cpp_verdict = row["verdict"]
        py_verdict, py_fields = _python_verdict(raw, cap)

        if py_verdict != cpp_verdict:
            disagreements.append(
                f"[{i}] cap={cap} verdict: C++ said {cpp_verdict!r}, "
                f"Python said {py_verdict!r}\n      raw = {row['raw']}"
            )
            continue

        if cpp_verdict != "ok":
            continue

        for field in FIELDS:
            cpp_value = row[field]
            py_value = py_fields[field]
            if cpp_value != py_value:
                disagreements.append(
                    f"[{i}] cap={cap} field {field}: C++ {cpp_value!r} vs Python {py_value!r}\n"
                    f"      raw = {row['raw']}"
                )

        # Every 3 disagreements is already plenty to debug; a wall of thousands is
        # not more informative and makes the first one hard to find.
        if len(disagreements) >= 20:
            disagreements.append("... truncated")
            break

    if disagreements:
        raise AssertionError(
            f"{len(disagreements)} disagreement(s) between the two decoders over "
            f"{len(rows)} inputs.\nA disagreement means ARCHITECTURE.md section 5 is "
            "ambiguous, or one implementation has a bug.\n\n" + "\n".join(disagreements)
        )


def test_python_reencode_reproduces_every_accepted_input():
    """Anything the Python side accepts must re-encode to the same bytes.

    The same property the C++ fuzzer checks, applied to the Python encoder, so the
    two encoders are covered as well as the two decoders. The one legitimate
    asymmetry is a nonzero auth tag: parse() accepts it because M5 owns
    verification, while encode() always writes zeroes, so those are skipped.
    """
    checked = 0
    for row in _corpus_rows():
        if row["verdict"] != "ok":
            continue
        raw = bytes.fromhex(row["raw"])
        f = fr.parse(raw, int(row["cap"]))
        if f.auth_tag is not None and f.auth_tag != bytes(fr.AUTH_TAG_SIZE):
            continue

        again = fr.encode(
            src=f.src,
            dst=f.dst,
            opcode=f.opcode,
            lclass=f.lclass,
            priority=f.priority,
            seq=f.seq,
            msg_id=f.msg_id,
            payload=f.payload,
            ack_req=f.wants_ack,
            auth=f.has_auth,
            frag=f.is_frag,
            frag_off=f.frag_off,
            total_len=f.total_len if f.is_frag else None,
        )
        assert again == raw, (
            f"re-encode differs\n  in  = {raw.hex()}\n  out = {again.hex()}"
        )
        checked += 1
    assert checked > 100, f"only {checked} accepted inputs were re-encoded"


def test_every_verdict_name_is_one_the_python_side_knows():
    """The rejection reasons must be the same vocabulary on both sides.

    Not cosmetic: the ERR opcode carries these values (opcodes.hpp maps codes below
    0x0100 onto FrameError), and a capture is read by the Python side. A reason the
    host does not recognise is a reason nobody will act on.
    """
    known = {
        "ok", "too_short", "bad_magic", "bad_version", "reserved_flag_set", "bad_class",
        "missing_auth_tag", "length_mismatch", "frag_not_allowed", "payload_too_long",
        "buffer_too_small", "bad_argument",
    }
    seen = {r["verdict"] for r in _corpus_rows()}
    unknown = seen - known
    assert not unknown, f"verdicts the Python side does not know: {unknown}"

    # And the corpus should reach most of them; a corpus that only ever produces
    # bad_magic is not exploring the parser.
    assert len(seen) >= 6, f"the corpus only produced {len(seen)} distinct verdicts: {seen}"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
