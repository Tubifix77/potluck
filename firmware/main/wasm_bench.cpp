// M7's experiment on a board: what the sandbox costs, and whether it holds (ARCHITECTURE section 13, M7).
//
// Built only with CONFIG_POT_WASM_BENCH. Like the crypto bench it runs first and then nothing else: no
// radio, no serial link, no tasks but its own. Prints one {"t":"wasm_bench"} line per measurement:
//
//   mc        M9's Monte Carlo kernel, native and interpreted, the same samples: time, and whether the
//             answers are identical (they must be, or the comparison means nothing)
//   load      heap taken by loading the kernel's module (environment, runtime, compiled code)
//   spin      a guest that never returns: how long until the fuel runs out, and that it does
//   oob       a load past the end of the guest's memory: a trap, and nothing read
//   grow      memory.grow up to the limit and past it
//   deep      endless recursion: a trap, before the task's stack is gone
//   pin       a module that imports a pin: refused at load
//   logger    the one import provided: called
//
// Each case runs in a fresh task with a 16 KB stack, so its stack figure is its own high-water mark.

#include "sdkconfig.h"
#if CONFIG_POT_WASM_BENCH

#include "wasm_bench.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pot/mc.hpp"
#include "pot/wasm_modules.h"
#include "pot/wasm_sandbox.hpp"
#include "sdkconfig.h"

namespace {

using namespace pot;
namespace wm = pot::wasm_modules;

constexpr uint32_t kTaskStack = 16384;
constexpr uint32_t kGuestStack = 12 * 1024;  // of the 16 KB: what the guest's calls may use below call()
int g_failures = 0;

int64_t now_us() { return esp_timer_get_time(); }
size_t heap_free() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("{\"t\":\"wasm_bench\",\"case\":\"FAIL\",\"what\":\"%s\"}\n", what);
    }
}

const char* err_or_none(const char* e) { return e == nullptr ? "none" : e; }

void case_mc() {
    constexpr uint32_t kSamples = 200000;
    const uint32_t args[3] = {0x1234u, 7u, kSamples};
    const int64_t t0 = now_us();
    const uint32_t native = mc_hits(args[0], args[1], args[2], nullptr, nullptr);
    const int64_t t1 = now_us();

    const size_t before = heap_free();
    WasmSandbox sb;
    const int64_t l0 = now_us();
    const char* why = sb.load(wm::k_mc, sizeof(wm::k_mc), WasmLimits{0, 8 * 1024});
    const int64_t l1 = now_us();
    const size_t after_load = heap_free();
    check(why == nullptr, "mc load");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"load\",\"module_b\":%u,\"heap_b\":%d,\"us\":%lld,\"error\":\"%s\"}\n",
                static_cast<unsigned>(sizeof(wm::k_mc)), static_cast<int>(before - after_load),
                static_cast<long long>(l1 - l0), err_or_none(why));
    if (why != nullptr) return;
    // The first call compiles the function; time the second as well, which is the steady state.
    for (int run = 0; run < 2; ++run) {
        const int64_t w0 = now_us();
        const WasmCallResult r = sb.call("mc_hits", args, 3, 100000000ull, kGuestStack);
        const int64_t w1 = now_us();
        check(r.error == nullptr && r.value == native, "mc answer");
        const double ratio = static_cast<double>(w1 - w0) / static_cast<double>(t1 - t0);
        std::printf("{\"t\":\"wasm_bench\",\"case\":\"mc\",\"run\":%d,\"samples\":%u,\"native_us\":%lld,\"wasm_us\":%lld,"
                    "\"ratio\":%.2f,\"native_hits\":%u,\"wasm_hits\":%u,\"same\":%s,\"fuel\":%llu,\"heap_min_b\":%u,"
                    "\"error\":\"%s\"}\n",
                    run, static_cast<unsigned>(kSamples), static_cast<long long>(t1 - t0),
                    static_cast<long long>(w1 - w0), ratio, static_cast<unsigned>(native),
                    static_cast<unsigned>(r.value), r.value == native ? "true" : "false",
                    static_cast<unsigned long long>(r.fuel_used),
                    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)), err_or_none(r.error));
    }
}

void case_spin() {
    WasmSandbox sb;
    check(sb.load(wm::k_spin, sizeof(wm::k_spin), WasmLimits{0, 4096}) == nullptr, "spin load");
    constexpr uint64_t kFuel = 10000000;
    const int64_t t0 = now_us();
    const WasmCallResult r = sb.call("spin", nullptr, 0, kFuel, kGuestStack);
    const int64_t t1 = now_us();
    check(r.error == pot_wasm_out_of_fuel, "spin out of fuel");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"spin\",\"fuel\":%llu,\"us\":%lld,\"ns_per_unit\":%.1f,\"error\":\"%s\"}\n",
                static_cast<unsigned long long>(r.fuel_used), static_cast<long long>(t1 - t0),
                1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(kFuel), err_or_none(r.error));
}

void case_oob() {
    WasmSandbox sb;
    check(sb.load(wm::k_oob, sizeof(wm::k_oob), WasmLimits{64 * 1024, 4096}) == nullptr, "oob load");
    const WasmCallResult r = sb.call("oob", nullptr, 0, 1000, kGuestStack);
    check(r.error != nullptr && std::strstr(r.error, "out of bounds") != nullptr, "oob trap");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"oob\",\"error\":\"%s\"}\n", err_or_none(r.error));
}

void case_grow() {
    WasmSandbox sb;
    check(sb.load(wm::k_grow, sizeof(wm::k_grow), WasmLimits{2 * 64 * 1024, 4096}) == nullptr, "grow load");
    const uint32_t one = 1;
    const WasmCallResult a = sb.call("grow", &one, 1, 1000, kGuestStack);
    const WasmCallResult b = sb.call("grow", &one, 1, 1000, kGuestStack);
    check(a.error == nullptr && a.value == 1 && b.error == nullptr && b.value == 0xFFFFFFFFu, "grow limit");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"grow\",\"first\":%d,\"second\":%d}\n", static_cast<int>(a.value),
                static_cast<int>(b.value));
}

void case_deep() {
    WasmSandbox sb;
    check(sb.load(wm::k_deep, sizeof(wm::k_deep), WasmLimits{0, 8 * 1024}) == nullptr, "deep load");
    const uint32_t zero = 0;
    const WasmCallResult r = sb.call("deep", &zero, 1, 100000000ull, kGuestStack);
    check(r.error != nullptr && r.error != pot_wasm_out_of_fuel, "deep trap");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"deep\",\"fuel\":%llu,\"error\":\"%s\"}\n",
                static_cast<unsigned long long>(r.fuel_used), err_or_none(r.error));
}

void case_imports() {
    WasmSandbox sb;
    const char* why = sb.load(wm::k_pin, sizeof(wm::k_pin), WasmLimits{0, 4096});
    check(why != nullptr, "pin refused");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"pin\",\"error\":\"%s\"}\n", err_or_none(why));
    WasmSandbox lg;
    check(lg.load(wm::k_logger, sizeof(wm::k_logger), WasmLimits{0, 4096}) == nullptr, "logger load");
    const uint32_t v = 42;
    const WasmCallResult r = lg.call("hello", &v, 1, 1000, kGuestStack);
    check(r.error == nullptr && lg.log_count() == 1 && lg.log_last() == 42, "logger call");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"logger\",\"calls\":%u,\"last\":%u}\n",
                static_cast<unsigned>(lg.log_count()), static_cast<unsigned>(lg.log_last()));
}

struct Job {
    void (*fn)();
    const char* name;
    SemaphoreHandle_t done;
};

void job_task(void* a) {
    Job* j = static_cast<Job*>(a);
    j->fn();
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"stack\",\"of\":\"%s\",\"free_min_b\":%u,\"stack_b\":%u}\n", j->name,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(kTaskStack));
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
}

void run_in_task(void (*fn)(), const char* name) {
    Job j{fn, name, xSemaphoreCreateBinary()};
    xTaskCreatePinnedToCore(job_task, name, kTaskStack, &j, 5, nullptr, 1);
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
}

}  // namespace

int pot_wasm_bench_run() {
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"start\",\"runtime\":\"wasm3 0.9.0 (patched)\",\"cpu_mhz\":%d}\n",
                CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    run_in_task(case_mc, "mc");
    run_in_task(case_spin, "spin");
    run_in_task(case_oob, "oob");
    run_in_task(case_grow, "grow");
    run_in_task(case_deep, "deep");
    run_in_task(case_imports, "imports");
    std::printf("{\"t\":\"wasm_bench\",\"case\":\"done\",\"failures\":%d}\n", g_failures);
    return g_failures;
}

#endif  // CONFIG_POT_WASM_BENCH
