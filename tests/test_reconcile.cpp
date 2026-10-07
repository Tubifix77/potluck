// M6, section 7.7: the reconciler on real pot::Node objects over a perfect channel whose nodes can be
// killed, revived in a new incarnation, and partitioned. The acceptance lines are the tests:
//
//   "kill the node running a portable actor -- the actor serves reads from another node within 2 s;
//    when the killed node returns, it does not double-run. Partition and heal the mesh -- duplication
//    ends within gossip convergence, and fenced consumers never accept the losing instance."

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/reconcile.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kKey = path_hash("potluck://lab/act/ticker/out");

struct RCell;

struct RNode {
    RCell* cell = nullptr;
    size_t index = 0;
    uint16_t id = 0;
    uint32_t epoch = 1;
    bool down = false;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    Reconciler* rec = nullptr;
    NodeHal hal{};
};

struct RCell {
    std::vector<RNode> nodes;
    std::vector<TickerConfig> actors;
    std::vector<std::pair<size_t, size_t>> deaf;
    uint32_t now_us = 0;
    // Frames land kLatencyMs after they are sent. Not decoration: with instant delivery a rebooted
    // node hears every claim before its own first tick, and settling could be removed without any
    // test noticing (it was, once, as a mutation check).
    static constexpr uint32_t kLatencyMs = 3;
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        bool bcast;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;
    // HELLO and HELLO_ACK to or from this node are lost until hello_blocked_until_us: admission takes
    // as long as it does on the boards, where a signed HELLO arrives 2-3.5 s after a reboot.
    size_t hello_blocked = SIZE_MAX;
    uint32_t hello_blocked_until_us = 0;

    bool deaf_pair(size_t a, size_t b) const {
        for (const auto& d : deaf) {
            if ((d.first == a && d.second == b) || (d.first == b && d.second == a)) return true;
        }
        return false;
    }

    static bool on_call(void* ctx, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
        return static_cast<RNode*>(ctx)->rec->on_cast(from, path, args, len);
    }

    void boot(RNode& t) {
        delete t.rec;
        delete t.node;
        NodeConfig cfg;
        cfg.node_id = t.id;
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        t.hal.ctx = &t;
        t.hal.send = &RCell::send;
        t.hal.now_ms = [](void* c) { return static_cast<RNode*>(c)->cell->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<RNode*>(c)->cell->now_us; };
        t.node = new Node(cfg, t.hal);
        t.rec = new Reconciler(*t.node);
        CHECK(t.rec->load(actors.data(), actors.size()));
        t.node->set_call_handler(&RCell::on_call, &t);
        t.node->start();
        t.rec->start(now_us / 1000);
        t.down = false;
    }

    void build(size_t n, const std::vector<uint8_t>& gravity) {
        nodes.resize(n);
        TickerConfig tc{};
        tc.out_hash = kKey;
        tc.period_ms = 100;
        tc.count = static_cast<uint8_t>(n);
        for (size_t i = 0; i < n; ++i) {
            RNode& t = nodes[i];
            t.cell = this;
            t.index = i;
            t.id = static_cast<uint16_t>(0x100 + i);
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(i);
            tc.node[i] = t.id;
            tc.gravity[i] = gravity.empty() ? 1 : gravity[i];
        }
        actors.assign(1, tc);
        for (RNode& t : nodes) boot(t);
    }

    ~RCell() {
        for (RNode& t : nodes) {
            delete t.rec;
            delete t.node;
        }
    }

    void kill(size_t i) { nodes[i].down = true; }
    void revive(size_t i) {
        ++nodes[i].epoch;
        boot(nodes[i]);
    }

    // One millisecond at a time, calling `each` after every step.
    template <typename F>
    void run(uint32_t ms, F each) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            deliver();
            for (RNode& t : nodes) {
                if (t.down) continue;
                t.node->tick(now_us / 1000);
                t.rec->tick(now_us / 1000);
            }
            each();
        }
    }
    void run(uint32_t ms) {
        run(ms, [] {});
    }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        RNode* from = static_cast<RNode*>(ctx);
        RCell* c = from->cell;
        if (from->down) return 0;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            if (i == from->index) continue;
            if (bcast || std::memcmp(c->nodes[i].mac, mac, kMacLen) == 0) {
                c->air.push_back(Flight{c->now_us + kLatencyMs * 1000, from->index, i, bcast,
                                        std::vector<uint8_t>(data, data + len)});
            }
        }
        return 0;
    }

    // Whether the receiver is down or deaf is decided on arrival, as on a real channel.
    void deliver() {
        std::vector<Flight> due;
        for (size_t k = 0; k < air.size();) {
            if (air[k].due_us <= now_us) {
                due.push_back(std::move(air[k]));
                air.erase(air.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
        for (Flight& f : due) {
            RNode& from = nodes[f.from];
            RNode& to = nodes[f.to];
            const bool hello = f.data.size() > 6 && (f.data[6] == kOpHello || f.data[6] == kOpHelloAck);
            const bool blocked = hello && now_us < hello_blocked_until_us &&
                                 (f.from == hello_blocked || f.to == hello_blocked);
            const bool heard = !to.down && to.node != nullptr && !deaf_pair(f.from, f.to) && !blocked;
            if (heard) to.node->on_rx(from.mac, f.data.data(), f.data.size(), now_us, -50);
            if (!f.bcast && !from.down && from.node != nullptr) from.node->on_tx_done(to.mac, heard, now_us);
        }
    }

    size_t running_count() const {
        size_t n = 0;
        for (const RNode& t : nodes) {
            if (!t.down && t.rec->view(0).running) ++n;
        }
        return n;
    }
    int runner() const {
        for (const RNode& t : nodes) {
            if (!t.down && t.rec->view(0).running) return static_cast<int>(t.index);
        }
        return -1;
    }
};

}  // namespace

// ---- the pure parts --------------------------------------------------------------------------

TEST(reconcile, rank_is_gravity_first_then_rendezvous_and_the_same_everywhere) {
    TickerConfig tc{};
    tc.out_hash = kKey;
    tc.period_ms = 100;
    tc.count = 3;
    tc.node[0] = 0x6300;
    tc.node[1] = 0x7368;
    tc.node[2] = 0x8160;
    tc.gravity[0] = tc.gravity[1] = tc.gravity[2] = 1;
    auto all = [](void*, uint16_t) { return true; };
    uint16_t a[kMaxEligible], b[kMaxEligible];
    CHECK_EQ(rank_nodes(tc, all, nullptr, a), 3u);
    // Listing order must not matter: reverse the config and rank again.
    TickerConfig rev = tc;
    rev.node[0] = tc.node[2];
    rev.node[2] = tc.node[0];
    CHECK_EQ(rank_nodes(rev, all, nullptr, b), 3u);
    for (size_t i = 0; i < 3; ++i) CHECK_EQ(a[i], b[i]);
    // Gravity outranks the hash: raise the last-ranked node.
    for (uint8_t i = 0; i < 3; ++i) {
        if (tc.node[i] == a[2]) tc.gravity[i] = 2;
    }
    CHECK_EQ(rank_nodes(tc, all, nullptr, b), 3u);
    CHECK_EQ(b[0], a[2]);
    // A dead node drops out and the others keep their relative order (HRW's minimal disruption).
    const uint16_t dead = a[0];
    auto not_dead = [](void* ctx, uint16_t n) { return n != *static_cast<const uint16_t*>(ctx); };
    tc.gravity[0] = tc.gravity[1] = tc.gravity[2] = 1;
    CHECK_EQ(rank_nodes(tc, not_dead, const_cast<uint16_t*>(&dead), b), 2u);
    CHECK_EQ(b[0], a[1]);
    CHECK_EQ(b[1], a[2]);
}

TEST(reconcile, rendezvous_spreads_actors_across_nodes) {
    // 3,000 keys over three nodes: each node should win about a third. A weight function with a bias
    // would pile portable actors onto one board.
    const uint16_t ids[3] = {0x6300, 0x7368, 0x8160};
    int wins[3] = {0, 0, 0};
    for (uint32_t k = 1; k <= 3000; ++k) {
        int best = 0;
        for (int i = 1; i < 3; ++i) {
            if (hrw_weight(k * 2654435761u, ids[i]) > hrw_weight(k * 2654435761u, ids[best])) best = i;
        }
        ++wins[best];
    }
    for (int w : wins) CHECK(w > 850 && w < 1150);
}

TEST(reconcile, ticker_config_is_validated) {
    uint8_t cfg[7 + 9] = {};
    auto put = [&](uint32_t hash, uint16_t period, uint8_t n) {
        cfg[0] = static_cast<uint8_t>(hash);
        cfg[1] = static_cast<uint8_t>(hash >> 8);
        cfg[2] = static_cast<uint8_t>(hash >> 16);
        cfg[3] = static_cast<uint8_t>(hash >> 24);
        cfg[4] = static_cast<uint8_t>(period);
        cfg[5] = static_cast<uint8_t>(period >> 8);
        cfg[6] = n;
    };
    put(kKey, 100, 3);
    const uint16_t ids[3] = {0x6300, 0x7368, 0x8160};
    for (int i = 0; i < 3; ++i) {
        cfg[7 + 3 * i] = static_cast<uint8_t>(ids[i]);
        cfg[8 + 3 * i] = static_cast<uint8_t>(ids[i] >> 8);
        cfg[9 + 3 * i] = 1;
    }
    ActorDecl a{kPortableNode, ActorType::Ticker, 16, cfg};
    TickerConfig tc{};
    CHECK(ticker_config(a, tc));
    CHECK_EQ(tc.count, 3);
    CHECK_EQ(tc.node[2], 0x8160);
    a.cfg_len = 13;  // count says 3, bytes say 2
    CHECK(!ticker_config(a, tc));
    a.cfg_len = 16;
    cfg[10] = cfg[7];  // node 1 := node 0: listed twice
    cfg[11] = cfg[8];
    CHECK(!ticker_config(a, tc));
}

// ---- the cell ----------------------------------------------------------------------------------

TEST(reconcile, three_nodes_agree_and_exactly_one_runs_the_actor) {
    RCell c;
    c.build(3, {});
    c.run(4000);
    CHECK_EQ(c.running_count(), 1u);
    const int r = c.runner();
    CHECK(r >= 0);
    for (const RNode& t : c.nodes) {
        CHECK(t.rec->settled());
        CHECK_EQ(t.rec->view(0).owner, c.nodes[static_cast<size_t>(r)].id);
        CHECK_EQ(t.rec->view(0).assigned, c.nodes[static_cast<size_t>(r)].id);
    }
}

TEST(reconcile, killing_the_runner_moves_the_actor_and_reads_follow_within_2_s) {
    RCell c;
    c.build(3, {1, 1, 2});  // gravity: node 2 is where the actor belongs
    c.run(4000);
    CHECK_EQ(c.runner(), 2);
    Node& consumer = *c.nodes[0].node;

    c.kill(2);
    uint32_t served_after_ms = 0;
    uint32_t elapsed = 0;
    uint32_t next_read = 0;
    c.run(3000, [&] {
        ++elapsed;
        if (served_after_ms != 0) return;
        // The consumer reads the way A's handle_read does for the host: from cache, refreshing from
        // whichever node it currently accepts as owner.
        if (elapsed >= next_read) {
            next_read = elapsed + 50;
            const uint16_t owner = c.nodes[0].rec->view(0).owner;
            if (owner != 0 && owner != consumer.config().node_id) consumer.request_read(owner, kKey);
        }
        Reading rd;
        if (consumer.read(kKey, rd) == NsError::Ok && rd.quality == Quality::Good) {
            uint32_t v = 0;
            if (rd.value.as_u32(v) && (v >> 16) != c.nodes[2].id) served_after_ms = elapsed;
        }
    });
    CHECK(served_after_ms > 0);
    CHECK(served_after_ms <= 2000);
    std::printf("    actor served by another node %u ms after the kill\n", served_after_ms);
    CHECK_EQ(c.running_count(), 1u);
}

TEST(reconcile, the_killed_node_returns_and_nothing_double_runs) {
    RCell c;
    c.build(3, {1, 1, 2});
    c.run(4000);
    CHECK_EQ(c.runner(), 2);
    c.kill(2);
    c.run(3000);
    const int interim = c.runner();
    CHECK(interim == 0 || interim == 1);

    c.revive(2);
    size_t worst = 0;
    c.run(6000, [&] { worst = c.running_count() > worst ? c.running_count() : worst; });
    CHECK_EQ(worst, 1u);  // never two at once, at any millisecond
    // The returning node has the most gravity, so it gets the actor back -- by handover.
    CHECK_EQ(c.runner(), 2);
    CHECK_EQ(c.nodes[static_cast<size_t>(interim)].rec->view(0).released, 1u);
    CHECK_EQ(c.nodes[static_cast<size_t>(interim)].rec->view(0).fenced, 0u);
}

TEST(reconcile, a_returning_node_admitted_slowly_still_does_not_double_run) {
    // Found on the boards (M0-LOG session 26): C came back, admitted nobody for 3.4 s because a signed
    // HELLO takes that long, decided it was alone, and started the ticker A was running. Here the
    // returning node's HELLOs are lost for 3.5 s.
    RCell c;
    c.build(3, {1, 1, 2});
    c.run(4000);
    c.kill(2);
    c.run(3000);
    c.hello_blocked = 2;
    c.hello_blocked_until_us = c.now_us + 3500 * 1000;
    c.revive(2);
    size_t worst = 0;
    c.run(10000, [&] { worst = c.running_count() > worst ? c.running_count() : worst; });
    CHECK_EQ(worst, 1u);
    CHECK_EQ(c.runner(), 2);  // and it still gets the actor back, by handover, once admitted
}

TEST(reconcile, partition_duplicates_then_heal_fences_the_loser) {
    RCell c;
    c.build(3, {1, 1, 2});
    c.run(4000);
    CHECK_EQ(c.runner(), 2);

    // Node 2 alone on one side; 0 and 1 on the other.
    c.deaf = {{2, 0}, {2, 1}};
    c.run(3000);
    CHECK_EQ(c.running_count(), 2u);  // duplication: the price of refusing consensus
    const RNode& n2 = c.nodes[2];
    CHECK(n2.rec->view(0).running);

    // From here, a consumer's accepted owner term may never go down: that is "never accept the
    // losing instance" in a form a test can check at every millisecond.
    uint32_t floor_term[3] = {0, 0, 0};
    for (size_t i = 0; i < 3; ++i) floor_term[i] = c.nodes[i].rec->view(0).owner_term;
    bool regressed = false;
    uint32_t single_after = 0;
    uint32_t t = 0;
    c.deaf.clear();
    c.run(6000, [&] {
        ++t;
        for (size_t i = 0; i < 3; ++i) {
            const Reconciler::ActorView v = c.nodes[i].rec->view(0);
            if (v.owner != 0) {
                if (v.owner_term < floor_term[i]) regressed = true;
                floor_term[i] = v.owner_term;
            }
        }
        if (single_after == 0 && c.running_count() == 1) single_after = t;
    });
    CHECK(!regressed);
    CHECK(single_after > 0);
    std::printf("    duplication ended %u ms after the heal\n", single_after);
    CHECK_EQ(c.running_count(), 1u);
    // Node 2 lost the fence (its term was older), then got the actor back by gravity and handover.
    CHECK(c.nodes[2].rec->view(0).fenced >= 1u);
    CHECK_EQ(c.runner(), 2);
    for (const RNode& x : c.nodes) CHECK_EQ(x.rec->view(0).owner, c.nodes[2].id);
}

TEST(reconcile, a_late_reply_from_the_old_owner_is_fenced_out) {
    RCell c;
    c.build(3, {1, 1, 2});
    c.run(4000);
    Node& consumer = *c.nodes[0].node;
    // Pretend the consumer has since moved the resource to node 1, then ask node 2 anyway.
    consumer.ns().set_owner(kKey, c.nodes[1].id);
    const uint32_t before = consumer.ns_counters().replies_fenced;
    CHECK(consumer.request_read(c.nodes[2].id, kKey) != 0);
    c.run(20);  // a round trip is 2 x kLatencyMs
    CHECK_EQ(consumer.ns_counters().replies_fenced, before + 1);
}

TEST(reconcile, a_malformed_claim_set_is_counted_and_ignored) {
    RCell c;
    c.build(2, {});
    c.run(2000);
    Reconciler& r = *c.nodes[0].rec;
    const uint8_t bad[] = {kClaimVersion, 0, 2, 1, 2, 3};  // says two entries, carries none
    CHECK(r.on_cast(c.nodes[1].id, kReconcilePath, bad, sizeof(bad)));
    CHECK_EQ(r.counters().sets_malformed, 1u);
    CHECK(!r.on_cast(c.nodes[1].id, 0x1234u, bad, sizeof(bad)));  // not ours: left to the app
}

TEST(reconcile, host_and_node_agree_on_the_hash_and_the_image) {
    // The same numbers are asserted in host/potluck/tests/test_reconcile.py: if either side's weight
    // function or the ticker encoding drifts, one of the two suites fails.
    CHECK_EQ(kKey, 1245650351u);
    CHECK_EQ(hrw_weight(kKey, 0x6300), 411316836u);
    CHECK_EQ(hrw_weight(kKey, 0x7368), 117789623u);
    CHECK_EQ(hrw_weight(kKey, 0x8160), 48773440u);
    // manifests/m6-ticker.json as potluck.deploy compiles it: key, 100 ms, three nodes, C with gravity 2.
    const uint8_t cfg[] = {0xaf, 0x1d, 0x3f, 0x4a, 0x64, 0x00, 0x03, 0x00, 0x63, 0x01,
                           0x68, 0x73, 0x01, 0x60, 0x81, 0x02};
    const ActorDecl a{kPortableNode, ActorType::Ticker, sizeof(cfg), cfg};
    TickerConfig tc{};
    CHECK(ticker_config(a, tc));
    CHECK_EQ(tc.out_hash, kKey);
    CHECK_EQ(tc.period_ms, 100);
    uint16_t order[kMaxEligible];
    CHECK_EQ(rank_nodes(tc, [](void*, uint16_t) { return true; }, nullptr, order), 3u);
    CHECK_EQ(order[0], 0x8160);  // gravity: it owns the bound resource
    CHECK_EQ(order[1], 0x6300);
    CHECK_EQ(order[2], 0x7368);
}
