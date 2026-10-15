// M20: a board's position on the floor plan -- the bytes potluck.deploy compiles, read here, and the two entries
// the actor declares and keeps fresh.

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/position.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_t = 0;

std::vector<uint8_t> from_hex(const char* hex) {
    std::vector<uint8_t> v;
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2) {
        v.push_back(static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    }
    return v;
}

// potluck.deploy.compile_image at counter 3: pos_7368 (builtin:position) pinned to B at x 3.5 m, y 12.25 m.
const char* kGolden = "504f5444010100000300000025a1f52be11b9e171400000068730c10666ab25bf96bb25c0000604000004441";

}  // namespace

TEST(position, the_host_compilers_position_declaration_parses_here) {
    const auto img = from_hex(kGolden);
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    CHECK_EQ(static_cast<int>(d.actor_count), 1);
    PositionConfig c{};
    CHECK(position_config(d.actors[0], c));
    CHECK_EQ(c.x_hash, path_hash("potluck://lab/node-7368/sys/pos/x"));
    CHECK_EQ(c.y_hash, path_hash("potluck://lab/node-7368/sys/pos/y"));
    CHECK(c.x == 3.5f && c.y == 12.25f);
    // Out of the plan, or not a number: refused.
    ActorDecl bad = d.actors[0];
    uint8_t cfg[kPositionCfgLen];
    bad.cfg = cfg;
    const float nan = std::nanf("");
    std::memcpy(cfg, d.actors[0].cfg, sizeof cfg);
    std::memcpy(cfg + 8, &nan, 4);
    CHECK(!position_config(bad, c));
    const float far = 1001.0f;
    std::memcpy(cfg, d.actors[0].cfg, sizeof cfg);
    std::memcpy(cfg + 12, &far, 4);
    CHECK(!position_config(bad, c));
}

TEST(position, the_board_holds_its_place_as_two_good_entries_and_keeps_them_fresh) {
    NodeHal hal{};
    hal.now_ms = [](void*) { return g_t; };
    hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
    NodeConfig cfg;
    cfg.node_id = 0x7368;
    cfg.boot_epoch = 1;
    cfg.mac[0] = 0x02;
    Node node(cfg, hal);
    const uint32_t hx = path_hash("potluck://lab/node-7368/sys/pos/x");
    const uint32_t hy = path_hash("potluck://lab/node-7368/sys/pos/y");
    alignas(PositionActor) unsigned char mem[sizeof(PositionActor)];
    auto* a = new (mem) PositionActor(node, PositionConfig{hx, hy, 3.5f, 12.25f});
    g_t = 1000;
    CHECK(a->start(g_t));
    a->tick(g_t);
    Reading r;
    float v = 0;
    CHECK(node.read(hx, r) == NsError::Ok);
    CHECK(r.value.as_f32(v) && v == 3.5f && r.quality == Quality::Good);
    CHECK(node.read(hy, r) == NsError::Ok);
    CHECK(r.value.as_f32(v) && v == 12.25f);
    g_t += 3 * kPositionPeriodMs - 1;  // re-published every period: still GOOD long after the start
    for (uint32_t t = 1000; t <= g_t; t += 1000) a->tick(t);
    CHECK(node.read(hx, r) == NsError::Ok);
    CHECK(r.quality == Quality::Good);
    a->~PositionActor();
}
