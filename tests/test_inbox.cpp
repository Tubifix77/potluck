// 2026-10-20: an inbox -- values a program outside the cell writes through the hub (pot/inbox.hpp).

#include <cstring>
#include <new>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/inbox.hpp"
#include "pot/namespace.hpp"
#include "pot/node.hpp"
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

// host/potluck/tests/test_inbox.py's GOLDEN: actor "phone" pinned to 0x6300, outputs
// "heart_rate:f32:hertz steps:u32 moving:bool", stale_ms 5000, compiled at counter 7.
const char* kGolden =
    "504f54440101000007000000058a82bf9a634aa21b00000000630e1788130000030834f8bf045a76714e6603008d0bfdbc0100";

}  // namespace

TEST(inbox, the_host_compilers_declaration_parses_here) {
    const auto img = from_hex(kGolden);
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    CHECK_EQ(static_cast<int>(d.actor_count), 1);
    CHECK_EQ(static_cast<int>(d.actors[0].node_id), 0x6300);
    InboxConfig c{};
    CHECK(inbox_config(d.actors[0], c));
    CHECK_EQ(c.stale_ms, 5000u);
    CHECK_EQ(static_cast<int>(c.n), 3);
    CHECK_EQ(c.hash[0], path_hash("potluck://lab/act/phone/heart_rate"));
    CHECK_EQ(static_cast<int>(c.type[0]), static_cast<int>(ValueType::F32));
    CHECK_EQ(static_cast<int>(c.unit[0]), static_cast<int>(Unit::Hertz));
    CHECK_EQ(c.hash[2], path_hash("potluck://lab/act/phone/moving"));
    CHECK_EQ(static_cast<int>(c.type[2]), static_cast<int>(ValueType::Bool));
    NsDecl out[kMaxOutputs];
    CHECK_EQ(kInboxKind.outputs(d.actors[0], 0x6300, 0, out, kMaxOutputs), static_cast<size_t>(3));  // its board's
    CHECK_EQ(static_cast<int>(out[1].access), static_cast<int>(Access::ReadWrite));
    CHECK_EQ(out[1].staleness_bound_ms, 5000u);
    CHECK_EQ(kInboxKind.outputs(d.actors[0], 0x7368, 0, out, kMaxOutputs), static_cast<size_t>(0));  // not another's
}

TEST(inbox, a_program_writes_its_values_and_the_board_holds_them) {
    NodeHal hal{};
    hal.now_ms = [](void*) { return g_t; };
    hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
    NodeConfig cfg;
    cfg.node_id = 0x6300;
    cfg.boot_epoch = 1;
    cfg.mac[0] = 0x02;
    Node node(cfg, hal);
    const auto img = from_hex(kGolden);
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    InboxConfig c{};
    CHECK(inbox_config(d.actors[0], c));
    alignas(InboxActor) unsigned char mem[sizeof(InboxActor)];
    auto* a = new (mem) InboxActor(node, c);
    g_t = 1000;
    CHECK(a->start(g_t));
    const uint32_t hr = path_hash("potluck://lab/act/phone/heart_rate");
    Reading r;
    node.ns().read(hr, g_t, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::NoData));  // nothing written yet
    // The package's outside rule makes it writable from outside (the cluster file's "outside": read_write).
    CHECK_EQ(static_cast<int>(node.ns().write_outside(hr, Value::of_f32(1.2f), g_t)),
             static_cast<int>(NsError::NotOutsideWritable));  // not without the rule
    const uint8_t flags = kOutsideExposed | kOutsideWritable;  // writable implies exposed
    CHECK(node.ns().set_outside(&hr, &flags, 1));
    CHECK_EQ(static_cast<int>(node.ns().write_outside(hr, Value::of_f32(1.2f), g_t)), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(node.ns().write_outside(hr, Value::of_u32(7), g_t)),
             static_cast<int>(NsError::TypeMismatch));  // the declared type, f32, or nothing
    a->tick(g_t);
    node.ns().read(hr, g_t + 100, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    node.ns().read(hr, g_t + 6000, true, r);  // the program stopped writing: past its 5 s bound
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Stale));
    char buf[128];
    CHECK(a->stats_json(buf, sizeof(buf), g_t) > 0);
    CHECK(std::strstr(buf, "\"outputs\":3") != nullptr);
    a->~InboxActor();
}

TEST(inbox, a_bad_declaration_is_refused) {
    uint8_t cfg[11] = {0x88, 0x13, 0, 0, 1, 0x01, 0x02, 0x03, 0x04, 8 /* Bytes: not a scalar */, 0};
    ActorDecl d{0x6300, ActorType::Inbox, sizeof(cfg), cfg};
    InboxConfig c{};
    CHECK(!inbox_config(d, c));
    cfg[9] = static_cast<uint8_t>(ValueType::F32);
    CHECK(inbox_config(d, c));
    const ActorDecl portable{kPortableNode, ActorType::Inbox, sizeof(cfg), cfg};
    const char* why = nullptr;
    CHECK(!kInboxKind.check(portable, ActorEnv{}, &why));  // held by one board: pinned
}
