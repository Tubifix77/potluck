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
};

// A cell with a perfect channel and a clock the test advances by hand.
struct TestCell {
    std::vector<TestNode> nodes;
    uint32_t now_us = 0;
    bool partitioned = false;  // when true, nothing is delivered
    bool drop_hello = false;   // when true, HELLOs are lost and everything else is delivered
    bool leak_adjacent = false;  // M5.1: a frame on channel n is also heard on n-1 and n+1 (close range)
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
            t.node = new Node(cfg, t.hal);
        }
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

TEST(node, two_free_nodes_one_channel_apart_settle_on_the_lower_ids_channel) {
    TestCell c;
    c.build(2, BeaconMode::BroadcastBeacon);
    c.leak_adjacent = true;
    c.start_all();
    c.nodes[0].node->set_channel_now(5);
    c.nodes[1].node->set_channel_now(6);
    c.advance_ms(3000);
    CHECK_EQ(static_cast<int>(c.nodes[0].chan), 5);
    CHECK_EQ(static_cast<int>(c.nodes[1].chan), 5);  // 0x101 defers to 0x100; no ping-pong
    c.advance_ms(5000);
    CHECK_EQ(static_cast<int>(c.nodes[1].chan), 5);
}
