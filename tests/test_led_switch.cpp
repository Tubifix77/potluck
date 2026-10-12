// M13: B's LED as a writable resource, and the presenter's config -- the bytes potluck.deploy compiles, read here.

#include <cstring>
#include <new>
#include <vector>

#include "pot/deploy.hpp"
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
    static void led(void* ctx, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t) {
        LedSpy* s = static_cast<LedSpy*>(ctx);
        ++s->calls;
        s->on = on;
        s->r = r;
        s->g = g;
        s->b = b;
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
