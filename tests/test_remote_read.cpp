// M1's acceptance test, between two real Nodes over a simulated link.
//
// §13-M1: "`potctl read potluck://lab/n2/adc/0` returns value + unit + age + class. Unplug node 2 —
// the read returns STALE, never a cached number presented as fresh."
//
// The last clause is the one that matters and the one this file is built around. Everything else is
// plumbing; that sentence is the reason the project exists.

#include <cstring>
#include <string>
#include <vector>

#include "pot/node.hpp"
#include "pot/ns_payloads.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

struct Cell2;

struct Slot {
    Cell2* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
};

// Two nodes, a perfect link, a clock the test advances by hand. No loss and no delay: what is under
// test is the *semantics* of a remote read, and a flaky link would make a failure ambiguous.
struct Cell2 {
    std::vector<Slot> slots;
    uint32_t now_us = 0;
    bool partitioned = false;

    void build(size_t n) {
        slots.resize(n);
        for (size_t i = 0; i < n; ++i) {
            Slot& s = slots[i];
            s.cell = this;
            s.index = i;
            s.mac[0] = 0x02;
            s.mac[5] = static_cast<uint8_t>(i);

            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x200 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, s.mac, kMacLen);
            cfg.hello_interval_ms = 500;

            s.hal.ctx = &s;
            s.hal.send = &Cell2::send;
            s.hal.now_ms = &Cell2::now_ms;
            s.hal.now_us = &Cell2::now_us_cb;
            s.node = new Node(cfg, s.hal);
        }
    }
    ~Cell2() {
        for (Slot& s : slots) delete s.node;
    }

    void start() {
        for (Slot& s : slots) s.node->start();
    }
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (Slot& s : slots) s.node->tick(now_us / 1000);
        }
    }

    static uint32_t now_ms(void* c) { return static_cast<Slot*>(c)->cell->now_us / 1000; }
    static uint32_t now_us_cb(void* c) { return static_cast<Slot*>(c)->cell->now_us; }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        Slot* from = static_cast<Slot*>(ctx);
        Cell2* c = from->cell;
        if (c->partitioned) return 0;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->slots.size(); ++i) {
            if (i == from->index) continue;
            if (bcast || std::memcmp(c->slots[i].mac, mac, kMacLen) == 0) {
                c->slots[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast) from->node->on_tx_done(mac, true, c->now_us);
        return 0;
    }
};

const uint32_t kAdc0 = path_hash("potluck://lab/n2/adc/0");

// Declare the resource on its owner, and declare the same path on the reader as a remote cache.
// Both sides know the shape; only the owner produces values.
void declare_both(Cell2& c, uint16_t owner_id, uint32_t bound_ms,
                  StalenessPolicy policy = StalenessPolicy::Informative) {
    NsDecl d;
    d.path_hash = kAdc0;
    d.owner_node = owner_id;
    d.type = ValueType::F32;
    d.unit = Unit::Volt;
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;
    d.latency_class = kClassL3;
    d.staleness_bound_ms = bound_ms;
    d.staleness_policy = policy;
    for (Slot& s : c.slots) {
        CHECK_EQ(static_cast<int>(s.node->ns().declare(d)), static_cast<int>(NsError::Ok));
    }
}

}  // namespace

TEST(m1, one_remote_read) {
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);  // discovery

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, /*bound_ms=*/1000);

    CHECK_EQ(static_cast<int>(owner.write_local(kAdc0, Value::of_f32(3.3f))),
             static_cast<int>(NsError::Ok));

    // Before asking, the reader has the declaration but no data — and says so, rather than
    // inventing a zero.
    Reading r;
    bool local = true;
    CHECK_EQ(static_cast<int>(reader.read(kAdc0, r, &local)), static_cast<int>(NsError::Ok));
    CHECK(!local);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::NoData));

    CHECK(reader.request_read(owner.config().node_id, kAdc0) != 0);
    c.advance_ms(50);

    // §13-M1: "returns value + unit + age + class".
    CHECK_EQ(static_cast<int>(reader.read(kAdc0, r, &local)), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    float v = 0;
    Quality q = Quality::NoData;
    CHECK(r.get_f32(v, q));
    CHECK(v > 3.29f && v < 3.31f);
    CHECK_EQ(static_cast<int>(r.unit), static_cast<int>(Unit::Volt));
    CHECK_EQ(static_cast<uint32_t>(r.latency_class), static_cast<uint32_t>(kClassL3));
    CHECK(r.age_ms <= 50);

    CHECK_EQ(reader.ns_counters().replies_matched, 1u);
    CHECK_EQ(owner.ns_counters().reads_served, 1u);
}

TEST(m1, unplug_the_owner_and_the_read_stops_claiming_freshness) {
    // The acceptance sentence, entire: "Unplug node 2 — the read returns STALE, never a cached
    // number presented as fresh."
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    // A generous bound, so that if the read still claimed Good it would be the *cache* talking and
    // not the staleness rule — which is exactly the failure being guarded against.
    declare_both(c, owner.config().node_id, /*bound_ms=*/60000);

    owner.write_local(kAdc0, Value::of_f32(1.25f));
    reader.request_read(owner.config().node_id, kAdc0);
    c.advance_ms(50);

    Reading r;
    reader.read(kAdc0, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));

    // Unplug it. §8.2 declares the peer dead after 600 ms of silence.
    c.partitioned = true;
    c.advance_ms(700);
    CHECK_EQ(reader.peers().count_in_state(PeerState::Dead), static_cast<size_t>(1));

    reader.read(kAdc0, r);
    // The cached value is 750 ms old against a 60-second bound, so a naive implementation would
    // happily call it Good. The owner being dead outranks that.
    CHECK(r.age_ms < 60000);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Unavailable));
    CHECK(!r.usable());
    CHECK_EQ(static_cast<int>(r.value.type), static_cast<int>(ValueType::None));

    // And nothing anywhere hands back the old number.
    float v = -1;
    Quality q = Quality::Good;
    CHECK(!r.get_f32(v, q));

    char buf[128];
    r.format(buf, sizeof(buf));
    CHECK(std::string(buf).find("UNAVAILABLE") != std::string::npos);
    CHECK(std::string(buf).find("1.25") == std::string::npos);

    // Plug it back in: the read recovers.
    c.partitioned = false;
    c.advance_ms(400);
    reader.request_read(owner.config().node_id, kAdc0);
    c.advance_ms(50);
    reader.read(kAdc0, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
}

TEST(m1, a_stale_cache_is_marked_stale_not_refused) {
    // The other half of §4 rule 2: the owner is alive, the value is simply old. It is still
    // delivered, with its exact age, because an estimator wants it.
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, /*bound_ms=*/200);

    owner.write_local(kAdc0, Value::of_f32(7.5f));
    reader.request_read(owner.config().node_id, kAdc0);
    c.advance_ms(50);

    Reading r;
    reader.read(kAdc0, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));

    c.advance_ms(400);  // past the bound, owner still alive and beaconing
    reader.read(kAdc0, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Stale));
    float v = 0;
    Quality q = Quality::NoData;
    CHECK(r.get_f32(v, q));  // still delivered
    CHECK(v > 7.49f && v < 7.51f);
    CHECK(r.age_ms >= 400);
}

TEST(m1, a_strict_resource_sends_no_number_over_the_wire_when_stale) {
    // Not merely "the reader hides it" — the owner must not put the byte on the wire at all.
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, /*bound_ms=*/100, StalenessPolicy::Strict);

    owner.write_local(kAdc0, Value::of_f32(42.0f));
    c.advance_ms(400);  // let it go stale on the owner

    reader.request_read(owner.config().node_id, kAdc0);
    c.advance_ms(50);

    // The reply carried no value, so the reader cached nothing.
    Reading r;
    reader.read(kAdc0, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::NoData));
    CHECK_EQ(reader.ns_counters().replies_matched, 1u);
}

TEST(m1, a_remote_write_lands_and_is_acknowledged) {
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& writer = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, /*bound_ms=*/5000);

    CHECK(writer.request_write(owner.config().node_id, kAdc0, Value::of_f32(2.5f)) != 0);
    c.advance_ms(50);

    CHECK_EQ(owner.ns_counters().writes_served, 1u);
    Reading r;
    owner.read(kAdc0, r);
    float v = 0;
    Quality q = Quality::NoData;
    CHECK(r.get_f32(v, q));
    CHECK(v > 2.49f && v < 2.51f);
    CHECK_EQ(static_cast<int>(q), static_cast<int>(Quality::Good));
}

TEST(m1, a_write_of_the_wrong_type_is_refused_and_the_refusal_comes_back) {
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& writer = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, /*bound_ms=*/5000);  // declared F32

    CHECK(writer.request_write(owner.config().node_id, kAdc0, Value::of_i32(9)) != 0);
    c.advance_ms(50);

    CHECK_EQ(owner.ns_counters().writes_rejected, 1u);
    CHECK_EQ(owner.ns_counters().writes_served, 0u);
    // §5.2: never silently dropped. The writer got an answer, it just was not a success.
    CHECK_EQ(writer.ns_counters().replies_matched, 1u);
}

TEST(m1, a_read_of_an_unknown_path_is_answered_not_ignored) {
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;

    CHECK(reader.request_read(owner.config().node_id, path_hash("potluck://lab/n2/nope")) != 0);
    c.advance_ms(50);
    // The owner served a reply carrying NotFound rather than dropping the request, so the reader's
    // pending slot is freed rather than waiting out its timeout.
    CHECK_EQ(reader.ns_counters().replies_matched, 1u);
    CHECK_EQ(reader.ns_counters().read_timeouts, 0u);
}

TEST(m1, outstanding_requests_are_bounded_and_expire) {
    // Four slots, then refusal — an unbounded request table on a link with a 500 ms p99 is a memory
    // leak waiting for a partition. And a request nobody answers must free its slot, or four stuck
    // reads would block every later one.
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, 5000);

    c.partitioned = true;  // nothing will be answered
    size_t issued = 0;
    for (int i = 0; i < 10; ++i) {
        if (reader.request_read(owner.config().node_id, kAdc0) != 0) ++issued;
    }
    CHECK_EQ(issued, static_cast<size_t>(4));

    c.advance_ms(2500);  // past ns_request_timeout_ms
    CHECK_EQ(reader.ns_counters().read_timeouts, 4u);

    // Slots are free again.
    c.partitioned = false;
    c.advance_ms(1000);
    CHECK(reader.request_read(owner.config().node_id, kAdc0) != 0);
}

TEST(m1, the_reply_payload_round_trips_the_whole_tuple) {
    // §4 rule 2's tuple must survive the wire intact — a reply that drops the age would make every
    // remote read a silent stale read.
    Reading in;
    in.value = Value::of_f32(-12.5f);
    in.unit = Unit::Celsius;
    in.timestamp_ms = 123456;
    in.age_ms = 789;
    in.latency_class = kClassL2;
    in.quality = Quality::Stale;

    ReplyPayload p{};
    reply_from_reading(p, 0xAABBCCDD, in, NsError::Ok);
    CHECK_EQ(p.path_hash, 0xAABBCCDDu);
    CHECK_EQ(static_cast<int>(p.reply_to), static_cast<int>(kOpRead));

    const Reading out = reading_from_reply(p);
    CHECK_EQ(out.timestamp_ms, in.timestamp_ms);
    CHECK_EQ(out.age_ms, in.age_ms);
    CHECK_EQ(static_cast<int>(out.unit), static_cast<int>(in.unit));
    CHECK_EQ(static_cast<uint32_t>(out.latency_class), static_cast<uint32_t>(in.latency_class));
    CHECK_EQ(static_cast<int>(out.quality), static_cast<int>(Quality::Stale));
    float v = 0;
    Quality q = Quality::NoData;
    CHECK(out.get_f32(v, q));
    CHECK(v < -12.49f && v > -12.51f);

    // And an unusable reading carries no value byte at all.
    Reading dead;
    dead.quality = Quality::Unavailable;
    dead.value = Value::of_f32(999.0f);  // even if the caller left one in the struct
    ReplyPayload p2{};
    reply_from_reading(p2, 1, dead, NsError::Ok);
    CHECK_EQ(static_cast<int>(p2.value_type), static_cast<int>(ValueType::None));
    for (size_t i = 0; i < kValueBytesMax; ++i) {
        CHECK_EQ(p2.value_raw[i], static_cast<uint8_t>(0));
    }
}
