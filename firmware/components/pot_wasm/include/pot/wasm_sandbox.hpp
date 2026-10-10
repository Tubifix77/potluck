// M7, as an experiment: a sandbox for code the owner does not trust (ARCHITECTURE section 13, M7).
//
// One WebAssembly module, interpreted by wasm3 (v0.9.0, MIT; third_party/wasm3), behind four limits
// the guest cannot lift:
//   fuel          every loop iteration and every call costs one unit; at zero the guest traps
//                 ("out of fuel"). wasm3 checks a hook only at calls, so third_party/wasm3 is patched to
//                 check it on every loop back-edge too (POTLUCK-PATCHES.md) -- otherwise `loop br 0`
//                 could never be stopped.
//   memory        linear memory is capped at load; memory.grow beyond it returns -1. Every load and store
//                 is bounds-checked by the interpreter: out of bounds is a trap, never an access.
//   native stack  a guest's calls run on the caller's C stack; past the limit a call traps.
//   imports       the guest can call nothing but what the sandbox provides -- today potluck.log(i32).
//                 A module importing anything else (a pin, the radio, memory of the node) is refused at
//                 load. There is no way for a guest to name an address outside its own memory.
//
// Portable: it builds for the host tests and for the boards. Off on the boards unless CONFIG_POT_WASM.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

struct WasmLimits {
    uint32_t memory_bytes = 64 * 1024;  // a multiple of the 64 KiB page; 0 = no linear memory allowed
    uint32_t value_stack_bytes = 8 * 1024;
};

// M7: called by the fuel hook every `yield_every` units of fuel spent -- on a board, to let the idle
// task run while a guest burns a long budget on a background worker, so the task watchdog stays happy.
// Portable: null on the host, where nothing needs yielding.
struct WasmYield {
    void (*fn)(void* ctx) = nullptr;
    void* ctx = nullptr;
    uint32_t every = 50000;
};
void wasm_set_yield(const WasmYield& y);

struct WasmImport;  // pot/wasm_imports.hpp

struct WasmCallResult {
    const char* error = nullptr;  // nullptr: returned normally; otherwise the trap or refusal
    uint32_t value = 0;           // the i32 result, when the function has one
    uint64_t fuel_used = 0;
};

class WasmSandbox {
  public:
    WasmSandbox() = default;
    ~WasmSandbox();
    WasmSandbox(const WasmSandbox&) = delete;
    WasmSandbox& operator=(const WasmSandbox&) = delete;

    // Parses, checks the imports, links what is allowed. nullptr on success, else why it was refused.
    // The bytes must outlive the sandbox (wasm3 keeps pointers into them).
    const char* load(const uint8_t* wasm, size_t len, const WasmLimits& limits);
    // M7: with the caller's import table instead of the default (potluck.log alone), and `user` as what
    // the imports read with m3_GetUserData. `user` must outlive the sandbox.
    const char* load(const uint8_t* wasm, size_t len, const WasmLimits& limits, const WasmImport* imports,
                     size_t n_imports, void* user);
    void unload();
    bool loaded() const { return runtime_ != nullptr; }

    // Calls an exported function taking up to four i32 arguments and returning at most one i32.
    // `native_stack_bytes` bounds how far below this call's own frame the guest may drive the C stack.
    WasmCallResult call(const char* name, const uint32_t* args, unsigned nargs, uint64_t fuel,
                        uint32_t native_stack_bytes);

    // What the guest logged through potluck.log, the last value and how often (the experiment's only import).
    uint32_t log_count() const { return log_count_; }
    uint32_t log_last() const { return log_last_; }

  private:
    void* env_ = nullptr;      // IM3Environment
    void* runtime_ = nullptr;  // IM3Runtime
    uint32_t log_count_ = 0;
    uint32_t log_last_ = 0;
    friend struct WasmImports;
};

// The error a call returns when the guest's fuel ran out (compare the pointer).
extern "C" const char* pot_wasm_out_of_fuel;

}  // namespace pot
