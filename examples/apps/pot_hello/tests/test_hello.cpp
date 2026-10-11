// pot_hello tested with Potluck's test kit: the rig gives the actor a real node, a clock the test moves and
// a captured LED. The declaration is the bytes the host tools compile from examples/apps/hello.json.

#include <cstring>

#include "hello/hello.hpp"
#include "pot/testkit.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {
// node 0xFFFF (every node), type 0xF0, 6 config bytes: system_prefix of potluck://lab/, period_ms 5000.
const uint8_t kDecl[] = {0xff, 0xff, 0xf0, 0x06, 0x27, 0x11, 0x39, 0x61, 0x88, 0x13};
}  // namespace

TEST(hello_kit, it_publishes_once_a_period_and_shows_cyan_alone) {
    testkit::Rig rig;  // node 0x0100
    CHECK(rig.declare_bytes(kDecl, sizeof(kDecl)));
    CHECK(rig.start(hello::peers_row()));
    rig.advance(10);
    const Reading r = rig.read("potluck://lab/node-0100/hello/peers");
    CHECK(r.quality == Quality::Good);
    uint32_t n = 99;
    CHECK(r.value.as_u32(n) && n == 0u);  // the rig has no radio, so no peers
    CHECK(rig.led.on);
    CHECK_EQ(rig.led.calls, 1);
    rig.advance(5000, 10);
    CHECK_EQ(rig.led.calls, 2);  // once per period
}

TEST(hello_kit, a_period_below_its_floor_is_refused_with_a_reason) {
    testkit::Rig rig;
    uint8_t bad[sizeof(kDecl)];
    std::memcpy(bad, kDecl, sizeof(kDecl));
    bad[8] = 0x10, bad[9] = 0x00;  // 16 ms
    CHECK(rig.declare_bytes(bad, sizeof(bad)));
    const char* why = nullptr;
    CHECK(!rig.start(hello::peers_row(), &why));
    CHECK(why != nullptr);
}

TEST(hello_kit, other_nodes_learn_its_output_from_the_row) {
    testkit::Rig rig;
    CHECK(rig.declare_bytes(kDecl, sizeof(kDecl)));
    CHECK(rig.start(hello::peers_row()));
    NsDecl out[kMaxOutputs];
    CHECK_EQ(rig.outputs(out, kMaxOutputs, 0x7368), static_cast<size_t>(1));
    CHECK_EQ(out[0].path_hash, path_hash("potluck://lab/node-7368/hello/peers"));
}
