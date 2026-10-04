// §5.3.1, the CAN profile -- M4. Single frames, segmentation, and the two properties M4 rests on:
// SAFE_STATE wins arbitration by construction, and every message decodes byte-identical.

#include <cstring>
#include <vector>

#include "pot/can_profile.hpp"
#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

std::vector<uint8_t> frame(const EncodeSpec& spec, const std::vector<uint8_t>& payload) {
    uint8_t buf[512];
    size_t n = 0;
    CHECK(encode(spec, payload.data(), static_cast<uint16_t>(payload.size()), buf, sizeof(buf), n) ==
          FrameError::Ok);
    return std::vector<uint8_t>(buf, buf + n);
}

std::vector<uint8_t> beacon(uint16_t node, uint16_t seq, uint32_t epoch) {
    // Exactly as Node::send_beacon encodes it: class L3, priority 1, seq 0, msg_id 0, broadcast.
    EncodeSpec s;
    s.src = node;
    s.dst = kNodeBroadcast;
    s.opcode = kOpHeartbeat;
    s.lclass = kClassL3;
    s.priority = 1;
    std::vector<uint8_t> p{static_cast<uint8_t>(node), static_cast<uint8_t>(node >> 8),
                           static_cast<uint8_t>(seq), static_cast<uint8_t>(seq >> 8),
                           static_cast<uint8_t>(epoch), static_cast<uint8_t>(epoch >> 8),
                           static_cast<uint8_t>(epoch >> 16), static_cast<uint8_t>(epoch >> 24)};
    return frame(s, p);
}

std::vector<uint8_t> roundtrip(const std::vector<uint8_t>& raw, uint16_t src, CanReassembler& rx,
                               size_t* frames_used = nullptr, uint8_t dst_alias = kCanBroadcastAlias) {
    CanFrame cf[kCanMaxSegments];
    const size_t n = can_encode(raw.data(), raw.size(), can_alias_of(src), dst_alias, cf, kCanMaxSegments);
    if (frames_used != nullptr) *frames_used = n;
    uint8_t out[kCanMaxMessage];
    std::vector<uint8_t> got;
    for (size_t i = 0; i < n; ++i) {
        const size_t m = rx.feed(cf[i], 0, out, sizeof(out));
        if (m > 0) got.assign(out, out + m);
    }
    return got;
}

}  // namespace

TEST(can, the_id_round_trips) {
    CanId id{17, 0x21, 0x2A, 0x3F, false, 0x9};
    const CanId back = can_id_unpack(can_id_pack(id));
    CHECK_EQ(static_cast<int>(back.priority), 17);
    CHECK_EQ(static_cast<int>(back.opcode), 0x21);
    CHECK_EQ(static_cast<int>(back.src), 0x2A);
    CHECK_EQ(static_cast<int>(back.dst), 0x3F);
    CHECK(!back.single);
    CHECK_EQ(static_cast<int>(back.index), 9);
    CHECK(can_id_pack(id) < (1u << 29));
}

TEST(can, safe_state_has_the_lowest_id_so_it_wins_arbitration) {
    // CAN arbitration: the numerically lowest ID wins. SAFE_STATE at Potluck priority 31 must beat
    // anything else on the bus, from any sender, whatever its opcode or aliases.
    const uint32_t safe = can_id_pack({31, kOpSafeState, 62, kCanBroadcastAlias, true, 0});
    for (uint8_t pri = 0; pri < 31; ++pri) {
        for (uint8_t op : {static_cast<uint8_t>(0x01), static_cast<uint8_t>(0x03), static_cast<uint8_t>(0x22)}) {
            const uint32_t other = can_id_pack({pri, op, 0, 0, false, 0});
            CHECK(safe < other);
        }
    }
}

TEST(can, a_beacon_is_one_frame_and_decodes_byte_identical) {
    CanReassembler rx(can_alias_of(0x8160));
    const auto raw = beacon(0x6300, 7, 21);
    size_t used = 0;
    const auto got = roundtrip(raw, 0x6300, rx, &used);
    CHECK_EQ(used, static_cast<size_t>(1));
    CHECK(got == raw);
    uint16_t learned = 0;
    CHECK(rx.aliases().lookup(can_alias_of(0x6300), learned));
    CHECK_EQ(learned, static_cast<uint16_t>(0x6300));
}

TEST(can, safe_state_is_one_frame_and_needs_a_known_sender) {
    CanReassembler rx(can_alias_of(0x7368));
    EncodeSpec s;
    s.src = 0x8160;
    s.opcode = kOpSafeState;
    s.lclass = kClassL0;
    s.priority = 31;
    const auto raw = frame(s, {0x01, 0x02});
    // Before any beacon from 0x8160, its alias means nothing: refused and counted, never guessed.
    CHECK(roundtrip(raw, 0x8160, rx).empty());
    CHECK_EQ(rx.counters().unknown_alias, 1u);
    roundtrip(beacon(0x8160, 1, 3), 0x8160, rx);
    size_t used = 0;
    CHECK(roundtrip(raw, 0x8160, rx, &used) == raw);
    CHECK_EQ(used, static_cast<size_t>(1));
}

TEST(can, frames_that_do_not_qualify_for_one_frame_are_segmented_and_survive) {
    CanReassembler rx(can_alias_of(0x7368));
    // A 48-byte probe: unicast, ACKREQ, msg_id. 64 bytes encoded -> 10 CAN frames (§3.1's count).
    EncodeSpec s;
    s.src = 0x6300;
    s.dst = 0x7368;
    s.opcode = kOpHeartbeat;
    s.priority = 1;
    s.seq = 41;
    s.msg_id = 9;
    s.ack_req = true;
    std::vector<uint8_t> p(48);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>(i * 7);
    const auto raw = frame(s, p);
    CHECK_EQ(raw.size(), static_cast<size_t>(64));
    size_t used = 0;
    CHECK(roundtrip(raw, 0x6300, rx, &used, can_alias_of(0x7368)) == raw);
    CHECK_EQ(used, static_cast<size_t>(10));

    // A beacon with a nonzero seq has something the ID cannot carry, so it segments too.
    EncodeSpec b;
    b.src = 0x6300;
    b.opcode = kOpHeartbeat;
    b.priority = 1;
    b.seq = 5;
    const auto odd = frame(b, std::vector<uint8_t>(8, 0));
    CHECK(roundtrip(odd, 0x6300, rx, &used) == odd);
    CHECK_EQ(used, static_cast<size_t>(4));  // 24 bytes / 7
}

TEST(can, a_lost_or_reordered_segment_drops_the_message_rather_than_misassembling_it) {
    CanReassembler rx(can_alias_of(0x7368));
    EncodeSpec s;
    s.src = 0x6300;
    s.opcode = kOpCast;
    s.msg_id = 3;
    const auto raw = frame(s, std::vector<uint8_t>(40, 0xAB));
    CanFrame cf[kCanMaxSegments];
    const size_t n = can_encode(raw.data(), raw.size(), can_alias_of(0x6300), kCanBroadcastAlias, cf, kCanMaxSegments);
    CHECK(n >= 3);
    uint8_t out[kCanMaxMessage];
    size_t got = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == 1) continue;  // lose the second segment
        got += rx.feed(cf[i], 0, out, sizeof(out));
    }
    CHECK_EQ(got, static_cast<size_t>(0));
    CHECK(rx.counters().out_of_order >= 1u);
    // The next complete message from the same sender still goes through.
    for (size_t i = 0; i < n; ++i) got = rx.feed(cf[i], 0, out, sizeof(out));
    CHECK_EQ(got, raw.size());
}

TEST(can, an_unfinished_message_times_out) {
    CanReassembler rx(can_alias_of(0x7368));
    EncodeSpec s;
    s.src = 0x6300;
    s.opcode = kOpCast;
    s.msg_id = 3;
    const auto raw = frame(s, std::vector<uint8_t>(40, 1));
    CanFrame cf[kCanMaxSegments];
    can_encode(raw.data(), raw.size(), can_alias_of(0x6300), kCanBroadcastAlias, cf, kCanMaxSegments);
    uint8_t out[kCanMaxMessage];
    rx.feed(cf[0], 0, out, sizeof(out));
    rx.expire(kCanReassemblyTimeoutMs + 1);
    CHECK_EQ(rx.counters().timeouts, 1u);
}

TEST(can, frames_for_another_node_are_ignored) {
    CanReassembler rx(can_alias_of(0x7368));
    roundtrip(beacon(0x6300, 1, 1), 0x6300, rx);
    EncodeSpec s;
    s.src = 0x6300;
    s.dst = 0x8160;
    s.opcode = kOpRead;
    s.msg_id = 1;
    const auto raw = frame(s, {1, 2, 3, 4});
    CHECK(roundtrip(raw, 0x6300, rx, nullptr, can_alias_of(0x8160)).empty());
    CHECK(rx.counters().not_for_us >= 1u);
}

TEST(can, a_beacon_that_contradicts_its_own_alias_is_refused) {
    CanReassembler rx(can_alias_of(0x7368));
    const auto raw = beacon(0x6300, 1, 1);
    CanFrame cf[1];
    can_encode(raw.data(), raw.size(), can_alias_of(0x8160), kCanBroadcastAlias, cf, 1);  // wrong alias
    uint8_t out[64];
    CHECK_EQ(rx.feed(cf[0], 0, out, sizeof(out)), static_cast<size_t>(0));
    CHECK_EQ(rx.counters().malformed, 1u);
}

TEST(can, the_448_byte_cap_holds) {
    std::vector<uint8_t> raw(kCanMaxMessage + 1, 0);
    CanFrame cf[kCanMaxSegments + 1];
    CHECK_EQ(can_encode(raw.data(), raw.size(), 1, 2, cf, kCanMaxSegments + 1), static_cast<size_t>(0));
}

TEST(can, aliases_are_never_the_broadcast_alias_and_our_three_boards_do_not_collide) {
    for (uint32_t n = 0; n <= 0xFFFF; ++n) {
        CHECK(can_alias_of(static_cast<uint16_t>(n)) != kCanBroadcastAlias);
    }
    const uint8_t a = can_alias_of(0x6300), b = can_alias_of(0x7368), c = can_alias_of(0x8160);
    CHECK(a != b && b != c && a != c);
}

// ---- two real Nodes on a simulated CAN bus ----------------------------------------------------------

#include "pot/node.hpp"
#include "pot/payloads.hpp"
#include "pot/sys_resources.hpp"

namespace {

struct Bus;
struct BusNode {
    Bus* bus = nullptr;
    size_t index = 0;
    uint16_t id = 0;
    uint8_t mac[6] = {};
    Node* node = nullptr;
    CanReassembler* rx = nullptr;
    NodeHal hal{};
    std::vector<std::pair<uint32_t, uint16_t>> safe_states;  // (counter, reason)
};

// Every CAN frame reaches every other node, as on a real bus; the codec does the rest.
struct Bus {
    std::vector<BusNode> nodes;
    uint32_t now_us = 0;
    size_t can_frames = 0;

    void build(const std::vector<uint16_t>& ids) {
        nodes.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            BusNode& b = nodes[i];
            b.bus = this;
            b.index = i;
            b.id = ids[i];
            can_mac_of(b.id, b.mac);
            b.rx = new CanReassembler(can_alias_of(b.id));
            NodeConfig cfg;
            cfg.node_id = b.id;
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, b.mac, 6);
            cfg.admit_on_beacon = true;
            cfg.hb_period_ms = 20;  // §8.2's wired row
            cfg.hb_miss_limit = 3;
            b.hal.ctx = &b;
            b.hal.send = &Bus::send;
            b.hal.now_ms = [](void* c) { return static_cast<BusNode*>(c)->bus->now_us / 1000; };
            b.hal.now_us = [](void* c) { return static_cast<BusNode*>(c)->bus->now_us; };
            b.node = new Node(cfg, b.hal);
            b.node->set_safe_state_handler(
                [](void* c, uint16_t, uint32_t counter, uint16_t reason) {
                    static_cast<BusNode*>(c)->safe_states.push_back({counter, reason});
                },
                &b);
        }
    }
    ~Bus() {
        for (BusNode& b : nodes) {
            delete b.node;
            delete b.rx;
        }
    }
    void start() {
        for (BusNode& b : nodes) b.node->start();
    }
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (BusNode& b : nodes) b.node->tick(now_us / 1000);
        }
    }
    static int32_t send(void* ctx, const uint8_t mac[6], const uint8_t* data, size_t len) {
        BusNode* from = static_cast<BusNode*>(ctx);
        Bus* bus = from->bus;
        uint16_t dst_node = 0;
        const uint8_t dst_alias = can_mac_node(mac, dst_node) ? can_alias_of(dst_node) : kCanBroadcastAlias;
        CanFrame cf[kCanMaxSegments];
        const size_t n = can_encode(data, len, can_alias_of(from->id), dst_alias, cf, kCanMaxSegments);
        if (n == 0) return -1;
        bus->can_frames += n;
        uint8_t out[kCanMaxMessage];
        for (size_t i = 0; i < n; ++i) {
            for (BusNode& b : bus->nodes) {
                if (&b == from) continue;
                const size_t m = b.rx->feed(cf[i], bus->now_us / 1000, out, sizeof(out));
                if (m > 0) {
                    Frame f;
                    if (parse(out, m, f) != FrameError::Ok) continue;
                    uint8_t src_mac[6];
                    can_mac_of(f.hdr.src, src_mac);
                    b.node->on_rx(src_mac, out, m, bus->now_us, 0);
                }
            }
        }
        if (dst_alias != kCanBroadcastAlias) from->node->on_tx_done(mac, true, bus->now_us);
        return 0;
    }
};

}  // namespace

TEST(can, two_nodes_admit_each_other_from_beacons_alone_and_stay_alive) {
    Bus bus;
    bus.build({0x7368, 0x8160});
    bus.start();
    bus.advance_ms(100);
    for (BusNode& b : bus.nodes) {
        CHECK_EQ(b.node->peers().count_in_state(PeerState::Alive), static_cast<size_t>(1));
    }
    bus.advance_ms(3000);  // 150 wired heartbeat periods: beacons alone keep them alive
    for (BusNode& b : bus.nodes) {
        CHECK_EQ(b.node->peers().count_in_state(PeerState::Alive), static_cast<size_t>(1));
        CHECK_EQ(b.node->peers().count_in_state(PeerState::Dead), static_cast<size_t>(0));
        CHECK_EQ(b.rx->counters().unknown_alias, 0u);
    }
}

TEST(can, safe_state_reaches_every_node_once_in_one_can_frame) {
    Bus bus;
    bus.build({0x6300, 0x7368, 0x8160});
    bus.start();
    bus.advance_ms(100);
    const size_t before = bus.can_frames;
    CHECK_EQ(bus.nodes[2].node->send_safe_state(7), 1u);
    CHECK_EQ(bus.can_frames - before, static_cast<size_t>(1));
    for (size_t i = 0; i < 2; ++i) {
        CHECK_EQ(bus.nodes[i].safe_states.size(), static_cast<size_t>(1));
        CHECK_EQ(bus.nodes[i].safe_states[0].first, 1u);
        CHECK_EQ(static_cast<int>(bus.nodes[i].safe_states[0].second), 7);
    }
    CHECK(bus.nodes[2].safe_states.empty());  // a sender does not hear itself
}

TEST(can, a_remote_read_works_over_segmented_can) {
    Bus bus;
    bus.build({0x7368, 0x8160});
    bus.start();
    bus.advance_ms(100);
    Node& reader = *bus.nodes[0].node;
    Node& owner = *bus.nodes[1].node;
    const uint16_t oid = owner.config().node_id;
    declare_sys_resources(owner.ns(), oid);
    declare_sys_resources(reader.ns(), oid);
    const uint32_t up = sys_resource_hash(oid, SysResource::UptimeSeconds);
    CHECK_EQ(static_cast<int>(owner.publish(up, Value::of_u32(42))), static_cast<int>(NsError::Ok));
    CHECK(reader.request_read(oid, up) != 0);
    bus.advance_ms(20);
    Reading r;
    CHECK_EQ(static_cast<int>(reader.read(up, r)), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    CHECK_EQ(reader.ns_counters().replies_matched, 1u);
}

TEST(can, without_admit_on_beacon_a_beacon_admits_nobody) {
    // The radio's rule stays the radio's: there, only HELLO admits, because it pins the version.
    CanReassembler rx(can_alias_of(0x7368));
    NodeConfig cfg;
    cfg.node_id = 0x7368;
    NodeHal hal{};
    Node n(cfg, hal);
    const auto raw = beacon(0x8160, 1, 1);
    uint8_t mac[6];
    can_mac_of(0x8160, mac);
    n.on_rx(mac, raw.data(), raw.size(), 0, 0);
    CHECK_EQ(n.peers().count_in_state(PeerState::Free), PeerTable::capacity());
}

// ---- Transmit order: found on the two-board bench, where a SAFE_STATE queued behind its own node's
// ---- 10-frame probe took 2.5-5.5 ms to leave under flood. The driver sends in queue order.

namespace {

std::vector<CanFrame> to_can(const std::vector<uint8_t>& raw, uint16_t src, uint8_t dst_alias) {
    CanFrame cf[kCanMaxSegments];
    const size_t n = can_encode(raw.data(), raw.size(), can_alias_of(src), dst_alias, cf, kCanMaxSegments);
    CHECK(n > 0);
    return std::vector<CanFrame>(cf, cf + n);
}

std::vector<uint8_t> probe(uint16_t src, uint16_t dst, uint8_t priority, uint8_t fill) {
    EncodeSpec s;
    s.src = src;
    s.dst = dst;
    s.opcode = kOpHeartbeat;
    s.priority = priority;
    s.seq = 41;
    s.msg_id = 9;
    s.ack_req = true;
    return frame(s, std::vector<uint8_t>(48, fill));  // 64 bytes encoded: 10 CAN frames
}

std::vector<uint8_t> safe_state(uint16_t src) {
    EncodeSpec s;
    s.src = src;
    s.opcode = kOpSafeState;
    s.lclass = kClassL0;
    s.priority = 31;
    return frame(s, {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
}

bool push(CanTxQueue& q, const std::vector<CanFrame>& f) { return q.push(f.data(), f.size(), CanTxMeta{}); }

}  // namespace

TEST(can, safe_state_overtakes_its_own_nodes_queued_probe) {
    CanTxQueue q;
    const auto pr = to_can(probe(0x8160, 0x7368, 1, 0x11), 0x8160, can_alias_of(0x7368));
    CHECK_EQ(pr.size(), static_cast<size_t>(10));
    CHECK(push(q, pr));
    CanTxItem it{};
    CHECK(q.pop(it));  // the probe's first segment is in the controller
    CHECK_EQ(it.f.id, pr[0].id);
    CHECK(push(q, to_can(safe_state(0x8160), 0x8160, kCanBroadcastAlias)));
    CHECK(q.pop(it));  // ... and SAFE_STATE is next, not eleventh
    CHECK_EQ(static_cast<int>(can_id_unpack(it.f.id).opcode), static_cast<int>(kOpSafeState));
    CHECK(it.last);
    for (size_t i = 1; i < pr.size(); ++i) {
        CHECK(q.pop(it));
        CHECK(std::memcmp(it.f.data, pr[i].data, pr[i].dlc) == 0);
        CHECK_EQ(it.last, i + 1 == pr.size());
    }
    CHECK(!q.pop(it));
}

TEST(can, a_started_segmented_message_finishes_before_a_higher_priority_one_starts) {
    // A receiver reassembles one message per sender, so interleaving two would lose both. A single
    // frame may cut in; another segmented message may not, whatever its priority.
    CanTxQueue q;
    const auto low = probe(0x8160, 0x7368, 1, 0x22);
    const auto high = probe(0x8160, 0x7368, 20, 0x33);
    CHECK(push(q, to_can(low, 0x8160, can_alias_of(0x7368))));
    CanTxItem it{};
    CHECK(q.pop(it));
    CHECK(push(q, to_can(high, 0x8160, can_alias_of(0x7368))));

    CanReassembler rx(can_alias_of(0x7368));
    uint8_t out[kCanMaxMessage];
    std::vector<std::vector<uint8_t>> got;
    size_t m = rx.feed(it.f, 0, out, sizeof(out));
    while (q.pop(it)) {
        m = rx.feed(it.f, 0, out, sizeof(out));
        if (m > 0) got.emplace_back(out, out + m);
    }
    CHECK_EQ(got.size(), static_cast<size_t>(2));
    CHECK(got[0] == low);
    CHECK(got[1] == high);
    CHECK_EQ(rx.counters().out_of_order, 0u);
}

TEST(can, the_transmit_queue_orders_by_priority_then_fifo_and_takes_all_or_nothing) {
    CanTxQueue q;
    // Before anything is in flight, the higher-priority segmented message goes first.
    const auto a = to_can(probe(0x8160, 0x7368, 1, 0x44), 0x8160, can_alias_of(0x7368));
    const auto b = to_can(probe(0x8160, 0x7368, 5, 0x55), 0x8160, can_alias_of(0x7368));
    const auto c = to_can(probe(0x8160, 0x7368, 5, 0x66), 0x8160, can_alias_of(0x7368));
    CHECK(push(q, a));
    CHECK(push(q, b));
    CHECK(push(q, c));
    CanTxItem it{};
    for (const auto* want : {&b, &c, &a}) {
        for (const CanFrame& f : *want) {
            CHECK(q.pop(it));
            CHECK_EQ(it.f.id, f.id);
            CHECK(std::memcmp(it.f.data, f.data, f.dlc) == 0);
        }
    }
    CHECK(!q.pop(it));

    // Full: a message that does not fit is refused whole, and nothing of it is queued.
    std::vector<CanFrame> big(kCanMaxSegments, a[1]);
    CHECK(push(q, big));
    CHECK_EQ(q.free(), CanTxQueue::kCapacity - kCanMaxSegments);
    CHECK(!push(q, std::vector<CanFrame>(q.free() + 1, a[1])));
    CHECK_EQ(q.free(), CanTxQueue::kCapacity - kCanMaxSegments);
    CHECK(push(q, to_can(safe_state(0x8160), 0x8160, kCanBroadcastAlias)));
}
