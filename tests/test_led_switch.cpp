// M13: B's LED as a writable resource, and the presenter's config -- the bytes potluck.deploy compiles, read here.

#include <cstring>
#include <new>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/led_slots.hpp"
#include "pot/led_switch.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_t = 0;

struct LedSpy {
    int calls = 0;
    bool on = false;
    uint8_t r = 0, g = 0, b = 0;
    int slot = -1;  // the slot of the last led_claim; -1 after led_app
    static void led(void* ctx, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t) {
        LedSpy* s = static_cast<LedSpy*>(ctx);
        ++s->calls;
        s->on = on;
        s->r = r;
        s->g = g;
        s->b = b;
        s->slot = -1;
    }
    static void claim(void* ctx, uint8_t slot, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t p) {
        led(ctx, on, r, g, b, p);
        static_cast<LedSpy*>(ctx)->slot = slot;
    }
};

std::vector<uint8_t> from_hex(const char* hex) {
    std::vector<uint8_t> v;
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2) {
        v.push_back(static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    }
    return v;
}

// host/potluck/tests/test_outside.py's PRESENT_GOLDEN: a present actor (broker 192.168.0.77:1883, keep-alive 10 s,
// every 2 s, eligible A and B) and led_b on B (255, 140, 0, steady), compiled at counter 3.
const char* kGolden =
    "504f54440102000003000000cd8c12d9a4f8758a25000000feff0a159e35af6ed00702006301687301c0a8004d5b070a00"
    "68730b08a41b2e6bff8c0000";

}  // namespace

TEST(led_switch, the_host_compilers_present_and_led_switch_declarations_parse_here) {
    const auto img = from_hex(kGolden);
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    CHECK_EQ(static_cast<int>(d.actor_count), 2);
    PresentConfig pc{};
    CHECK(present_config(d.actors[0], pc));
    CHECK_EQ(pc.place.out_hash, path_hash("potluck://lab/act/present/out"));
    CHECK_EQ(static_cast<int>(pc.place.period_ms), 2000);
    CHECK_EQ(static_cast<int>(pc.place.count), 2);
    CHECK_EQ(static_cast<int>(pc.ip[0]), 192);
    CHECK_EQ(static_cast<int>(pc.ip[3]), 77);
    CHECK_EQ(static_cast<int>(pc.port), 1883);
    CHECK_EQ(static_cast<int>(pc.keepalive_s), 10);
    LedSwitchConfig lc{};
    CHECK(led_switch_config(d.actors[1], lc));
    CHECK_EQ(lc.out_hash, path_hash("potluck://lab/node-7368/led/on"));
    CHECK_EQ(static_cast<int>(lc.g), 140);
}

TEST(led_switch, two_apps_on_one_led_no_longer_take_turns) {
    // The owner's B, 2026-10-18: the LED switch (blue, refreshed every 2 s) and passive-sensor's indicator (off, every
    // window) shared one state, so the switched-on LED went blue and green every few seconds.
    LedSlots s;
    uint32_t t = 1000;
    s.set(kLedSwitchSlot, true, 0, 0, 255, 0, t);  // the switch: on
    for (int i = 0; i < 10; ++i) {
        t += 1000;
        s.set(0, false, 0, 0, 255, 0, t);  // the indicator, slot 0 (led_app): nobody in the room
        if (i % 2 == 0) s.set(kLedSwitchSlot, true, 0, 0, 255, 0, t);  // the switch refreshes every 2 s
        const LedSlots::State* now = s.current(t);
        CHECK(now != nullptr);  // still blue: an "off" in slot 0 does not put out slot 1
    }
    s.set(0, true, 0, 255, 255, 0, t + 1);  // someone walks in: the indicator's cyan, the newest "on", shows
    CHECK_EQ(static_cast<int>(s.current(t + 1)->g), 255);
    s.set(0, false, 0, 0, 0, 0, t + 2);      // they leave: the switch's blue again
    CHECK_EQ(static_cast<int>(s.current(t + 2)->g), 0);
    s.set(kLedSwitchSlot, false, 0, 0, 0, 0, t + 3);  // switched off: green
    CHECK(s.current(t + 3) == nullptr);
    s.set(kLedSwitchSlot, true, 0, 0, 255, 0, t + 4);  // on, then never refreshed: gone 5 s later
    CHECK(s.current(t + 4 + LedSlots::kLapseMs) == nullptr);
}

TEST(led_switch, the_switch_claims_its_own_slot_where_the_board_has_slots) {
    NodeHal hal{};
    hal.now_ms = [](void*) { return g_t; };
    hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
    NodeConfig cfg;
    cfg.node_id = 0x7368;
    cfg.boot_epoch = 1;
    cfg.mac[0] = 0x02;
    Node node(cfg, hal);
    LedSpy spy;
    BoardServices board;
    board.ctx = &spy;
    board.led_app = &LedSpy::led;
    board.led_claim = &LedSpy::claim;
    const uint32_t out = path_hash("potluck://lab/node-7368/led/on");
    alignas(LedSwitchActor) unsigned char mem[sizeof(LedSwitchActor)];
    auto* a = new (mem) LedSwitchActor(node, LedSwitchConfig{out, 0, 0, 255, 0}, &board);
    g_t = 1000;
    CHECK(a->start(g_t));
    CHECK(node.write_local(out, Value::of_bool(true)) == NsError::Ok);
    a->tick(g_t);
    CHECK(spy.on);
    CHECK_EQ(spy.slot, static_cast<int>(kLedSwitchSlot));
    a->~LedSwitchActor();
    CHECK(!spy.on);
    CHECK_EQ(spy.slot, static_cast<int>(kLedSwitchSlot));  // it clears its own slot, nobody else's
}

TEST(led_switch, a_write_lights_the_led_and_the_actor_going_clears_it) {
    NodeHal hal{};
    hal.now_ms = [](void*) { return g_t; };
    hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
    NodeConfig cfg;
    cfg.node_id = 0x7368;
    cfg.boot_epoch = 1;
    cfg.mac[0] = 0x02;
    Node node(cfg, hal);
    LedSpy spy;
    BoardServices board;
    board.ctx = &spy;
    board.led_app = &LedSpy::led;
    const uint32_t out = path_hash("potluck://lab/node-7368/led/on");
    alignas(LedSwitchActor) unsigned char mem[sizeof(LedSwitchActor)];
    auto* a = new (mem) LedSwitchActor(node, LedSwitchConfig{out, 255, 140, 0, 0}, &board);
    g_t = 1000;
    CHECK(a->start(g_t));
    Reading r;
    CHECK(node.read(out, r) == NsError::Ok);
    bool v = true;
    CHECK(r.value.as_bool(v) && !v);  // off until written, and it has a value: GOOD, not NO_DATA
    CHECK(r.quality == Quality::Good);
    // A WRITE from the wire (here, the presenter's path on the owning board): writable from outside only if the
    // package says so -- it does not yet.
    CHECK(node.ns().write_outside(out, Value::of_bool(true), g_t) == NsError::NotOutsideWritable);
    const uint32_t h[] = {out};
    const uint8_t f[] = {kOutsideExposed | kOutsideWritable};
    CHECK(node.ns().set_outside(h, f, 1));
    CHECK(node.ns().write_outside(out, Value::of_bool(true), g_t) == NsError::Ok);
    a->tick(g_t);
    CHECK(spy.on);
    CHECK_EQ(static_cast<int>(spy.g), 140);
    CHECK_EQ(a->changes(), 1u);
    const int before = spy.calls;
    g_t += 2500;  // PS-5's light lapses after 5 s: the actor refreshes it every 2 s
    a->tick(g_t);
    CHECK(spy.calls > before);
    CHECK(node.write_local(out, Value::of_bool(false)) == NsError::Ok);
    a->tick(g_t);
    CHECK(!spy.on);
    CHECK(node.write_local(out, Value::of_i32(1)) == NsError::TypeMismatch);  // a bool, nothing else
    node.write_local(out, Value::of_bool(true));
    a->tick(g_t);
    a->~LedSwitchActor();  // moved away or removed by a package: nothing left lit
    CHECK(!spy.on);
}
