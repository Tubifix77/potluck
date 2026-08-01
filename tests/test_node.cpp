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
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
};

// A cell with a perfect channel and a clock the test advances by hand.
struct TestCell {
    std::vector<TestNode> nodes;
    uint32_t now_us = 0;
    bool partitioned = false;  // when true, nothing is delivered
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
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            if (i == from->index) continue;
            if (bcast || std::memcmp(c->nodes[i].mac, mac, kMacLen) == 0) {
                c->nodes[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast) {
            from->node->on_tx_done(mac, true, c->now_us);
        }
        return 0;
    }
};

size_t alive_peers(Node& n) { return n.peers().count_in_state(PeerState::Alive); }

}  // namespace

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
