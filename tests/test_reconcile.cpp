// M6, section 7.7: the reconciler on real pot::Node objects over a perfect channel whose nodes can be
// killed, revived in a new incarnation, and partitioned. The acceptance lines are the tests:
//
//   "kill the node running a portable actor -- the actor serves reads from another node within 2 s;
//    when the killed node returns, it does not double-run. Partition and heal the mesh -- duplication
//    ends within gossip convergence, and fenced consumers never accept the losing instance."

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/ticker.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/actor.hpp"
#include "pot/reconcile.hpp"
#include "guest_fixture.hpp"
#include "pot/guest.hpp"
#include "pot/guest_examples.h"
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
    // M12: a stand-in for the board's NVS -- survives revive(), as flash survives a reboot.
    bool use_persist = false;
    std::map<uint32_t, std::vector<uint8_t>> nvs;  // key -> term u32, seq u32, data
    int nvs_writes = 0;
    CheckpointPersist persist{};
};

struct RCell {
    std::vector<RNode> nodes;
    std::vector<TickerConfig> actors;
    std::vector<PortableSpec> specs;  // M8.2: when set, loaded instead of `actors` (any portable kind)
    uint32_t need_caps = 0;  // M13: build()'s actor needs these HELLO capabilities
    uint32_t avoid_caps = 0;  // M13: ... and ranks nodes declaring these last
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
    size_t drop_from = SIZE_MAX, drop_to = SIZE_MAX;
    uint8_t drop_op = 0;
    uint32_t drop_until_us = 0;

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
        if (t.use_persist) {
            t.persist.ctx = &t;
            t.persist.interval_ms = 2000;
            t.persist.write = [](void* c, uint32_t key, uint32_t term, uint32_t seq, const uint8_t* d, size_t len) {
                RNode& n = *static_cast<RNode*>(c);
                std::vector<uint8_t> v(8 + len);
                std::memcpy(v.data(), &term, 4);
                std::memcpy(v.data() + 4, &seq, 4);
                if (len > 0) std::memcpy(v.data() + 8, d, len);
                n.nvs[key] = v;
                ++n.nvs_writes;
                return true;
            };
            t.persist.read = [](void* c, uint32_t key, uint32_t* term, uint32_t* seq, uint8_t* out, size_t cap,
                                size_t* len) {
                RNode& n = *static_cast<RNode*>(c);
                auto it = n.nvs.find(key);
                if (it == n.nvs.end() || it->second.size() - 8 > cap) return false;
                std::memcpy(term, it->second.data(), 4);
                std::memcpy(seq, it->second.data() + 4, 4);
                *len = it->second.size() - 8;
                std::memcpy(out, it->second.data() + 8, *len);
                return true;
            };
            t.persist.retain = [](void* c, const uint32_t* keys, size_t n) {
                RNode& r = *static_cast<RNode*>(c);
                for (auto it = r.nvs.begin(); it != r.nvs.end();) {
                    bool keep = false;
                    for (size_t i = 0; i < n; ++i) keep = keep || keys[i] == it->first;
                    it = keep ? std::next(it) : r.nvs.erase(it);
                }
            };
            t.rec->set_persist(&t.persist);
        }
        if (specs.empty()) {
            CHECK(t.rec->load(actors.data(), actors.size()));
        } else {
            CHECK(t.rec->load(specs.data(), specs.size()));
        }
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
        tc.need_caps = need_caps;
        tc.avoid_caps = avoid_caps;
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

    // `k` portable actors, every node eligible for each with equal gravity: the rendezvous hash spreads them.
    void build_many(size_t n, size_t k) {
        nodes.resize(n);
        actors.clear();
        for (size_t a = 0; a < k; ++a) {
            TickerConfig tc{};
            char path[48];
            std::snprintf(path, sizeof(path), "potluck://lab/act/job%u/out", static_cast<unsigned>(a));
            tc.out_hash = path_hash(path);
            tc.period_ms = 100;
            tc.count = static_cast<uint8_t>(n);
            for (size_t i = 0; i < n; ++i) {
                tc.node[i] = static_cast<uint16_t>(0x100 + i);
                tc.gravity[i] = 1;
            }
            actors.push_back(tc);
        }
        for (size_t i = 0; i < n; ++i) {
            RNode& t = nodes[i];
            t.cell = this;
            t.index = i;
            t.id = static_cast<uint16_t>(0x100 + i);
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(i);
        }
        for (RNode& t : nodes) boot(t);
    }
    size_t running_count_of(size_t a) const {
        size_t n = 0;
        for (const RNode& t : nodes) {
            if (!t.down && t.rec->view(a).running) ++n;
        }
        return n;
    }
    // Cut the cell into groups: every pair of nodes in different groups is deaf to each other.
    void split(const std::vector<int>& group_of) {
        deaf.clear();
        for (size_t i = 0; i < nodes.size(); ++i) {
            for (size_t j = i + 1; j < nodes.size(); ++j) {
                if (group_of[i] != group_of[j]) deaf.emplace_back(i, j);
            }
        }
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
            // One direction, one opcode, for a while: a lossy relay that loses a claim while heartbeats get through.
            const bool dropped = f.from == drop_from && f.to == drop_to && f.data.size() > 6 && f.data[6] == drop_op &&
                                 now_us < drop_until_us;
            const bool heard = !to.down && to.node != nullptr && !deaf_pair(f.from, f.to) && !blocked && !dropped;
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

// ---- M8.2 (PS-4): a portable actor's checkpoint survives a failover ---------------------------------

namespace {
// A portable actor that counts, saves the count once a second, and on start resumes from the
// checkpoint if there is one. What it loaded, per node, is kept for the test to read.
uint32_t g_ck_loaded[8];
uint32_t g_ck_loaded_age[8];
bool g_ck_had[8];
uint32_t g_ck_last_saved[8];
bool g_ck_save_once = false;  // a holder that saves once and then never again

struct Saver : Actor {
    Node& node;
    const CheckpointStore* ck;
    size_t idx;
    uint32_t count = 0;
    uint32_t next = 0;
    Saver(Node& n, const CheckpointStore* c) : node(n), ck(c), idx(n.config().node_id - 0x100) {}
    bool start(uint32_t now) override {
        uint8_t buf[kMaxCheckpoint];
        size_t len = 0;
        uint32_t age = 0;
        g_ck_had[idx] = ck != nullptr && ck->load(ck->ctx, buf, sizeof(buf), &len, &age) && len == 4;
        if (g_ck_had[idx]) {
            std::memcpy(&count, buf, 4);
            g_ck_loaded[idx] = count;
            g_ck_loaded_age[idx] = age;
        }
        next = now;
        return true;
    }
    void tick(uint32_t now) override {
        if (static_cast<int32_t>(now - next) < 0) return;
        next = now + 100;
        ++count;
        if (g_ck_save_once && g_ck_last_saved[idx] != 0) return;  // one save per holder, then quiet
        if (ck != nullptr && ck->save(ck->ctx, reinterpret_cast<const uint8_t*>(&count), 4)) {
            g_ck_last_saved[idx] = count;
        }
    }
};
bool saver_check(const ActorDecl&, const ActorEnv&, const char**) { return true; }
Actor* saver_create(void* mem, const ActorDecl&, const ActorEnv& env) {
    return new (mem) Saver(*env.node, env.checkpoint);
}
const ActorKind kSaverKind = {static_cast<ActorType>(0x82), "saver", &saver_check, &saver_create};

void saver_specs(RCell& c, size_t n) {
    PortableSpec sp;
    sp.place.out_hash = kKey;
    sp.place.period_ms = 100;
    sp.place.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        sp.place.node[i] = static_cast<uint16_t>(0x100 + i);
        sp.place.gravity[i] = 1;
    }
    sp.kind = &kSaverKind;
    sp.decl = ActorDecl{kPortableNode, static_cast<ActorType>(0x82), 0, sp.cfg};
    c.specs.assign(1, sp);
}
}  // namespace

TEST(reconcile, a_moved_actor_resumes_from_the_last_checkpoint_with_its_age) {
    std::memset(g_ck_loaded, 0, sizeof(g_ck_loaded));
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    std::memset(g_ck_last_saved, 0, sizeof(g_ck_last_saved));
    RCell c;
    saver_specs(c, 3);
    c.build(3, {});
    c.run(8000);
    const int first = c.runner();
    CHECK(first >= 0);
    if (first < 0) return;
    CHECK(!g_ck_had[first]);  // nothing to resume from at the very start
    const uint32_t saved = g_ck_last_saved[first];
    CHECK(saved > 0);
    c.run(300);  // no new save in the last 300 ms: the rate limit is 1 s, so "saved" is the last
    c.kill(static_cast<size_t>(first));
    c.run(4000);
    const int second = c.runner();
    CHECK(second >= 0 && second != first);
    if (second < 0) return;
    CHECK(g_ck_had[second]);
    CHECK(g_ck_loaded[second] == saved || g_ck_loaded[second] + 10 >= saved);  // the newest that reached it
    CHECK(g_ck_loaded_age[second] < 3000u);
    CHECK(c.nodes[static_cast<size_t>(second)].rec->counters().checkpoints_accepted > 0u);
}

TEST(reconcile, a_checkpoint_from_a_fenced_instance_never_overwrites_the_winners) {
    RCell c;
    saver_specs(c, 2);
    c.build(2, {});
    c.run(6000);
    const int holder = c.runner();
    CHECK(holder >= 0);
    if (holder < 0) return;
    const size_t other = holder == 0 ? 1 : 0;
    Reconciler& r = *c.nodes[other].rec;
    const uint32_t accepted = r.counters().checkpoints_accepted;
    const uint32_t refused = r.counters().checkpoints_refused;
    // A stale instance -- the non-holder itself, at term 1 -- claims to have a newer snapshot.
    uint8_t buf[kCheckpointHeaderLen + 4] = {};
    const uint32_t key = kKey, term = 0, seq = 999;
    std::memcpy(buf, &key, 4);
    std::memcpy(buf + 4, &term, 4);
    std::memcpy(buf + 8, &seq, 4);
    buf[12] = 4;
    CHECK(r.on_cast(c.nodes[other].id, kCheckpointPath, buf, sizeof(buf)));
    CHECK_EQ(r.counters().checkpoints_refused, refused + 1u);
    CHECK_EQ(r.counters().checkpoints_accepted, accepted);
    // And a malformed one is refused, not read past its end.
    CHECK(r.on_cast(c.nodes[holder].id, kCheckpointPath, buf, 10));
    CHECK_EQ(r.counters().checkpoints_refused, refused + 2u);
}

TEST(reconcile, a_node_that_comes_back_and_takes_the_actor_over_resumes_from_the_holders_checkpoint) {
    // Found on the bench (M8.2): the preferred node rebooted, took rf_fusion back at 2.4 s, and had
    // nothing to resume from -- it had been down for every save. The holder now hands its newest
    // checkpoint to a node the moment it reappears. Here the holder saves ONCE, long before the
    // preferred node returns, so only that hand-over can bring it.
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    std::memset(g_ck_last_saved, 0, sizeof(g_ck_last_saved));
    g_ck_save_once = true;
    RCell c;
    saver_specs(c, 2);
    c.specs[0].place.gravity[0] = 1;
    c.specs[0].place.gravity[1] = 9;  // node 1 is where the actor wants to be
    c.build(2, {});
    c.run(8000);
    CHECK_EQ(c.runner(), 1);
    c.kill(1);
    c.run(6000);
    CHECK_EQ(c.runner(), 0);
    CHECK(g_ck_last_saved[0] > 0);  // node 0 saved its one checkpoint while holding
    c.run(3000);                    // ... and nothing since
    c.revive(1);
    g_ck_had[1] = false;
    c.run(10000);
    CHECK_EQ(c.runner(), 1);  // back where it wants to be
    CHECK(g_ck_had[1]);       // and it resumed instead of starting over
    CHECK_EQ(g_ck_loaded[1], g_ck_last_saved[0]);
    g_ck_save_once = false;
}

TEST(reconcile, a_node_still_hunting_for_its_channel_does_not_settle_until_it_knows_it_or_the_cap) {
    // Found on the bench (M0-LOG session 33): the extender settled 2.6 s after booting, its station's
    // association then hid a peer from it for 600 ms, and it started the actor that peer was running.
    RCell c;
    c.build(3, {});
    c.run(5000);
    c.kill(0);
    c.run(2000);
    c.revive(0);
    c.nodes[0].node->set_channel_known(false);  // booted hunting
    c.run(3000);
    CHECK(!c.nodes[0].rec->settled());  // heard everyone long ago, but cannot trust its view yet
    c.nodes[0].node->set_channel_known(true);  // associated
    c.run(300);
    CHECK(c.nodes[0].rec->settled());

    // And one whose station never associates still settles at the cap.
    c.kill(1);
    c.run(2000);
    c.revive(1);
    c.nodes[1].node->set_channel_known(false);
    c.run(5000);
    CHECK(!c.nodes[1].rec->settled());
    c.run(1500);
    CHECK(c.nodes[1].rec->settled());
}

TEST(reconcile, a_late_joining_high_gravity_node_that_settles_at_the_cap_never_runs_beside_the_holder) {
    // M10 on the bench: the PC (top gravity, channel unknown, so it settles only at settle_max_ms)
    // joined a cell whose ticker A held, and started it 6 s later while A still ran it, for 8.5 s.
    RCell c;
    c.build(4, {1, 1, 1, 9});
    c.kill(3);
    c.run(8000);
    CHECK(c.runner() >= 0 && c.runner() < 3);
    c.revive(3);
    c.nodes[3].node->set_channel_known(false);
    int most = 0;
    for (int k = 0; k < 15000; ++k) {
        c.run(1);
        int n = 0;
        for (const RNode& t : c.nodes) {
            if (!t.down && t.rec->view(0).running) ++n;
        }
        if (n > most) most = n;
    }
    CHECK_EQ(most, 1);         // never two
    CHECK_EQ(c.runner(), 3);   // and it did move to the node gravity prefers
}

// ---- M7: a guest actor fails over like any portable actor, and continues from its checkpoint -----------

TEST(reconcile, a_guest_moves_when_its_node_dies_and_counts_on_from_its_checkpoint) {
    guest_runtime().reset();
    guestfx::Keys k;
    const uint32_t temp = path_hash("potluck://lab/node-0100/hw/temp");  // nobody owns it: Unavailable
    const uint32_t out = path_hash("potluck://lab/act/alarm/out");
    const uint32_t alarm = path_hash("potluck://lab/act/alarm/alarm");
    const uint32_t count = path_hash("potluck://lab/act/alarm/count");
    const guestfx::Bytes blob = guestfx::make_blob(
        k, guest_examples::k_overheat_alarm, sizeof(guest_examples::k_overheat_alarm), {temp},
        {{out, ValueType::F32}, {alarm, ValueType::I32}, {count, ValueType::I32}});
    const guestfx::Bytes image = guestfx::make_image({guestfx::guest_decl(kKey, 100, {0x100, 0x101, 0x102}, 0)}, {blob});
    DeployImage img{};
    const char* why = nullptr;
    CHECK(parse_image(image.data(), image.size(), img, &why));
    static uint8_t pool[kMaxImageLen];
    CHECK(guest_library().load(img, pool, sizeof(pool), &why));
    guest_runtime().set_ca(k.ca.pub);
    {
        RCell c;
        PortableSpec specs[kMaxPortable];
        size_t n = 0;
        CHECK(collect_portable(img, &kGuestKind, 1, specs, kMaxPortable, n, &why));
        CHECK_EQ(n, 1u);
        c.specs.assign(specs, specs + n);
        c.build(3, {});
        auto guest_task = [] { guest_runtime().service(); };
        auto count_on = [&](size_t i) {
            Reading r;
            int32_t v = -1;
            Quality q;
            c.nodes[i].node->read(count, r);
            r.get_i32(v, q);
            return v;
        };
        c.run(8000, guest_task);
        const int first = c.runner();
        CHECK(first >= 0);
        if (first < 0) return;
        const int32_t before = count_on(static_cast<size_t>(first));
        CHECK(before > 30);  // ~10 ticks a second once placed
        c.kill(static_cast<size_t>(first));
        c.run(4000, guest_task);
        const int second = c.runner();
        CHECK(second >= 0 && second != first);
        if (second < 0) return;
        const int32_t after = count_on(static_cast<size_t>(second));
        CHECK(after > before);  // continued from the checkpoint (a cold start would be ~20 by now)
        const auto* g = static_cast<const GuestActor*>(c.nodes[static_cast<size_t>(second)].rec->instance(0));
        CHECK(g != nullptr);
        if (g == nullptr) return;
        CHECK_EQ(g->stats().fresh, 1u);
        CHECK_EQ(g->stats().failed, 0u);
        CHECK_EQ(g->stats().log_last, 1);  // its init() found the checkpoint
    }
    guest_runtime().service();
    guest_library().clear();
    guest_runtime().reset();
}

// ---- M12: live package changes, and checkpoints that survive a whole-cell reboot ------------------------

namespace {
PortableSpec saver_spec(uint32_t key, size_t n_nodes) {
    PortableSpec sp;
    sp.place.out_hash = key;
    sp.place.period_ms = 100;
    sp.place.count = static_cast<uint8_t>(n_nodes);
    for (size_t i = 0; i < n_nodes; ++i) {
        sp.place.node[i] = static_cast<uint16_t>(0x100 + i);
        sp.place.gravity[i] = 1;
    }
    sp.kind = &kSaverKind;
    // The key in the config too, so two savers are two different declarations.
    sp.cfg[0] = static_cast<uint8_t>(key);
    sp.cfg[1] = static_cast<uint8_t>(key >> 8);
    sp.decl = ActorDecl{kPortableNode, static_cast<ActorType>(0x82), 2, sp.cfg};
    return sp;
}
int runner_of(const RCell& c, size_t slot) {
    for (const RNode& t : c.nodes) {
        if (!t.down && t.rec->view(slot).running) return static_cast<int>(t.index);
    }
    return -1;
}
}  // namespace

TEST(reconcile, a_live_reload_keeps_the_unchanged_actor_running_and_swaps_the_rest_everywhere) {
    const uint32_t k1 = path_hash("potluck://lab/act/one/out"), k2 = path_hash("potluck://lab/act/two/out"),
                   k3 = path_hash("potluck://lab/act/three/out");
    RCell c;
    c.specs = {saver_spec(k1, 3), saver_spec(k2, 3)};
    c.build(3, {});
    c.run(8000);
    const int r1 = runner_of(c, 0);
    CHECK(r1 >= 0);
    if (r1 < 0) return;
    Actor* inst = c.nodes[static_cast<size_t>(r1)].rec->instance(0);
    const uint32_t count_before = static_cast<Saver*>(inst)->count;
    // Every node takes the new package live, one after another, as a deploy's push would deliver it.
    const std::vector<PortableSpec> next = {saver_spec(k1, 3), saver_spec(k3, 3)};
    for (RNode& t : c.nodes) {
        Reconciler::ReloadStats st;
        CHECK(t.rec->reload(next.data(), next.size(), c.now_us / 1000, &st));
        CHECK_EQ(st.kept, static_cast<size_t>(1));
        CHECK_EQ(st.stopped, static_cast<size_t>(1));
        CHECK_EQ(st.added, static_cast<size_t>(1));
        CHECK(t.node->ns().find(k2) == nullptr);  // the removed actor's output is gone, not aging
        c.run(50);
    }
    c.run(6000);
    CHECK(c.nodes[static_cast<size_t>(r1)].rec->instance(0) == inst);  // the same instance, never restarted
    CHECK(static_cast<Saver*>(inst)->count > count_before + 50);         // and still counting
    int running_k3 = 0;
    for (const RNode& t : c.nodes) {
        for (size_t i = 0; i < t.rec->actor_count(); ++i) {
            if (t.rec->view(i).key == k3 && t.rec->view(i).running) ++running_k3;
            CHECK(t.rec->view(i).key != k2);
        }
    }
    CHECK_EQ(running_k3, 1);  // the new actor placed, exactly once
}

TEST(reconcile, a_checkpoint_survives_a_reboot_of_every_node_at_once) {
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    std::memset(g_ck_last_saved, 0, sizeof(g_ck_last_saved));
    const uint32_t k = path_hash("potluck://lab/act/persist/out");
    RCell c;
    c.specs = {saver_spec(k, 3)};
    c.nodes.resize(3);
    for (RNode& t : c.nodes) t.use_persist = true;
    c.build(3, {});
    c.run(12000);
    const int first = runner_of(c, 0);
    CHECK(first >= 0);
    if (first < 0) return;
    const uint32_t saved = g_ck_last_saved[first];
    CHECK(saved > 50);
    int writes = 0;
    for (const RNode& t : c.nodes) writes += t.nvs_writes;
    CHECK(writes > 0);
    CHECK(writes < 3 * 8);  // at most one a node per 2 s interval, not one per save
    // Every board loses power at once: RAM is gone, the stand-in NVS is not.
    for (size_t i = 0; i < c.nodes.size(); ++i) c.kill(i);
    c.run(100);
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    for (size_t i = 0; i < c.nodes.size(); ++i) c.revive(i);
    c.run(12000);
    const int second = runner_of(c, 0);
    CHECK(second >= 0);
    if (second < 0) return;
    CHECK(g_ck_had[second]);  // it resumed, instead of starting from nothing
    CHECK(g_ck_loaded[second] + 30 >= saved);  // from no older than one persist interval (2 s = 20 saves) back
    // And the new activation's saves outrank what was restored: the count keeps moving after the reboot.
    CHECK(g_ck_last_saved[second] > g_ck_loaded[second]);
}

TEST(reconcile, a_live_change_of_an_actors_declaration_resumes_it_from_its_checkpoint) {
    // The bench (M0-LOG session 39): a package that changed only a guest's eligible nodes restarted its count
    // from zero at term 1. Same identity and type, a new declaration: a new instance, from the checkpoint.
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    std::memset(g_ck_last_saved, 0, sizeof(g_ck_last_saved));
    const uint32_t k = path_hash("potluck://lab/act/moved/out");
    RCell c;
    c.specs = {saver_spec(k, 3)};
    c.build(3, {});
    c.run(8000);
    const int first = runner_of(c, 0);
    CHECK(first >= 0);
    if (first < 0) return;
    const uint32_t saved = g_ck_last_saved[first];
    const uint32_t term_before = c.nodes[static_cast<size_t>(first)].rec->view(0).term;
    CHECK(saved > 20);
    std::memset(g_ck_had, 0, sizeof(g_ck_had));
    const std::vector<PortableSpec> next = {saver_spec(k, 2)};  // eligible: the first two nodes only
    for (RNode& t : c.nodes) {
        Reconciler::ReloadStats st;
        CHECK(t.rec->reload(next.data(), next.size(), c.now_us / 1000, &st));
        CHECK_EQ(st.stopped, static_cast<size_t>(1));
        CHECK_EQ(st.added, static_cast<size_t>(1));
        CHECK_EQ(st.carried, static_cast<size_t>(1));
    }
    c.run(6000);
    const int second = runner_of(c, 0);
    CHECK(second >= 0 && second < 2);
    if (second < 0) return;
    CHECK(g_ck_had[second]);                    // resumed, not started from nothing
    CHECK(g_ck_loaded[second] + 10 >= saved);   // from where it was
    CHECK(c.nodes[static_cast<size_t>(second)].rec->view(0).term > term_before);  // and its terms move on
}

TEST(reconcile, an_actor_that_needs_a_connection_runs_only_where_one_is_declared_and_moves_with_it) {
    // M13: the presenter's placement -- the review's item 3. Gravity says node 2; only connected nodes qualify.
    RCell c;
    c.need_caps = kHelloCapConnected;
    c.build(3, {1, 1, 2});
    c.run(4000);
    CHECK_EQ(c.running_count(), 0u);  // nobody connected: nobody presents
    c.nodes[0].node->set_connected(true);
    c.run(3000);
    CHECK_EQ(c.runner(), 0);
    CHECK_EQ(c.running_count(), 1u);
    c.nodes[1].node->set_connected(true);
    c.run(3000);
    CHECK_EQ(c.running_count(), 1u);  // two qualify: still exactly one runs (the rendezvous hash picks)
    c.nodes[c.runner() == 0 ? 0 : 1].node->set_connected(false);  // the holder's Wi-Fi goes
    c.run(3000);
    CHECK_EQ(c.running_count(), 1u);
    CHECK(c.runner() == 0 || c.runner() == 1);
    CHECK(c.nodes[static_cast<size_t>(c.runner())].node->connected());
}

TEST(reconcile, a_busy_node_is_ranked_last_but_still_takes_the_actor_when_it_is_the_only_one) {
    // M13: the presenter avoids the extender (BUSY) when another connected board is alive, and falls back to it.
    RCell c;
    c.need_caps = kHelloCapConnected;
    c.avoid_caps = kHelloCapBusy;
    c.build(3, {1, 1, 3});  // gravity alone would choose node 2
    c.nodes[2].node->set_connected(true);
    c.nodes[2].node->set_busy(true);  // the extender
    c.nodes[1].node->set_connected(true);
    c.run(5000);
    CHECK_EQ(c.runner(), 1);  // the board with nothing else on it, despite gravity
    c.kill(1);
    c.run(4000);
    CHECK_EQ(c.runner(), 2);  // the only connected board left: the extender presents
    CHECK_EQ(c.running_count(), 1u);
}

// ---- M14: a placement host judges only the actors it can be placed on -----------------------------------

TEST(reconcile, a_placement_host_skips_portable_actors_it_is_not_eligible_for) {
    // A cell's package carries board-only portable actors (an app's external ones, the presenter) whose types a PC
    // build does not have; pot_hostnode collects only those whose eligible set names it (found on the bench,
    // M0-LOG session 42: the host refused the whole image).
    TickerConfig mine{};
    mine.out_hash = path_hash("potluck://lab/act/ticker/out");
    mine.period_ms = 100;
    mine.count = 2;
    mine.node[0] = 0x00FE;
    mine.node[1] = 0x0100;
    TickerConfig boards = mine;
    boards.out_hash = path_hash("potluck://lab/act/present/out");
    boards.node[0] = 0x0100;
    boards.node[1] = 0x0101;
    static uint8_t c1[64], c2[64];
    DeployImage img{};
    img.actor_count = 2;
    img.actors[0] = ActorDecl{kPortableNode, ActorType::Ticker, static_cast<uint8_t>(encode_ticker_config(mine, c1, sizeof(c1))), c1};
    img.actors[1] = ActorDecl{kPortableNode, ActorType::Present, static_cast<uint8_t>(encode_ticker_config(boards, c2, sizeof(c2))), c2};
    PortableSpec specs[kMaxPortable];
    size_t n = 0;
    const char* why = nullptr;
    CHECK(!collect_portable(img, &kTickerKind, 1, specs, kMaxPortable, n, &why));  // a board: an unknown type is a fault
    CHECK(collect_portable(img, &kTickerKind, 1, specs, kMaxPortable, n, &why, 0x00FE));
    CHECK_EQ(n, static_cast<size_t>(1));
    CHECK_EQ(specs[0].place.out_hash, mine.out_hash);
}

// ---- eight nodes, split many ways (the owner's question, 2026-10-14: "how does a severed potluck network behave,
// and what happens when it reconnects?"). The boards are three; these are eight real Nodes and Reconcilers over the
// simulated channel above, every one eligible for four portable jobs. What is checked at every millisecond, not just
// at the end: no node ever accepts a lower owner term for a job than it accepted before (never the losing copy); and
// at the end of every heal, each job runs on exactly one node and every node names that node as its owner.

namespace {

constexpr size_t kEight = 8;
constexpr size_t kJobs = 4;

struct Fence {
    RCell& c;
    std::vector<uint32_t> floor = std::vector<uint32_t>(kEight * kJobs, 0);
    bool regressed = false;
    explicit Fence(RCell& cell) : c(cell) { reset(); }
    // A rebooted node starts again from nothing: its floor starts again too.
    void reset() {
        for (size_t i = 0; i < kEight; ++i)
            for (size_t a = 0; a < kJobs; ++a) floor[i * kJobs + a] = c.nodes[i].down ? 0 : c.nodes[i].rec->view(a).owner_term;
    }
    void step() {
        for (size_t i = 0; i < kEight; ++i) {
            if (c.nodes[i].down) continue;
            for (size_t a = 0; a < kJobs; ++a) {
                const Reconciler::ActorView v = c.nodes[i].rec->view(a);
                if (v.owner == 0) continue;
                if (v.owner_term < floor[i * kJobs + a]) regressed = true;
                floor[i * kJobs + a] = v.owner_term;
            }
        }
    }
};

// Every job on exactly one live node, and every live node agreeing on which.
bool settled(const RCell& c) {
    for (size_t a = 0; a < kJobs; ++a) {
        if (c.running_count_of(a) != 1) return false;
        uint16_t owner = 0;
        for (const RNode& t : c.nodes) {
            if (t.down) continue;
            const Reconciler::ActorView v = t.rec->view(a);
            if (owner == 0) owner = v.owner;
            if (v.owner == 0 || v.owner != owner) return false;
        }
    }
    return true;
}

size_t copies(const RCell& c) {
    size_t n = 0;
    for (size_t a = 0; a < kJobs; ++a) n += c.running_count_of(a);
    return n;
}

// Run `ms` checking the fence every millisecond; the first millisecond the cell is settled (0: never).
// Also every later millisecond it was not (a handover back to a job's preferred node passes through an unsettled
// moment): `last_unsettled` is the last one, `extra_ms` how many milliseconds any job ran on two nodes after the first
// settled moment.
uint32_t g_last_unsettled = 0, g_extra_ms = 0;
uint32_t heal_watch(RCell& c, Fence& f, uint32_t ms) {
    uint32_t t = 0, settled_at = 0;
    g_last_unsettled = 0;
    g_extra_ms = 0;
    c.run(ms, [&] {
        ++t;
        f.step();
        const bool s = settled(c);
        if (settled_at == 0 && s) settled_at = t;
        if (!s) g_last_unsettled = t;
        if (settled_at != 0 && copies(c) > kJobs) ++g_extra_ms;
    });
    return settled_at;
}

}  // namespace

TEST(partition8, four_pairs_each_run_their_own_copy_and_the_heal_leaves_one_of_each) {
    RCell c;
    c.build_many(kEight, kJobs);
    c.run(8000);
    CHECK(settled(c));
    Fence f(c);
    c.split({0, 0, 1, 1, 2, 2, 3, 3});
    uint32_t t = 0, all_four_at = 0;
    c.run(10000, [&] {
        ++t;
        f.step();
        if (all_four_at == 0 && copies(c) == kJobs * 4) all_four_at = t;
    });
    // Each pair, alone, runs every job: four copies of each (no quorum: section 7.3).
    for (size_t a = 0; a < kJobs; ++a) CHECK_EQ(c.running_count_of(a), static_cast<size_t>(4));
    CHECK(all_four_at > 0);
    c.deaf.clear();
    const uint32_t healed = heal_watch(c, f, 15000);
    CHECK(!f.regressed);
    CHECK(healed > 0);
    CHECK(settled(c));
    CHECK_EQ(g_extra_ms, 0u);
    std::printf("      last unsettled %u ms after the heal; %u ms with a second copy after the first settled\n",
                g_last_unsettled, g_extra_ms);
    std::printf("    4 pairs: every pair running all %u jobs %u ms after the split (16 copies); one of each "
                "%u ms after the heal\n", static_cast<unsigned>(kJobs), all_four_at, healed);
}

TEST(partition8, uneven_splits_heal_the_same_way) {
    const std::vector<std::vector<int>> shapes = {
        {0, 1, 1, 1, 1, 1, 1, 1},  // one alone
        {0, 0, 1, 1, 1, 1, 1, 1},  // two and six
        {0, 0, 0, 1, 1, 1, 1, 1},  // three and five
        {0, 1, 2, 0, 1, 2, 0, 1},  // three groups, interleaved ids
    };
    for (const auto& shape : shapes) {
        RCell c;
        c.build_many(kEight, kJobs);
        c.run(8000);
        CHECK(settled(c));
        Fence f(c);
        c.split(shape);
        c.run(8000, [&] { f.step(); });
        int groups = 0;
        for (int g : shape) groups = g + 1 > groups ? g + 1 : groups;
        for (size_t a = 0; a < kJobs; ++a) CHECK_EQ(c.running_count_of(a), static_cast<size_t>(groups));
        c.deaf.clear();
        const uint32_t healed = heal_watch(c, f, 15000);
        CHECK(!f.regressed);
        CHECK(healed > 0);
        CHECK_EQ(g_extra_ms, 0u);
        std::printf("      last unsettled %u ms after the heal; %u ms with a second copy after the first settled\n",
                    g_last_unsettled, g_extra_ms);
        std::printf("    %d groups (%d alone first): %u copies in the split, one of each %u ms after the heal\n",
                    groups, static_cast<int>(std::count(shape.begin(), shape.end(), 0)),
                    static_cast<unsigned>(groups * kJobs), healed);
    }
}

TEST(partition8, groups_merge_one_step_at_a_time) {
    RCell c;
    c.build_many(kEight, kJobs);
    c.run(8000);
    Fence f(c);
    c.split({0, 0, 1, 1, 2, 2, 3, 3});
    c.run(6000, [&] { f.step(); });
    CHECK_EQ(copies(c), kJobs * 4);
    c.split({0, 0, 0, 0, 1, 1, 1, 1});  // the pairs meet two by two
    c.run(6000, [&] { f.step(); });
    CHECK_EQ(copies(c), kJobs * 2);
    c.deaf.clear();
    const uint32_t healed = heal_watch(c, f, 15000);
    CHECK(!f.regressed);
    CHECK(healed > 0);
    CHECK_EQ(g_extra_ms, 0u);
    std::printf("      last unsettled %u ms after the heal; %u ms with a second copy after the first settled\n",
                g_last_unsettled, g_extra_ms);
    std::printf("    4 -> 2 -> 1 groups: 16 -> 8 -> 4 copies, one of each %u ms after the last heal\n", healed);
}

TEST(partition8, links_that_flap_never_let_a_node_go_back_to_a_losing_copy) {
    RCell c;
    c.build_many(kEight, kJobs);
    c.run(8000);
    Fence f(c);
    uint32_t seed = 0x5eed;
    auto rnd = [&seed](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % n;
    };
    for (int round = 0; round < 25; ++round) {  // 25 random splits into 2-4 groups, 1.5 s each, 1 s apart
        std::vector<int> shape(kEight);
        const uint32_t groups = 2 + rnd(3);
        for (size_t i = 0; i < kEight; ++i) shape[i] = static_cast<int>(rnd(groups));
        c.split(shape);
        c.run(1500, [&] { f.step(); });
        c.deaf.clear();
        c.run(1000, [&] { f.step(); });
    }
    const uint32_t healed = heal_watch(c, f, 15000);
    CHECK(!f.regressed);
    CHECK(healed > 0);
    CHECK_EQ(g_extra_ms, 0u);
    std::printf("      last unsettled %u ms after the heal; %u ms with a second copy after the first settled\n",
                g_last_unsettled, g_extra_ms);
    std::printf("    25 random splits: no node ever took a lower term; one of each %u ms after the last\n", healed);
}

TEST(partition8, a_node_that_reboots_inside_a_split_rejoins_without_a_second_copy) {
    RCell c;
    c.build_many(kEight, kJobs);
    c.run(8000);
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(4000);
    c.kill(5);
    c.run(2000);
    c.revive(5);  // a new incarnation, inside the minority side
    c.run(6000);
    Fence f(c);
    c.deaf.clear();
    const uint32_t healed = heal_watch(c, f, 15000);
    CHECK(!f.regressed);
    CHECK(healed > 0);
    CHECK_EQ(g_extra_ms, 0u);
    std::printf("      last unsettled %u ms after the heal; %u ms with a second copy after the first settled\n",
                g_last_unsettled, g_extra_ms);
    std::printf("    a reboot inside the split: one of each %u ms after the heal\n", healed);
}

TEST(reconcile, a_holder_that_takes_an_actor_back_offers_the_handover_again) {
    // Found on the bench (M14, M0-LOG session 42): C released the presenting role to the PC, the PC's first claim
    // was lost on the relay while heartbeats got through, so C saw the role unclaimed and started it again -- at the
    // same term the PC had just taken, winning the tie on node id. C kept the role for good: it still remembered
    // having released it to the PC, and only a change of membership cleared that, which never came.
    RCell c;
    c.build(3, {2, 1, 1});
    c.kill(0);
    c.run(8000);
    const int holder = c.runner();
    CHECK(holder == 1 || holder == 2);
    // The preferred node comes back. The moment the holder releases to it, the preferred node's claims to the
    // holder are lost for 1.5 s -- longer than the holder's orphan step -- while everything else arrives.
    const uint32_t released_before = c.nodes[static_cast<size_t>(holder)].rec->view(0).released;
    c.drop_from = 0;
    c.drop_to = static_cast<size_t>(holder);
    c.drop_op = kOpCast;
    c.revive(0);
    bool took_back = false;
    c.run(20000, [&] {
        const Reconciler::ActorView v = c.nodes[static_cast<size_t>(holder)].rec->view(0);
        if (c.drop_until_us == 0 && v.released > released_before) c.drop_until_us = c.now_us + 1500 * 1000;
        if (c.drop_until_us != 0 && v.running) took_back = true;
    });
    CHECK(c.drop_until_us != 0);  // the release happened
    CHECK(took_back);             // and the holder, hearing no claim, took it back: the bench's sequence
    // The preferred node holds it in the end, and only it.
    CHECK_EQ(c.running_count(), 1u);
    CHECK_EQ(c.runner(), 0);
}
