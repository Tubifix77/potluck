// pot::Node tests — the policy that decides how a cell behaves.
//
// A tiny in-test cell: N nodes, perfect instantaneous delivery, a virtual clock. No loss and no
// delay on purpose — sim/ models the channel, and mixing the two would make a failure here
// ambiguous between "the policy is wrong" and "the link was unlucky". What is under test is what
// the node *chooses to send* and *when it declares a peer dead*, which must be exact.

#include <cstring>
#include <vector>

#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/payloads.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

struct TestCell;

struct TestNode {
    TestCell* cell = nullptr;
    size_t index = 0;
    uint8_t chan = 1;  // M5.1: frames reach only nodes tuned to the sender's channel
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
    NodeConfig cfg{};  // kept so a test can reboot the node
    std::vector<RxSample> samples;  // M8.2: what on_rx_sample reported
};

// A cell with a perfect channel and a clock the test advances by hand.
struct TestCell {
    std::vector<TestNode> nodes;
    uint32_t now_us = 0;
    bool partitioned = false;  // when true, nothing is delivered
    bool drop_hello = false;   // when true, HELLOs are lost and everything else is delivered
    // Every HELLO sent: who, on which radio channel, and the channel it declared.
    struct SentHello {
        size_t from;
        uint8_t radio;
        uint8_t declared;
    };
    std::vector<SentHello> hellos;
    bool leak_adjacent = false;  // M5.1: a frame on channel n is also heard on n-1 and n+1 (close range)
    // ADR-009: pairs of nodes out of each other's range, both directions.
    std::vector<std::pair<size_t, size_t>> deaf;
    bool deaf_pair(size_t a, size_t b) const {
        for (const auto& d : deaf) {
            if ((d.first == a && d.second == b) || (d.first == b && d.second == a)) return true;
        }
        return false;
    }
    // Added to every send completion's timestamp. Negative models the radio's callback stamping a
    // completion before the node task submitted the probe it is then credited to.
    int32_t done_skew_us = 0;
    // Signal the send completion before delivering the frame, as a real radio does: the MAC ACK
    // returns before the peer's reply can. Off by default, where the reply arrives first and closes
    // the probe before its completion is seen, so txq is never measured at all.
    bool done_before_delivery = false;
    uint32_t frames_on_wire = 0;

    void build(size_t n, BeaconMode mode, uint32_t probe_ms = 1000) {
        nodes.resize(n);
        for (size_t i = 0; i < n; ++i) {
            TestNode& t = nodes[i];
            t.cell = this;
            t.index = i;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(i);

            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x100 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, t.mac, kMacLen);
            cfg.espnow_version = 2;
            cfg.beacon_mode = mode;
            cfg.probe_interval_ms = probe_ms;
            cfg.hello_interval_ms = 500;

            t.hal.ctx = &t;
            t.hal.send = &TestCell::send;
            t.hal.add_peer = nullptr;
            t.hal.now_ms = &TestCell::now_ms;
            t.hal.now_us = &TestCell::now_us_cb;
            t.hal.set_channel = [](void* ctx, uint8_t ch) { static_cast<TestNode*>(ctx)->chan = ch; };
            t.hal.on_rx_sample = [](void* ctx, const RxSample& rs) { static_cast<TestNode*>(ctx)->samples.push_back(rs); };
            t.cfg = cfg;
            t.node = new Node(cfg, t.hal);
        }
    }

    // A reboot: a fresh Node with the next boot epoch and no memory of anyone.
    void reboot(size_t i) {
        TestNode& t = nodes[i];
        delete t.node;
        ++t.cfg.boot_epoch;
        t.node = new Node(t.cfg, t.hal);
        t.node->start();
    }

    ~TestCell() {
        for (TestNode& t : nodes) delete t.node;
    }

    void start_all() {
        for (TestNode& t : nodes) t.node->start();
    }

    // Advance the clock in 1 ms steps, ticking every node. Stepping rather than jumping matters:
    // the scheduler inside Node works on deadlines, and a jump would collapse many periods into one.
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (TestNode& t : nodes) t.node->tick(now_us / 1000);
        }
    }

    static uint32_t now_ms(void* ctx) { return static_cast<TestNode*>(ctx)->cell->now_us / 1000; }
    static uint32_t now_us_cb(void* ctx) { return static_cast<TestNode*>(ctx)->cell->now_us; }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        TestNode* from = static_cast<TestNode*>(ctx);
        TestCell* c = from->cell;
        ++c->frames_on_wire;
        if (c->partitioned) {
            return 0;  // accepted by the transport, never delivered — a real and important case
        }
        if (len > 6 && data[6] == kOpHello) {
            Frame fr{};
            HelloPayload h{};
            if (parse(data, len, fr) == FrameError::Ok && load_hello(fr.payload, fr.payload_len, h)) {
                c->hellos.push_back({from->index, from->chan,
                                     static_cast<uint8_t>((h.caps & kHelloCapChannelMask) >> kHelloCapChannelShift)});
            }
        }
        if (c->drop_hello && len > 6 && data[6] == kOpHello) {
            return 0;
        }
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        if (!bcast && c->done_before_delivery) {
            from->node->on_tx_done(mac, true, c->now_us + static_cast<uint32_t>(c->done_skew_us));
        }
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            if (i == from->index) continue;
            const int dch = static_cast<int>(c->nodes[i].chan) - static_cast<int>(from->chan);
            if (dch != 0 && !(c->leak_adjacent && (dch == 1 || dch == -1))) continue;
            if (c->deaf_pair(from->index, i)) continue;
            if (bcast || std::memcmp(c->nodes[i].mac, mac, kMacLen) == 0) {
                c->nodes[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast && !c->done_before_delivery) {
            from->node->on_tx_done(mac, true, c->now_us + static_cast<uint32_t>(c->done_skew_us));
        }
        return 0;
    }
};

size_t alive_peers(Node& n) { return n.peers().count_in_state(PeerState::Alive); }

}  // namespace

TEST(node, a_looser_window_is_announced_first_and_nobody_declares_the_node_dead) {
    // M5.1 CR-3: node 0 relaxes from 100 ms x 6 to 1000 ms x 6 at run time.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(1000);
    Node& a = *c.nodes[0].node;
    Node& b = *c.nodes[1].node;
    CHECK(a.set_heartbeat_window(1000, 6));
    c.advance_ms(150);
    // Peers learned the new window from the announcement ...
    CHECK_EQ(b.peers().slot(0).hb_period_ms, 1000u);
    // ... while node 0 still beacons at the old rate: old window 600 ms + 2 x 500 ms HELLO interval.
    CHECK_EQ(a.heartbeat_period_ms(), 100u);
    c.advance_ms(1500);
    CHECK_EQ(a.heartbeat_period_ms(), 1000u);
    c.advance_ms(20000);
    CHECK_EQ(b.counters().deaths_declared, 0u);
    CHECK_EQ(alive_peers(b), static_cast<size_t>(1));
}

TEST(node, a_peer_that_missed_every_announcement_still_does_not_declare_death) {
    // CR-3's kill criterion: the announcements are all lost; a periodic HELLO must arrive before the
    // node actually slows down. Probes are made rare, or their replies would keep the peer alive and
    // hide the heartbeat question entirely.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon, 1000000);
    c.start_all();
    c.advance_ms(1000);
    c.drop_hello = true;
    CHECK(c.nodes[0].node->set_heartbeat_window(1000, 6));
    c.advance_ms(1250);  // the announcements AND the next periodic HELLOs, lost: longer than the old 600 ms grace
    CHECK_EQ(c.nodes[1].node->peers().slot(0).hb_period_ms, 100u);
    c.drop_hello = false;
    c.advance_ms(20000);
    CHECK_EQ(c.nodes[1].node->peers().slot(0).hb_period_ms, 1000u);
    CHECK_EQ(c.nodes[1].node->counters().deaths_declared, 0u);
}

TEST(node, a_tighter_window_applies_at_once_and_the_range_is_enforced) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(1000);
    Node& a = *c.nodes[0].node;
    CHECK(a.set_heartbeat_window(100, 3));
    CHECK_EQ(a.heartbeat_period_ms(), 100u);
    c.advance_ms(150);
    CHECK_EQ(static_cast<int>(c.nodes[1].node->peers().slot(0).miss_limit), 3);
    CHECK(!a.set_heartbeat_window(2560, 6));  // one byte of centiseconds: 2550 ms is the ceiling
    CHECK(!a.set_heartbeat_window(0, 6));
    CHECK(!a.set_heartbeat_window(105, 6));   // 10 ms steps
    CHECK(!a.set_heartbeat_window(100, 0));
    CHECK(a.set_heartbeat_window(2550, 6));
}

TEST(node, the_busy_bit_reaches_peers_within_one_hello) {
    // M5.1 CR-4: a placement input. Nothing places work yet (M6); this proves the signal arrives.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(1000);
    Node& b = *c.nodes[1].node;
    CHECK_EQ(b.peer_caps(&b.peers().slot(0)) & kHelloCapBusy, 0u);
    c.nodes[0].node->set_busy(true);
    c.advance_ms(50);
    CHECK((b.peer_caps(&b.peers().slot(0)) & kHelloCapBusy) != 0u);
    c.nodes[0].node->set_busy(false);
    c.advance_ms(50);
    CHECK_EQ(b.peer_caps(&b.peers().slot(0)) & kHelloCapBusy, 0u);
}

TEST(node, two_nodes_discover_each_other) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(300);

    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));

    // Each learned the other's id and pinned the v2 profile (§5.3), since both advertise v2.
    const PeerLink& p = c.nodes[0].node->peers().slot(0);
    CHECK_EQ(p.node_id, static_cast<uint16_t>(0x101));
    CHECK_EQ(static_cast<int>(p.version), static_cast<int>(EspNowVersion::V2));
    CHECK_EQ(p.max_payload(), kMaxPayloadV2);
}

TEST(node, broadcast_beacon_cost_is_linear_in_the_cell_size) {
    // The §8.2 scaling fix, as an assertion. One beacon per node per period, no matter how many
    // peers are listening — this is the whole difference between a cell that fits its channel and
    // one that does not.
    for (size_t n : {2u, 5u, 7u}) {
        TestCell c;
        c.build(n, BeaconMode::BroadcastBeacon, /*probe_ms=*/100000);  // probes off
        c.start_all();
        c.advance_ms(1000);  // 10 heartbeat periods

        for (size_t i = 0; i < n; ++i) {
            const Node::TxTally& t = c.nodes[i].node->tx_tally();
            // 10 periods, one beacon each. Independent of n.
            CHECK_EQ(t.beacons, 10u);
            CHECK_EQ(t.probes, 0u);
            CHECK_EQ(t.replies, 0u);
        }
    }
}

TEST(node, unicast_full_mesh_cost_is_quadratic) {
    // The bug being fixed, measured rather than asserted. Every node probes every peer every
    // period, and every probe is answered.
    const size_t n = 5;
    TestCell c;
    c.build(n, BeaconMode::UnicastFullMesh);
    c.start_all();
    c.advance_ms(1000);  // 10 periods

    const uint32_t periods = 10;
    const uint32_t peers = static_cast<uint32_t>(n - 1);

    for (size_t i = 0; i < n; ++i) {
        const Node::TxTally& t = c.nodes[i].node->tx_tally();
        CHECK_EQ(t.beacons, 0u);
        // Exactly (n-1) probes per period, each answered. The relationship is exact rather than
        // approximate, which is what makes the cost predictable enough to have shown up in the
        // airtime table before anyone owned seven boards.
        CHECK_EQ(t.probes, peers * periods);
        CHECK_EQ(t.replies, peers * periods);
    }

    // The blow-up the fix removes, as a ratio against the same cell in broadcast mode: 2(n-1)
    // frames per node per period instead of 1. At n=5 that is 8x; at n=20 it is 38x.
    const Node::TxTally& t0 = c.nodes[0].node->tx_tally();
    CHECK_EQ(t0.probes + t0.replies, 2u * peers * periods);
    CHECK_EQ((t0.probes + t0.replies) / periods, 2u * peers);
}

TEST(node, round_robin_probe_visits_every_peer) {
    // RTT must still be measured for every link, just not every period. Over enough intervals the
    // cursor should reach all of them — a probe policy that starved one link would leave a hole in
    // the M0 histogram that nobody would notice until the report.
    const size_t n = 4;
    TestCell c;
    c.build(n, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
    c.start_all();
    c.advance_ms(3000);

    Node& a = *c.nodes[0].node;
    size_t measured = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const PeerLink& p = a.peers().slot(i);
        if (p.state == PeerState::Free) continue;
        CHECK(p.rtt_samples > 0);
        ++measured;
    }
    CHECK_EQ(measured, n - 1);
}

TEST(node, death_is_declared_at_600ms_of_silence) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(300);
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));

    // Cut the link. §8.2: dead after 6 missed 100 ms periods.
    c.partitioned = true;
    c.advance_ms(599);
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
    CHECK_EQ(c.nodes[0].node->counters().deaths_declared, 0u);

    c.advance_ms(2);
    CHECK_EQ(c.nodes[0].node->counters().deaths_declared, 1u);
    CHECK_EQ(c.nodes[0].node->peers().count_in_state(PeerState::Dead), static_cast<size_t>(1));

    // Heal it: the peer comes back, same incarnation, so this is a revival not a reboot.
    c.partitioned = false;
    c.advance_ms(300);
    CHECK_EQ(c.nodes[0].node->counters().revivals, 1u);
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
}

TEST(node, a_peer_first_heard_by_broadcast_hello_is_not_charged_a_unicast_gap) {
    // Found in a soak (M0-LOG session 30): a board that booted into a running cell reported 47 %
    // inbound unicast delivery from both peers with zero gaps in the window -- ~12,500 "lost" frames
    // counted once, just after boot. The HELLO that creates a peer is a broadcast, and its seq (the
    // sender's broadcast stream) became the baseline for the unicast stream (section 5.1: seq is per
    // (src,dst)), so the peer's first unicast frame was charged with the difference.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
    c.start_all();
    c.advance_ms(3000);  // node 1's unicast and broadcast counters drift apart

    c.reboot(0);
    c.advance_ms(3000);

    const PeerLink* p = c.nodes[0].node->peers().find_by_node_id(0x101);
    CHECK(p != nullptr);
    if (p == nullptr) return;
    CHECK(p->rx_frames > 40u);
    CHECK(p->rtt_samples > 20u);  // unicast traffic flowed after the reboot
    CHECK_EQ(p->rx_lost_seqgap, 0u);
    CHECK_EQ(p->rx_reorder_dup, 0u);
    uint32_t ppm = 0;
    CHECK(p->pdr_rx_ppm(ppm));
    CHECK_EQ(ppm, 1000000u);
}

TEST(node, every_frame_accepted_from_a_peer_is_reported_with_its_rssi_and_kind) {
    // M8.2 (passive-sensor's PS-0, and PS-2's raw material): one sample per accepted frame, never one
    // per pass of the link task -- so the count matches the link's own rx_frames.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
    c.start_all();
    c.advance_ms(3000);
    const PeerLink& p = c.nodes[0].node->peers().slot(0);
    const std::vector<RxSample>& got = c.nodes[0].samples;
    // All but one: the HELLO that introduced the peer is counted by the link but arrives before the
    // peer is known, so there is no peer to report it for. PS-2's acceptance compares deltas.
    CHECK_EQ(static_cast<uint32_t>(got.size()), p.rx_frames - 1u);
    size_t beacons = 0, unicast = 0;
    uint16_t last_seq = 0;
    bool seq_rises = true;
    for (const RxSample& rs : got) {
        CHECK_EQ(static_cast<unsigned>(rs.node_id), 0x101u);
        CHECK_EQ(static_cast<int>(rs.rssi), -50);  // what the test cell's radio reports
        CHECK(!rs.relayed);
        CHECK_EQ(static_cast<int>(rs.channel), 1);  // the channel the frame was accepted on
        if (rs.kind == kRxKindBeacon) {
            if (beacons > 0 && static_cast<uint16_t>(rs.hb_seq - last_seq) != 1u) seq_rises = false;
            last_seq = rs.hb_seq;
            ++beacons;
        } else if (rs.kind == kRxKindUnicast) {
            ++unicast;
        }
    }
    CHECK(beacons > 20u);  // a 100 ms heartbeat for ~3 s
    CHECK(unicast > 20u);  // the probes and their replies
    CHECK(seq_rises);      // consecutive beacons, consecutive sequence numbers: nothing skipped
}

TEST(node, bye_marks_a_peer_left_not_dead) {
    // §5.2: an intentional departure is not a failure, and counting it as one would corrupt the
    // failure statistics M0 exists to produce.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(300);

    c.nodes[1].node->depart();
    CHECK(c.nodes[1].node->departed());
    c.advance_ms(1500);  // well past the 600 ms death window

    CHECK_EQ(c.nodes[0].node->peers().count_in_state(PeerState::Left), static_cast<size_t>(1));
    CHECK_EQ(c.nodes[0].node->counters().deaths_declared, 0u);

    // A departed node stops beaconing entirely.
    const uint32_t beacons_before = c.nodes[1].node->tx_tally().beacons;
    c.advance_ms(500);
    CHECK_EQ(c.nodes[1].node->tx_tally().beacons, beacons_before);

    // And can come back.
    c.nodes[1].node->rejoin();
    c.advance_ms(300);
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
}

TEST(node, broadcast_and_unicast_sequence_streams_do_not_corrupt_each_other) {
    // §5.1 makes seq per (src,dst). A node sends broadcast beacons and unicast probes to the same
    // peer, which are two different sequences; folding them into one seq_rx_last would invent a gap
    // at every alternation and destroy the inbound PDR figure. This is the test for that.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
    c.start_all();
    c.advance_ms(3000);

    const PeerLink& p = c.nodes[0].node->peers().slot(0);
    CHECK(p.rx_frames > 40u);        // plenty of traffic went past
    CHECK(p.rx_bcast_frames > 20u);  // including many beacons
    CHECK(p.rtt_samples > 20u);      // and many probes

    // On a perfect channel nothing was lost, in either stream.
    CHECK_EQ(p.rx_lost_seqgap, 0u);
    CHECK_EQ(p.rx_hb_lost_seqgap, 0u);
    CHECK_EQ(p.rx_reorder_dup, 0u);

    uint32_t ppm = 0;
    CHECK(p.pdr_rx_ppm(ppm));
    CHECK_EQ(ppm, 1000000u);
}

TEST(node, a_seven_node_cell_holds_together_on_a_perfect_channel) {
    // The fleet actually on order. Every node should see all six peers, continuously.
    const size_t n = 7;
    TestCell c;
    c.build(n, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(5000);

    uint32_t total_beacons = 0, total_deaths = 0;
    for (size_t i = 0; i < n; ++i) {
        CHECK_EQ(alive_peers(*c.nodes[i].node), n - 1);
        total_beacons += c.nodes[i].node->tx_tally().beacons;
        total_deaths += c.nodes[i].node->counters().deaths_declared;
    }
    CHECK_EQ(total_deaths, 0u);

    // 7 nodes x 50 periods of beacons. The point is that it is linear: a unicast full mesh would
    // put 7 x 6 x 2 x 50 = 4200 frames on the wire for the same liveness.
    CHECK_EQ(total_beacons, static_cast<uint32_t>(n * 50));
}

TEST(node, the_peer_table_stops_at_nineteen_unicast_peers) {
    // §3 caps ESP-NOW at 20 paired devices *including* the broadcast entry, so a cell admits 19.
    // Not reachable with a 7-board fleet, but it is the boundary §4's "~20 nodes" runs into.
    CHECK_EQ(kMaxUnicastPeers, static_cast<size_t>(19));
    CHECK(kMaxUnicastPeers < kMaxPeers);
}

TEST(node, a_node_that_never_started_sends_nothing) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    // No start_all(): tick() must be inert until start() has run, or a half-initialised node would
    // put frames on the wire before its identity was settled.
    c.advance_ms(1000);
    CHECK_EQ(c.frames_on_wire, 0u);
    CHECK_EQ(c.nodes[0].node->tx_tally().beacons, 0u);
}

TEST(node, a_completion_stamped_before_the_probe_is_not_credited_to_it) {
    // The 2026-10-02 soak recorded txq_max_us = 2^32 - 86 on two links: a completion stamped 86 us
    // before the probe's submit was credited to it, and sendcb - submit wrapped.
    //
    // Two halves, so the test cannot pass vacuously. A completion stamped 50 us *after* submit must
    // be credited (txq reads 50); one stamped 86 us *before* must not (txq stays 0, not ~4.29e9).
    // RTT never reads the send callback and must be measured in both.
    {
        TestCell c;
        c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
        c.done_before_delivery = true;
        c.done_skew_us = 50;
        c.start_all();
        c.advance_ms(3000);
        const PeerLink& p = c.nodes[0].node->peers().slot(0);
        CHECK(p.rtt_samples > 0u);
        CHECK_EQ(p.txq_max_us, 50u);
    }
    {
        TestCell c;
        c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100);
        c.done_before_delivery = true;
        c.done_skew_us = -86;
        c.start_all();
        c.advance_ms(3000);
        const PeerLink& p = c.nodes[0].node->peers().slot(0);
        CHECK(p.rtt_samples > 0u);
        CHECK_EQ(p.txq_max_us, 0u);
        CHECK_EQ(p.txq_last_us, 0u);
    }
}

TEST(node, the_beacon_is_eight_bytes_with_seq_zero_so_it_fits_one_can_frame) {
    // §5.3.1 / M4. The broadcast beacon carries liveness only; header seq and msg_id are 0 on every
    // transport so the CAN single-frame profile can reconstruct it byte-identical.
    struct Capture {
        std::vector<std::vector<uint8_t>> frames;
    } cap;
    NodeConfig cfg;
    cfg.node_id = 0x0123;
    cfg.boot_epoch = 7;
    cfg.mac[0] = 0x02;
    cfg.beacon_mode = BeaconMode::BroadcastBeacon;
    NodeHal hal{};
    hal.ctx = &cap;
    hal.send = [](void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) -> int32_t {
        if (std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0) {
            static_cast<Capture*>(ctx)->frames.emplace_back(data, data + len);
        }
        return 0;
    };
    static uint32_t now = 0;
    now = 0;
    hal.now_ms = [](void*) { return now; };
    hal.now_us = [](void*) { return now * 1000; };
    Node n(cfg, hal);
    n.start();
    for (int k = 0; k < 350; ++k) {
        ++now;
        n.tick(now);
    }
    int beacons = 0;
    for (const auto& raw : cap.frames) {
        Frame f;
        CHECK(parse(raw.data(), raw.size(), f) == FrameError::Ok);
        if (f.hdr.opcode != kOpHeartbeat) continue;
        ++beacons;
        CHECK_EQ(f.payload_len, static_cast<uint16_t>(8));
        CHECK_EQ(f.hdr.seq, static_cast<uint16_t>(0));
        CHECK_EQ(f.hdr.msg_id, static_cast<uint16_t>(0));
        BeaconPayload b{};
        CHECK(load_beacon(f.payload, f.payload_len, b));
        CHECK_EQ(b.node_id, static_cast<uint16_t>(0x0123));
        CHECK_EQ(b.boot_epoch, 7u);
        CHECK_EQ(b.hb_seq, static_cast<uint16_t>(beacons));
    }
    CHECK(beacons >= 3);
}

TEST(node, beacons_keep_a_peer_alive_and_a_lying_beacon_is_refused) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon, /*probe_ms=*/100000);  // probes off: beacons only
    c.start_all();
    c.advance_ms(300);
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
    c.advance_ms(5000);  // far past the 600 ms death window: only beacons can have kept it alive
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(1));
    const PeerLink& p = c.nodes[0].node->peers().slot(0);
    CHECK(p.rx_bcast_frames >= 50u);
    CHECK_EQ(p.rx_hb_lost_seqgap, 0u);

    // A beacon whose payload names another node is malformed, whoever sent it.
    BeaconPayload liar{};
    liar.node_id = 0x0999;
    liar.hb_seq = 1;
    liar.boot_epoch = 1;
    EncodeSpec spec;
    spec.src = c.nodes[1].node->config().node_id;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpHeartbeat;
    uint8_t buf[64];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&liar), sizeof(liar), buf, sizeof(buf), n) == FrameError::Ok);
    const uint32_t before = p.rx_bcast_frames;
    c.nodes[0].node->on_rx(c.nodes[1].mac, buf, n, c.now_us, -50);
    CHECK_EQ(p.rx_bcast_frames, before);
}

// ---- M5.1 CR-1: the cell moves channel, and a node that missed it finds the cell again ----

namespace {
// Rebuild node i with its channel owned by someone else (a Wi-Fi station's router).
void make_fixed(TestCell& c, size_t i) {
    TestNode& t = c.nodes[i];
    delete t.node;
    NodeConfig cfg;
    cfg.node_id = static_cast<uint16_t>(0x100 + i);
    cfg.boot_epoch = 1;
    std::memcpy(cfg.mac, t.mac, kMacLen);
    cfg.hello_interval_ms = 500;
    cfg.channel_fixed = true;
    t.node = new Node(cfg, t.hal);
}
}  // namespace

TEST(node, an_announced_move_takes_the_whole_cell_and_nobody_dies) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(1000);
    CHECK(c.nodes[0].node->move_cell(6, 300));
    c.advance_ms(400);
    for (TestNode& t : c.nodes) {
        CHECK_EQ(static_cast<int>(t.chan), 6);
        CHECK_EQ(static_cast<int>(t.node->channel()), 6);
    }
    c.advance_ms(5000);
    for (TestNode& t : c.nodes) {
        CHECK_EQ(alive_peers(*t.node), static_cast<size_t>(2));
        CHECK_EQ(t.node->counters().deaths_declared, 0u);
    }
}

TEST(node, a_node_whose_router_moved_is_found_again_by_scanning) {
    // CR-1's accept shape: the station node is moved by its router without warning; the others lose
    // it, declare it dead (which is honest: they cannot hear it), scan, and re-converge.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(1000);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));
    c.nodes[0].node->set_channel_now(9);
    // Bound: 3 s alone, then at most one full sweep of 11 channels at 300 ms, plus a HELLO round.
    c.advance_ms(3000 + 11 * 300 + 1000);
    CHECK_EQ(static_cast<int>(c.nodes[1].chan), 9);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));
    CHECK_EQ(c.nodes[1].node->channel_counters().found_by_scan, 1u);
    CHECK(!c.nodes[1].node->scanning());
    // And the station node, whose channel is not its own to change, never hopped.
    CHECK_EQ(static_cast<int>(c.nodes[0].chan), 9);
    CHECK_EQ(c.nodes[0].node->channel_counters().scan_hops, 0u);
}

TEST(node, when_the_station_moves_with_its_router_the_whole_cell_follows_it) {
    // Found on the bench (M0-LOG session 29): the router restarted on another channel, the extender
    // (the station) went with it, and the other two -- still hearing each other -- never looked for it,
    // because CR-1's scan starts only when every peer is lost. The two-node test above could not show
    // it: with two nodes, losing the station IS losing every peer.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(2));
    c.nodes[0].node->set_channel_now(9);
    // Bound: the 600 ms death window, one sweep of 10 channels at 300 ms, and a HELLO round.
    c.advance_ms(600 + 10 * 300 + 2000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 9);
        CHECK_EQ(alive_peers(*c.nodes[i].node), static_cast<size_t>(2));
        CHECK_EQ(c.nodes[i].node->channel_counters().authority_sweeps, 1u);
        CHECK_EQ(c.nodes[i].node->channel_counters().authority_found, 1u);
        CHECK(!c.nodes[i].node->sweeping_for_authority());
    }
    CHECK_EQ(static_cast<int>(c.nodes[0].chan), 9);  // the station itself never hopped
    CHECK_EQ(c.nodes[0].node->channel_counters().scan_hops, 0u);
}

TEST(node, a_station_still_hunting_for_its_router_sends_nobody_to_the_channel_it_booted_on) {
    // Found on the bench (M0-LOG session 33): the extender rebooted on channel 1, the channel it saved
    // the night before; its station's scan carried the radio to channel 9, where the cell was, and its
    // HELLO -- fixed, declaring 1 -- took the cell to channel 1. Six seconds later it associated on 11.
    // A station that does not yet know its channel declares 0: still the authority, followed nowhere.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.nodes[0].node->set_channel_known(false);
    c.start_all();
    c.nodes[1].node->set_channel_now(9);  // the cell is on 9 ...
    c.nodes[2].node->set_channel_now(9);
    c.nodes[0].chan = 9;  // ... and the station's scan has its radio there, while it still thinks 1
    // Sampled, not checked once: followed, the others go to 1, lose the station, sweep back to 9 and
    // are sent to 1 again -- a cycle that passes through 9 every ~3 s.
    for (int k = 0; k < 30; ++k) {
        c.advance_ms(200);
        for (size_t i = 1; i < 3; ++i) CHECK_EQ(static_cast<int>(c.nodes[i].chan), 9);  // nobody went to "1"
    }
    for (size_t i = 1; i < 3; ++i) CHECK_EQ(alive_peers(*c.nodes[i].node), static_cast<size_t>(2));
    // Associated: the router's channel is now the station's, and declaring it is right again.
    c.nodes[0].node->set_channel_now(9);
    c.nodes[0].node->set_channel_known(true);
    c.advance_ms(3000);
    c.nodes[0].node->set_channel_now(11);  // the router moves; the cell follows its station
    c.advance_ms(600 + 10 * 300 + 2000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 11);
        CHECK_EQ(alive_peers(*c.nodes[i].node), static_cast<size_t>(2));
    }
}

TEST(node, a_sweep_neither_stops_where_a_hunting_station_is_heard_nor_follows_another_sweeper) {
    // Found on the bench (M0-LOG session 33): the extender rebooted; A and C swept for it at once. A
    // heard it on channel 2 -- its station's scan passing through -- and stopped there, then followed
    // C to 3, a channel C was only visiting. Each move made the detector on C relearn the room.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    c.deaf = {{0, 1}, {0, 2}};  // the station goes down; 600 ms on, the others sweep, 2 first
    c.advance_ms(650);
    TestNode& st = c.nodes[0];  // it reboots hunting, its radio wherever its scan is: 2
    st.cfg.channel_fixed = true;
    st.cfg.channel_known = false;
    st.cfg.channel = 2;
    c.reboot(0);
    st.chan = 2;  // a fixed node never tunes its own radio: the router (here, the test) does
    c.advance_ms(50);  // its boot HELLO is lost (on the bench, to its station's scan) ...
    c.deaf.clear();    // ... so the sweepers on 2 hear its new incarnation's beacons first
    // While it hunts, the others may sweep, or visit a channel for one dwell (300 ms), but never stay
    // anywhere but home (1): outside a sweep, no run off home longer than a visit.
    int run[3] = {0, 0, 0}, longest = 0;
    for (int k = 0; k < 60; ++k) {
        c.advance_ms(100);
        for (size_t i = 1; i < 3; ++i) {
            const bool away = c.nodes[i].chan != 1 && !c.nodes[i].node->sweeping_for_authority();
            run[i] = away ? run[i] + 1 : 0;
            if (run[i] > longest) longest = run[i];
        }
    }
    CHECK(longest <= 4);
    // Associated on 1, where the cell is: everyone together, nobody sweeping.
    c.nodes[0].node->set_channel_now(1);
    c.nodes[0].node->set_channel_known(true);
    c.advance_ms(600 + 11 * 300 + 3000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 1);
        CHECK_EQ(alive_peers(*c.nodes[i].node), static_cast<size_t>(2));
        CHECK(!c.nodes[i].node->sweeping_for_authority());
    }
}

TEST(node, a_node_sweeping_for_its_authority_declares_no_channel_on_the_channels_it_visits) {
    // Found on the bench (M0-LOG session 33): A followed C to channel 3, a channel C was only visiting
    // in its sweep. A sweeper's HELLO on a visit now declares 0, so nobody can be sent there by it.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    c.deaf = {{0, 1}, {0, 2}};  // the station goes silent: 1 and 2 sweep
    c.hellos.clear();
    c.advance_ms(600 + 10 * 300 + 3000);
    size_t on_visits = 0;
    for (const TestCell::SentHello& h : c.hellos) {
        if (h.from == 0 || h.radio == 1) continue;  // the station, or a sweeper at home
        ++on_visits;
        CHECK_EQ(static_cast<int>(h.declared), 0);
    }
    CHECK(on_visits >= 10u);  // the sweep greets on every channel it visits
}

TEST(node, when_the_station_dies_the_others_sweep_once_and_come_home) {
    // The other half of the same rule: a station that is gone, not moved. One sweep, then back to the
    // channel the rest of the cell is on -- and no sweeping for ever after.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    c.deaf = {{0, 1}, {0, 2}};  // the station goes silent
    c.advance_ms(600 + 10 * 300 + 3000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 1);  // home
        CHECK_EQ(c.nodes[i].node->channel_counters().authority_sweeps, 1u);
        CHECK_EQ(c.nodes[i].node->channel_counters().authority_found, 0u);
    }
    c.advance_ms(10000);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));  // 1 and 2 have each other again
    CHECK_EQ(c.nodes[1].node->channel_counters().authority_sweeps, 1u);  // and did not sweep again
    CHECK_EQ(c.nodes[2].node->channel_counters().authority_sweeps, 1u);
}

TEST(node, a_station_that_comes_back_on_another_channel_after_the_sweep_is_found) {
    // Found on the bench (M0-LOG session 30): the extender rebooted, and its station joined the house
    // mesh's OTHER access point -- channel 11, not 1. Its reboot outlasted the one sweep the others
    // made for it, they went home and stopped looking, and the cell stayed split until the extender
    // happened to reboot onto channel 1. After a failed sweep the others now keep looking, one
    // channel per visit, each visit shorter than the death window.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    c.deaf = {{0, 1}, {0, 2}};          // the station goes away (a reboot) ...
    c.advance_ms(600 + 10 * 300 + 2000);  // ... for longer than the sweep, which finds nothing
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 1);
        CHECK_EQ(c.nodes[i].node->channel_counters().authority_found, 0u);
    }
    c.nodes[0].node->set_channel_now(9);  // ... and comes back on the other access point's channel
    c.deaf.clear();
    // Bound: one visit to each of ten channels, every 2 s, plus CR-1's scan for the node left behind.
    c.advance_ms(10 * 2000 + 11 * 300 + 3000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 9);
        CHECK_EQ(alive_peers(*c.nodes[i].node), static_cast<size_t>(2));
    }
}

TEST(node, a_station_gone_for_good_is_looked_for_ever_more_rarely) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(2000);
    c.deaf = {{0, 1}, {0, 2}};
    c.advance_ms(600 + 10 * 300 + 25000);  // the sweep, then the first pass of visits
    const uint32_t first = c.nodes[1].node->channel_counters().search_visits;
    CHECK(first >= 9u);
    c.advance_ms(60000);  // the second pass, at half the rate
    const uint32_t second = c.nodes[1].node->channel_counters().search_visits - first;
    CHECK(second >= 8u && second <= 16u);
    c.advance_ms(30u * 60000u);  // half an hour on, the rate has fallen to one visit a minute
    const uint32_t before = c.nodes[1].node->channel_counters().search_visits;
    c.advance_ms(5u * 60000u);
    const uint32_t late = c.nodes[1].node->channel_counters().search_visits - before;
    CHECK(late >= 4u && late <= 6u);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));  // 1 and 2 still together
    CHECK_EQ(c.nodes[1].node->counters().deaths_declared, 1u);       // only the station's
}

TEST(node, a_node_that_boots_on_the_wrong_channel_finds_the_cell) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    for (TestNode& t : c.nodes) t.chan = 4;  // the cell lives on 4 ...
    c.nodes[2].chan = 1;                     // ... and node 2 boots on its default, 1
    // Nodes 0 and 1 start on 4 by being told so; node 2 by default config is on 1.
    c.start_all();
    c.nodes[0].node->set_channel_now(4);
    c.nodes[1].node->set_channel_now(4);
    c.advance_ms(3000 + 11 * 300 + 1500);
    CHECK_EQ(static_cast<int>(c.nodes[2].chan), 4);
    CHECK_EQ(alive_peers(*c.nodes[2].node), static_cast<size_t>(2));
}

TEST(node, a_scan_that_locks_one_channel_off_moves_to_where_the_station_really_is) {
    // Found on the bench: at 10 cm, board C locked onto channel 10 hearing board A on 11. With the
    // channel declared in HELLO, the scanner moves to the station's channel at its next HELLO.
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.leak_adjacent = true;
    make_fixed(c, 0);
    c.start_all();
    c.advance_ms(1000);
    c.nodes[0].node->set_channel_now(9);
    c.advance_ms(3000 + 11 * 300 + 2000);
    CHECK_EQ(static_cast<int>(c.nodes[1].chan), 9);
    CHECK_EQ(static_cast<int>(c.nodes[0].chan), 9);
    CHECK_EQ(alive_peers(*c.nodes[1].node), static_cast<size_t>(1));
}

TEST(node, a_scanner_defers_to_a_stable_peer_and_never_drags_the_cell) {
    // Found on the bench at boot: node 0x6300 scanned while its peer rebooted, locked onto channel 2
    // hearing the peer on 1, and the peer then moved to 2 because 0x6300 had the lower id. Now the
    // scanner defers to the stable peer, whatever the ids.
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.leak_adjacent = true;
    c.start_all();
    c.advance_ms(1000);
    CHECK(c.nodes[1].node->move_cell(5, 100));  // the cell lives on 5
    c.advance_ms(2000);
    // Node 0 -- the LOWEST id -- is suddenly on 3; nodes 1 and 2 still hear each other on 5, so they
    // are stable and do not scan. Node 0's first hop is 4, which hears 5 by leakage: the bench case.
    c.nodes[0].node->set_channel_now(3);
    c.advance_ms(3000 + 11 * 300 + 2500);
    CHECK_EQ(static_cast<int>(c.nodes[1].chan), 5);  // the stable nodes did not move
    CHECK_EQ(static_cast<int>(c.nodes[2].chan), 5);
    CHECK_EQ(static_cast<int>(c.nodes[0].chan), 5);  // the scanner came to them
    CHECK_EQ(alive_peers(*c.nodes[0].node), static_cast<size_t>(2));
}

// ---- M5.1 CR-2 / ADR-009: one relay, one hop ----

TEST(node, a_relay_joins_two_nodes_that_cannot_hear_each_other) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.deaf.push_back({0, 2});
    c.nodes[1].node->set_relay(true);
    c.start_all();
    c.advance_ms(4000);
    Node& a = *c.nodes[0].node;
    Node& g = *c.nodes[2].node;
    CHECK_EQ(alive_peers(a), static_cast<size_t>(2));  // the relay directly, the far node through it
    CHECK_EQ(alive_peers(g), static_cast<size_t>(2));
    const PeerLink* ag = a.peers().find_by_node_id(0x102);
    CHECK(ag != nullptr && a.peer_relayed(ag));
    CHECK(!a.peer_relayed(a.peers().find_by_node_id(0x101)));
    CHECK(c.nodes[1].node->relay_counters().unicast_forwarded > 0u);  // probes and replies went through
    CHECK(ag->rtt_samples > 0u);                                       // a two-hop round trip was measured
    // The far node falls silent (out of the relay's range too): the near node declares it dead.
    c.deaf.push_back({1, 2});
    c.advance_ms(2000);
    CHECK_EQ(alive_peers(a), static_cast<size_t>(1));
    CHECK(a.counters().deaths_declared >= 1u);
}

TEST(node, the_direct_path_wins_when_it_exists_and_the_relay_takes_over_when_it_dies) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.deaf.push_back({0, 2});
    c.nodes[1].node->set_relay(true);
    c.start_all();
    c.advance_ms(4000);
    Node& a = *c.nodes[0].node;
    CHECK(a.peer_relayed(a.peers().find_by_node_id(0x102)));
    c.deaf.clear();  // node 2 comes into direct range
    c.advance_ms(3000);
    CHECK(!a.peer_relayed(a.peers().find_by_node_id(0x102)));
    CHECK(a.relay_counters().relay_dup > 0u);  // relayed copies dropped while the direct path lives
    c.deaf.push_back({0, 2});  // and out of range again
    c.advance_ms(3000);
    CHECK(a.peer_relayed(a.peers().find_by_node_id(0x102)));
    CHECK_EQ(alive_peers(a), static_cast<size_t>(2));
}

TEST(node, a_forwarded_frame_from_a_node_that_is_not_a_relay_is_refused) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.start_all();
    c.advance_ms(1000);
    // A beacon claiming to be from node 2, arriving from node 1's MAC; node 1 never declared RELAY.
    BeaconPayload bp{};
    bp.node_id = 0x102;
    bp.hb_seq = 9;
    bp.boot_epoch = 1;
    EncodeSpec spec;
    spec.src = 0x102;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpHeartbeat;
    spec.lclass = kClassL3;
    spec.priority = 1;
    uint8_t buf[64];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&bp), static_cast<uint16_t>(sizeof(bp)), buf, sizeof(buf), n) ==
          FrameError::Ok);
    const uint32_t before = c.nodes[0].node->relay_counters().refused;
    c.nodes[0].node->on_rx(c.nodes[1].mac, buf, n, c.now_us, -50);
    CHECK_EQ(c.nodes[0].node->relay_counters().refused, before + 1);
}

TEST(node, two_relays_in_one_cell_resolve_to_the_lower_node_id) {
    TestCell c;
    c.build(3, BeaconMode::BroadcastBeacon);
    c.nodes[1].node->set_relay(true);
    c.nodes[2].node->set_relay(true);
    c.start_all();
    c.advance_ms(2000);
    CHECK(c.nodes[1].node->relay());
    CHECK(!c.nodes[2].node->relay());
}
