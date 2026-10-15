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

// potluck.deploy.compile_image at counter 3: pos_7368 (builtin:position) pinned to B at x 3.5 m, y 12.25 m, room 2,
// nearest C then A, in cells 1 and 3 (M19); and one with a room and no place.
const char* kGolden =
    "504f54440101000003000000f30f281b5cdd7a4f2a00000068730c26666ab25bf96bb25c5286b12000006040000044410200000027d1e20b"
    "60810063000000000103";
const char* kRoomOnly =
    "504f5444010100000300000057f54b1fcde4d76e2a00000068730c26666ab25bf96bb25c5286b120000080bf000080bf0100000027d1e20b"
    "00000000000000000000";

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
    CHECK_EQ(c.room_hash, path_hash("potluck://lab/node-7368/sys/room"));
    CHECK(c.x == 3.5f && c.y == 12.25f && c.placed());
    CHECK_EQ(c.room, 2u);
    CHECK_EQ(c.near_hash, path_hash("potluck://lab/node-7368/sys/near"));
    CHECK(c.near_count() == 2 && c.near[0] == 0x8160 && c.near[1] == 0x6300);
    CHECK(c.cells[0] == 1 && c.cells[1] == 3);
    NsDecl outs[kMaxOutputs];
    CHECK_EQ(kPositionKind.outputs(d.actors[0], 0x7368, 0, outs, kMaxOutputs), size_t{4});
    CHECK(kPositionKind.outputs_on_demand);  // other boards adopt them when they read them
    // A room and no place: one entry, the room.
    const auto img2 = from_hex(kRoomOnly);
    DeployImage d2{};
    CHECK(parse_image(img2.data(), img2.size(), d2, &why));
    PositionConfig c2{};
    CHECK(position_config(d2.actors[0], c2));
    CHECK(!c2.placed() && c2.room == 1u);
    CHECK_EQ(kPositionKind.outputs(d2.actors[0], 0x7368, 0, outs, kMaxOutputs), size_t{1});
    CHECK_EQ(outs[0].path_hash, path_hash("potluck://lab/node-7368/sys/room"));
    // Out of the plan, or not a number: refused.
    ActorDecl bad = d.actors[0];
    uint8_t cfg[kPositionCfgLen];
    bad.cfg = cfg;
    const float nan = std::nanf("");
    std::memcpy(cfg, d.actors[0].cfg, sizeof cfg);
    std::memcpy(cfg + 12, &nan, 4);
    CHECK(!position_config(bad, c));
    const float far = 1001.0f;
    std::memcpy(cfg, d.actors[0].cfg, sizeof cfg);
    std::memcpy(cfg + 16, &far, 4);
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
    const uint32_t hr = path_hash("potluck://lab/node-7368/sys/room");
    const uint32_t hn = path_hash("potluck://lab/node-7368/sys/near");
    auto* a = new (mem) PositionActor(node, PositionConfig{hx, hy, hr, 3.5f, 12.25f, 2, hn, {0x8160, 0x6300, 0, 0}, {4, 0}});
    g_t = 1000;
    CHECK(a->start(g_t));
    CHECK_EQ(node.groups(), static_cast<uint16_t>(4));  // M19: the board joins its cell when the actor starts
    a->tick(g_t);
    Reading r;
    float v = 0;
    CHECK(node.read(hx, r) == NsError::Ok);
    CHECK(r.value.as_f32(v) && v == 3.5f && r.quality == Quality::Good);
    CHECK(node.read(hy, r) == NsError::Ok);
    CHECK(r.value.as_f32(v) && v == 12.25f);
    uint32_t room = 0;
    CHECK(node.read(hr, r) == NsError::Ok);
    CHECK(r.value.as_u32(room) && room == 2u);
    CHECK(node.read(hn, r) == NsError::Ok);
    CHECK(r.value.type == ValueType::Bytes && r.value.len == 8 && r.value.raw[0] == 0x60 && r.value.raw[1] == 0x81 &&
          r.value.raw[2] == 0x00 && r.value.raw[3] == 0x63 && r.value.raw[4] == 0);
    g_t += 3 * kPositionPeriodMs - 1;  // re-published every period: still GOOD long after the start
    for (uint32_t t = 1000; t <= g_t; t += 1000) a->tick(t);
    CHECK(node.read(hx, r) == NsError::Ok);
    CHECK(r.quality == Quality::Good);
    a->~PositionActor();
    CHECK_EQ(node.groups(), static_cast<uint16_t>(0));  // and leaves it when the package no longer names it
}
