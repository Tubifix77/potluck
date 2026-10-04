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
#include "pot/sys_resources.hpp"
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
    // Pairs of slots that cannot hear each other. A host is cabled to one board and has no radio,
    // so it never hears the others directly; this is how a test says so.
    std::vector<std::pair<size_t, size_t>> cut;
    // Every REPLY delivered to slot 0, decoded -- what the host would print.
    std::vector<ReplyPayload> replies_to_0;

    bool blocked(size_t a, size_t b) const {
        for (const auto& pr : cut) {
            if ((pr.first == a && pr.second == b) || (pr.first == b && pr.second == a)) return true;
        }
        return false;
    }

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
        bool delivered = false;
        for (size_t i = 0; i < c->slots.size(); ++i) {
            if (i == from->index || c->blocked(from->index, i)) continue;
            if (bcast || std::memcmp(c->slots[i].mac, mac, kMacLen) == 0) {
                if (i == 0) {
                    Frame f;
                    ReplyPayload rp{};
                    if (parse(data, len, f) == FrameError::Ok && f.hdr.opcode == kOpReply &&
                        load_reply(f.payload, f.payload_len, rp)) {
                        c->replies_to_0.push_back(rp);
                    }
                }
                c->slots[i].node->on_rx(from->mac, data, len, c->now_us, -50);
                delivered = true;
            }
        }
        if (!bcast && delivered) from->node->on_tx_done(mac, true, c->now_us);
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
    // The table is bounded, then requests are refused -- an unbounded request table on a link with
    // a 500 ms p99 is a memory leak waiting for a partition. And a request nobody answers must free
    // its slot, or a table full of stuck reads would block every later one.
    //
    // The bound is read from the class rather than typed in. It was typed in once, as 4, and
    // section 7.8's coordinator later needed one slot per worker: the test then failed for the
    // table being *larger*, which is not a defect anybody wanted reported.
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);

    Node& reader = *c.slots[0].node;
    Node& owner = *c.slots[1].node;
    declare_both(c, owner.config().node_id, 5000);

    c.partitioned = true;  // nothing will be answered
    const size_t cap = Node::max_calls_outstanding();
    size_t issued = 0;
    for (size_t i = 0; i < cap + 5; ++i) {
        if (reader.request_read(owner.config().node_id, kAdc0) != 0) ++issued;
    }
    CHECK_EQ(issued, cap);

    c.advance_ms(2500);  // past ns_request_timeout_ms
    CHECK_EQ(reader.ns_counters().read_timeouts, static_cast<uint32_t>(cap));

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

TEST(m1, a_host_on_one_board_reads_another_boards_builtin_resource) {
    // M1's acceptance as it runs on hardware: the host is cabled to A and has no radio, so it can
    // never hear B. It asks A for B's sys/uptime. A has no entry for it -- nobody declared it -- but
    // a peer's built-in paths are derived from its node id, so A adopts the declaration, answers
    // with what it knows, and fetches a replica from B over its own single hop.
    Cell2 c;
    c.build(3);
    c.cut.push_back({0, 2});  // host <-> B: never connected
    c.start();
    c.advance_ms(300);

    Node& host = *c.slots[0].node;
    Node& a = *c.slots[1].node;
    Node& b = *c.slots[2].node;
    const uint16_t b_id = b.config().node_id;
    const uint32_t up = sys_resource_hash(b_id, SysResource::UptimeSeconds);

    CHECK_EQ(declare_sys_resources(b.ns(), b_id), kSysResourceCount);
    CHECK_EQ(static_cast<int>(b.publish(up, Value::of_u32(1234))), static_cast<int>(NsError::Ok));
    CHECK(a.ns().find(up) == nullptr);  // A starts knowing nothing about B's resources

    // First read: A answers honestly that it has no data yet -- not a zero, not silence.
    CHECK(host.request_read(a.config().node_id, up) != 0);
    c.advance_ms(50);
    CHECK_EQ(c.replies_to_0.size(), static_cast<size_t>(1));
    CHECK_EQ(static_cast<int>(c.replies_to_0.back().status), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(c.replies_to_0.back().quality), static_cast<int>(Quality::NoData));
    CHECK(a.ns().find(up) != nullptr);  // adopted on demand
    CHECK_EQ(b.ns_counters().reads_served, 1u);  // and A went to B for it

    // Second read: the replica, with its age and B's value.
    CHECK(host.request_read(a.config().node_id, up) != 0);
    c.advance_ms(50);
    CHECK_EQ(c.replies_to_0.size(), static_cast<size_t>(2));
    const ReplyPayload good = c.replies_to_0.back();
    CHECK_EQ(static_cast<int>(good.quality), static_cast<int>(Quality::Good));
    const Value v = value_from_wire(good.value_type, good.value_len, good.value_raw);
    CHECK_EQ(static_cast<int>(v.type), static_cast<int>(ValueType::U32));
    CHECK(good.age_ms < 1000);

    // Unplug B. A declares it dead after 600 ms; the replica is fresh by its staleness bound, and
    // must not be served as if B were still there.
    c.cut.push_back({1, 2});
    c.advance_ms(700);
    CHECK(a.peers().count_in_state(PeerState::Dead) >= 1u);
    CHECK(host.request_read(a.config().node_id, up) != 0);
    c.advance_ms(50);
    CHECK_EQ(c.replies_to_0.size(), static_cast<size_t>(3));
    CHECK_EQ(static_cast<int>(c.replies_to_0.back().quality), static_cast<int>(Quality::Unavailable));
}

TEST(m1, a_read_for_a_path_no_peer_owns_is_not_adopted) {
    // On-demand adoption must only ever adopt a real peer's built-in path, never invent an entry.
    Cell2 c;
    c.build(2);
    c.start();
    c.advance_ms(300);
    Node& host = *c.slots[0].node;
    Node& a = *c.slots[1].node;
    const uint32_t stranger = sys_resource_hash(0x7777, SysResource::UptimeSeconds);  // no such peer
    CHECK(host.request_read(a.config().node_id, stranger) != 0);
    c.advance_ms(50);
    CHECK(a.ns().find(stranger) == nullptr);
    CHECK_EQ(c.replies_to_0.size(), static_cast<size_t>(1));
    CHECK_EQ(static_cast<int>(c.replies_to_0.back().status), static_cast<int>(NsError::NotFound));
}
