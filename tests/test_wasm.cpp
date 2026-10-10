// M7's experiment: the sandbox (pot_wasm) on the host. The same interpreter and the same hand-built
// modules (tools/wasm_modules.py) run on the boards under CONFIG_POT_WASM_BENCH.

#include <cstring>
#include <string>

#include "pot/mc.hpp"
#include "pot/wasm_modules.h"
#include "pot/wasm_sandbox.hpp"
#include "test_harness.hpp"

using namespace pot;
namespace wm = pot::wasm_modules;

namespace {
constexpr uint32_t kStack = 64 * 1024;  // native stack the guest may use below the call
bool contains(const char* s, const char* part) { return s != nullptr && std::strstr(s, part) != nullptr; }
}  // namespace

TEST(wasm, the_monte_carlo_kernel_gives_native_answers) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_mc, sizeof(wm::k_mc), WasmLimits{0, 8 * 1024}) == nullptr);
    const uint32_t cases[][3] = {{1, 0, 2000}, {0xdeadbeef, 7, 5000}, {42, 959, 100}, {0, 0, 1}};
    for (const auto& c : cases) {
        const WasmCallResult r = sb.call("mc_hits", c, 3, 1000000, kStack);
        CHECK(r.error == nullptr);
        CHECK_EQ(r.value, mc_hits(c[0], c[1], c[2], nullptr, nullptr));
        // Metering is exact and deterministic: one unit per loop back-edge, one per sample.
        CHECK_EQ(r.fuel_used, static_cast<uint64_t>(c[2]));
    }
}

TEST(wasm, a_guest_that_never_returns_runs_out_of_fuel) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_spin, sizeof(wm::k_spin), WasmLimits{0, 4096}) == nullptr);
    const WasmCallResult r = sb.call("spin", nullptr, 0, 100000, kStack);
    CHECK(r.error == pot_wasm_out_of_fuel);
    CHECK_EQ(r.fuel_used, 100000u);
    // And the sandbox is still usable after the trap.
    const WasmCallResult again = sb.call("spin", nullptr, 0, 10, kStack);
    CHECK(again.error == pot_wasm_out_of_fuel);
}

TEST(wasm, a_load_past_the_end_of_memory_traps_and_reads_nothing) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_oob, sizeof(wm::k_oob), WasmLimits{64 * 1024, 4096}) == nullptr);
    const WasmCallResult r = sb.call("oob", nullptr, 0, 1000, kStack);
    CHECK(contains(r.error, "out of bounds"));
}

TEST(wasm, memory_cannot_grow_past_the_limit) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_grow, sizeof(wm::k_grow), WasmLimits{2 * 64 * 1024, 4096}) == nullptr);
    const uint32_t one = 1;
    WasmCallResult r = sb.call("grow", &one, 1, 1000, kStack);
    CHECK(r.error == nullptr);
    CHECK_EQ(r.value, 1u);  // the old size, in pages: it grew to two
    r = sb.call("grow", &one, 1, 1000, kStack);
    CHECK(r.error == nullptr);
    CHECK_EQ(r.value, 0xFFFFFFFFu);  // -1: refused, the limit is two pages
}

TEST(wasm, a_module_that_needs_more_memory_than_allowed_is_refused) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_oob, sizeof(wm::k_oob), WasmLimits{0, 4096}) != nullptr);  // it asks for one page
    CHECK(!sb.loaded());
}

TEST(wasm, endless_recursion_traps_instead_of_taking_the_stack) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_deep, sizeof(wm::k_deep), WasmLimits{0, 8 * 1024}) == nullptr);
    const uint32_t zero = 0;
    const WasmCallResult r = sb.call("deep", &zero, 1, 100000000, kStack);
    CHECK(r.error != nullptr);
    CHECK(r.error != pot_wasm_out_of_fuel);  // the stack ran out first, not the fuel
}

TEST(wasm, a_module_importing_what_the_sandbox_does_not_provide_is_refused) {
    WasmSandbox sb;
    const char* why = sb.load(wm::k_pin, sizeof(wm::k_pin), WasmLimits{0, 4096});
    CHECK(contains(why, "imports something"));
    CHECK(!sb.loaded());
}

TEST(wasm, an_imported_memory_or_global_is_refused_too) {
    WasmSandbox sb;
    CHECK(contains(sb.load(wm::k_memimp, sizeof(wm::k_memimp), WasmLimits{64 * 1024, 4096}), "imports something"));
    CHECK(contains(sb.load(wm::k_globimp, sizeof(wm::k_globimp), WasmLimits{0, 4096}), "imports something"));
    CHECK(!sb.loaded());
}

TEST(wasm, the_one_import_provided_works) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_logger, sizeof(wm::k_logger), WasmLimits{0, 4096}) == nullptr);
    const uint32_t v = 42;
    const WasmCallResult r = sb.call("hello", &v, 1, 1000, kStack);
    CHECK(r.error == nullptr);
    CHECK_EQ(sb.log_count(), 1u);
    CHECK_EQ(sb.log_last(), 42u);
}

TEST(wasm, a_malformed_module_is_refused) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_mc, sizeof(wm::k_mc) - 3, WasmLimits{0, 4096}) != nullptr);
    uint8_t bad[sizeof(wm::k_mc)];
    std::memcpy(bad, wm::k_mc, sizeof(bad));
    bad[0] = 0x01;  // not the magic
    CHECK(sb.load(bad, sizeof(bad), WasmLimits{0, 4096}) != nullptr);
    CHECK(!sb.loaded());
}

TEST(wasm, only_small_i32_signatures_are_called) {
    WasmSandbox sb;
    CHECK(sb.load(wm::k_mc, sizeof(wm::k_mc), WasmLimits{0, 4096}) == nullptr);
    const uint32_t a[2] = {1, 2};
    CHECK(sb.call("mc_hits", a, 2, 1000, kStack).error != nullptr);  // three arguments, not two
    CHECK(sb.call("nope", a, 0, 1000, kStack).error != nullptr);
}
