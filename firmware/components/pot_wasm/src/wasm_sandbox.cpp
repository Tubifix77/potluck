// M7's experiment: the sandbox. See pot/wasm_sandbox.hpp.

#include "pot/wasm_sandbox.hpp"

#include <cstring>

extern "C" {
#include "m3_env.h"
#include "wasm3.h"
}
#include "pot/wasm_imports.hpp"

namespace pot {

extern "C" {
const char* pot_wasm_out_of_fuel = "[trap] out of fuel (potluck)";
}

namespace {
// One guest runs at a time on a board (the interpreter's hook has no context argument), so the budget
// is a single counter, set by call() and read by the hook.
uint64_t g_fuel = 0;
uint64_t g_fuel_start = 0;
}  // namespace

}  // namespace pot

// The patched interpreter calls this at every call and every loop back-edge (POTLUCK-PATCHES.md).
extern "C" M3Result m3_Yield(void) {
    if (pot::g_fuel == 0) return pot::pot_wasm_out_of_fuel;
    --pot::g_fuel;
    return m3Err_none;
}

namespace pot {

struct WasmImports {
    static m3ApiRawFunction(log) {
        (void)_ctx;
        (void)_mem;
        m3ApiGetArg(uint32_t, v);
        auto* sb = static_cast<WasmSandbox*>(m3_GetUserData(runtime));
        if (sb != nullptr) {
            ++sb->log_count_;
            sb->log_last_ = v;
        }
        m3ApiSuccess();
    }
};

namespace {
constexpr const char* kRefusedImport = "refused: the module imports something the sandbox does not provide";
constexpr const char* kTooMuchMemory = "refused: the module's minimum memory exceeds the limit";
}  // namespace

WasmSandbox::~WasmSandbox() { unload(); }

void WasmSandbox::unload() {
    if (runtime_ != nullptr) m3_FreeRuntime(static_cast<IM3Runtime>(runtime_));  // frees the loaded module
    if (env_ != nullptr) m3_FreeEnvironment(static_cast<IM3Environment>(env_));
    runtime_ = nullptr;
    env_ = nullptr;
}

const char* WasmSandbox::load(const uint8_t* wasm, size_t len, const WasmLimits& limits) {
    static const WasmImport kDefault[] = {{"log", "v(i)", &WasmImports::log}};
    return load(wasm, len, limits, kDefault, 1, this);
}

const char* WasmSandbox::load(const uint8_t* wasm, size_t len, const WasmLimits& limits, const WasmImport* imports,
                              size_t n_imports, void* user) {
    unload();
    log_count_ = 0;
    IM3Environment env = m3_NewEnvironment();
    if (env == nullptr) return "m3_NewEnvironment failed";
    IM3Runtime rt = m3_NewRuntime(env, limits.value_stack_bytes, user);
    if (rt == nullptr) {
        m3_FreeEnvironment(env);
        return "m3_NewRuntime failed";
    }
    rt->memoryLimit = limits.memory_bytes;
    IM3Module mod = nullptr;
    M3Result r = m3_ParseModule(env, &mod, wasm, static_cast<uint32_t>(len));
    if (r == m3Err_none) {
        // Imports: everything the module names must be on the allow-list, or nothing of it runs.
        for (u32 i = 0; i < mod->numFunctions && r == m3Err_none; ++i) {
            const M3Function& f = mod->functions[i];
            if (f.import.moduleUtf8 == nullptr) continue;
            bool allowed = false;
            if (std::strcmp(f.import.moduleUtf8, "potluck") == 0) {
                for (size_t k = 0; k < n_imports && !allowed; ++k) {
                    allowed = std::strcmp(f.import.fieldUtf8, imports[k].name) == 0;
                }
            }
            if (!allowed) r = kRefusedImport;
        }
        // A memory or a global from outside is an import too: refused like a function would be.
        if (mod->memoryImported) r = kRefusedImport;
        for (u32 i = 0; i < mod->numGlobals && r == m3Err_none; ++i) {
            if (mod->globals[i].import.moduleUtf8 != nullptr) r = kRefusedImport;
        }
        if (r == m3Err_none && mod->memoryInfo.initPages * 65536ull > limits.memory_bytes) r = kTooMuchMemory;
        if (r != m3Err_none) m3_FreeModule(mod);
    }
    if (r == m3Err_none) {
        r = m3_LoadModule(rt, mod);  // the runtime owns the module from here, even on failure
    }
    if (r == m3Err_none && rt->memory.pageSize != 0) {
        // The limit is the memory's maximum: memory.grow past it returns -1 to the guest. (wasm3's own
        // memoryLimit only caps the allocation and still reports the grow as done.)
        const u32 cap_pages = limits.memory_bytes / rt->memory.pageSize;
        if (rt->memory.maxPages > cap_pages) rt->memory.maxPages = cap_pages;
    }
    if (r == m3Err_none) {
        for (size_t k = 0; k < n_imports && r == m3Err_none; ++k) {
            const M3Result lr = m3_LinkRawFunction(mod, "potluck", imports[k].name, imports[k].signature, imports[k].fn);
            if (lr != m3Err_none && lr != m3Err_functionLookupFailed) r = lr;  // not imported: nothing to link
        }
    }
    if (r != m3Err_none) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return r;
    }
    env_ = env;
    runtime_ = rt;
    return nullptr;
}

WasmCallResult WasmSandbox::call(const char* name, const uint32_t* args, unsigned nargs, uint64_t fuel,
                                 uint32_t native_stack_bytes) {
    WasmCallResult out;
    if (runtime_ == nullptr) {
        out.error = "no module loaded";
        return out;
    }
    IM3Runtime rt = static_cast<IM3Runtime>(runtime_);
    IM3Function f = nullptr;
    M3Result r = m3_FindFunction(&f, rt, name);
    if (r != m3Err_none) {
        out.error = r;
        return out;
    }
    if (nargs > 4 || m3_GetArgCount(f) != nargs || m3_GetRetCount(f) > 1) {
        out.error = "refused: only functions of up to four i32 arguments and one i32 result";
        return out;
    }
    const void* argp[4];
    for (unsigned i = 0; i < nargs; ++i) argp[i] = &args[i];
    // The native stack bound, from this frame down: the patched interpreter traps a call past it.
    volatile uint8_t here = 0;
    rt->stackLimit = const_cast<uint8_t*>(&here) - native_stack_bytes;
    g_fuel = fuel;
    g_fuel_start = fuel;
    r = m3_Call(f, nargs, argp);
    out.fuel_used = g_fuel_start - g_fuel;
    g_fuel = 0;
    rt->stackLimit = nullptr;
    if (r != m3Err_none) {
        out.error = r;
        return out;
    }
    if (m3_GetRetCount(f) == 1) {
        uint32_t v = 0;
        const void* retp[1] = {&v};
        r = m3_GetResults(f, 1, retp);
        if (r != m3Err_none) out.error = r;
        out.value = v;
    }
    return out;
}

}  // namespace pot
