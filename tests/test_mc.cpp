// M9: borrowing an idle core. Three real pot::Nodes on a 2 ms channel: node 0 runs the job (and a
// lender), nodes 1 and 2 lend. Each node's background workers are simulated: a unit takes kUnitMs of
// simulated time, then the real pure function runs, so the hits are real and the timing is the test's.

#include <cstring>
#include <vector>

#include "pot/mc.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kOut = path_hash("potluck://lab/node-0100/job/mc/pi");
constexpr uint32_t kUnitMs = 200;
constexpr uint32_t kSamples = 2000;

struct MCell;
struct Exec {
    struct Job {
        void (*work)(void*);
        void* arg;
        uint32_t due_ms;
    };
    MCell* cell = nullptr;
    std::vector<Job> jobs;
    uint8_t workers = 2;
    uint32_t unit_ms = kUnitMs;
    bool black_hole = false;  // accepts work and never finishes it: an answer that never comes
};

struct MNode {
    MCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    uint32_t epoch = 1;
    bool down = false;
    bool lends = true;
    Node* node = nullptr;
    NodeHal hal{};
    Exec exec;
    BoardServices board;
    alignas(McLenderActor) uint8_t lender_mem[sizeof(McLenderActor)];
    McLenderActor* lender = nullptr;
};

struct MCell {
    MNode n[3];
    McJobActor* job = nullptr;
    uint32_t now_us = 0;
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    uint32_t now_ms() const { return now_us / 1000; }

    static bool bg_submit(void* ctx, void (*work)(void*), void* arg) {
        Exec* e = static_cast<Exec*>(ctx);
        if (e->jobs.size() >= e->workers) return false;
        e->jobs.push_back(Exec::Job{work, arg, e->black_hole ? 0xFFFFFFFFu : e->cell->now_ms() + e->unit_ms});
        return true;
    }
    static bool on_call(void* ctx, uint16_t from, uint16_t msg, uint32_t path, const uint8_t* a, uint16_t len) {
        MNode* t = static_cast<MNode*>(ctx);
        if (t->lender == nullptr) return false;
        return t->lender->on_call(from, msg, path, a, len) == Actor::kCallAccepted;
    }
    static void on_result(void* ctx, uint16_t from, uint16_t msg, uint32_t path, Node::CallOutcome o, const Value& v) {
        static_cast<MCell*>(ctx)->job->on_call_result(from, msg, path, o, v);
    }

    void boot(size_t i) {
        MNode& t = n[i];
        if (t.lender != nullptr) t.lender->~McLenderActor();
        t.lender = nullptr;
        delete t.node;
        t.exec.jobs.clear();
        NodeConfig cfg;
        cfg.node_id = static_cast<uint16_t>(0x100 + i);
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        cfg.probe_interval_ms = 100;
        t.hal.ctx = &t;
        t.hal.send = &MCell::send;
        t.hal.now_ms = [](void* c) { return static_cast<MNode*>(c)->cell->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<MNode*>(c)->cell->now_us; };
        t.node = new Node(cfg, t.hal);
        t.board = BoardServices{};
        t.board.ctx = &t.exec;
        t.board.bg_submit = &MCell::bg_submit;
        t.board.bg_workers = t.exec.workers;
        if (t.lends) {
            McLenderConfig lc{2};
            t.lender = new (t.lender_mem) McLenderActor(*t.node, &t.board, lc);
            CHECK(t.lender->start(now_ms()));
        }
        t.node->set_call_handler(&MCell::on_call, &t);
        t.node->start();
        t.down = false;
    }

    MCell(uint16_t units, bool lenders_lend = true) {
        for (size_t i = 0; i < 3; ++i) {
            n[i].cell = this;
            n[i].index = i;
            n[i].mac[0] = 0x02;
            n[i].mac[5] = static_cast<uint8_t>(0x30 + i);
            n[i].exec.cell = this;
            n[i].lends = (i == 0) || lenders_lend;
        }
        for (size_t i = 0; i < 3; ++i) boot(i);
        McJobConfig jc{kOut, units, kSamples, 12345, 0, 0};
        job = new McJobActor(*n[0].node, &n[0].board, jc);
        n[0].node->set_call_result(&MCell::on_result, this);
        CHECK(job->start(now_ms()));
    }
    ~MCell() {
        delete job;
        for (MNode& t : n) {
            if (t.lender != nullptr) t.lender->~McLenderActor();
            delete t.node;
        }
    }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        MNode* from = static_cast<MNode*>(ctx);
        MCell* c = from->cell;
        if (from->down) return 0;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t to = 0; to < 3; ++to) {
            if (to == from->index) continue;
            if (bcast || std::memcmp(c->n[to].mac, mac, kMacLen) == 0) {
                c->air.push_back(Flight{c->now_us + 2000, from->index, to, std::vector<uint8_t>(data, data + len)});
            }
        }
        return 0;
    }

    void kill(size_t i) {
        n[i].down = true;
        n[i].exec.jobs.clear();  // a reset loses whatever its cores were doing
    }
    void revive(size_t i) {
        ++n[i].epoch;
        boot(i);
    }

    void run(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (size_t i = 0; i < air.size();) {
                if (air[i].due_us <= now_us) {
                    Flight f = std::move(air[i]);
                    air.erase(air.begin() + static_cast<std::ptrdiff_t>(i));
                    MNode& to = n[f.to];
                    if (!to.down && !n[f.from].down) to.node->on_rx(n[f.from].mac, f.data.data(), f.data.size(), now_us, -50);
                } else {
                    ++i;
                }
            }
            for (MNode& t : n) {
                if (t.down) continue;
                for (size_t j = 0; j < t.exec.jobs.size();) {
                    if (t.exec.jobs[j].due_ms != 0xFFFFFFFFu && static_cast<int32_t>(now_ms() - t.exec.jobs[j].due_ms) >= 0) {
                        Exec::Job jb = t.exec.jobs[j];
                        t.exec.jobs.erase(t.exec.jobs.begin() + static_cast<std::ptrdiff_t>(j));
                        jb.work(jb.arg);
                    } else {
                        ++j;
                    }
                }
                t.node->tick(now_ms());
                if (t.lender != nullptr) t.lender->tick(now_ms());
            }
            if (!n[0].down) job->tick(now_ms());
        }
    }
    // Run until the job ends (or `cap` ms pass). Returns the simulated ms it took.
    uint32_t run_job(bool lend, uint32_t cap = 120000) {
        CHECK(job->run(lend, now_ms()));
        const uint32_t t0 = now_ms();
        for (uint32_t k = 0; k < cap && job->running(); ++k) run(1);
        CHECK(!job->running());
        return now_ms() - t0;
    }
};

uint64_t expected_hits(uint16_t units) {
    uint64_t h = 0;
    for (uint32_t i = 0; i < units; ++i) h += mc_hits(12345, i, kSamples, nullptr, nullptr);
    return h;
}

}  // namespace

TEST(mc, the_pure_function_is_deterministic_and_estimates_pi) {
    CHECK_EQ(mc_hits(1, 0, 10000, nullptr, nullptr), mc_hits(1, 0, 10000, nullptr, nullptr));
    CHECK(mc_hits(1, 0, 10000, nullptr, nullptr) != mc_hits(1, 1, 10000, nullptr, nullptr));  // units differ
    CHECK(mc_hits(1, 0, 10000, nullptr, nullptr) != mc_hits(2, 0, 10000, nullptr, nullptr));  // seeds differ
    const uint32_t h = mc_hits(7, 3, 1000000, nullptr, nullptr);
    const double pi = 4.0 * h / 1e6;
    CHECK(pi > 3.13 && pi < 3.153);  // 1e6 samples: the error is ~0.0016 at one sigma
    // The yield hook is called, and does not change the answer.
    struct Count {
        static void y(void* c) { ++*static_cast<uint32_t*>(c); }
    };
    uint32_t yields = 0;
    CHECK_EQ(mc_hits(7, 3, 1000000, &Count::y, &yields), h);
    CHECK_EQ(yields, 1000000u / kMcYieldEvery);
}

TEST(mc, a_job_alone_and_a_job_with_lenders_give_the_same_answer_and_lending_is_faster) {
    MCell c(60);
    c.run(3000);  // the cell forms; round trips are measured
    const uint32_t alone = c.run_job(false);
    const McJobActor::Result a = c.job->last();
    CHECK_EQ(a.hits, expected_hits(60));
    CHECK_EQ(static_cast<unsigned>(a.local_units), 60u);
    CHECK_EQ(static_cast<unsigned>(a.lent_units), 0u);
    // 60 units on two local workers at 200 ms: 30 rounds.
    CHECK(alone >= 30u * kUnitMs);

    const uint32_t lent = c.run_job(true);
    const McJobActor::Result b = c.job->last();
    CHECK_EQ(b.hits, a.hits);  // where a unit ran does not change the result -- to the last hit
    CHECK(b.lent_units > 0u);
    CHECK_EQ(static_cast<unsigned>(b.local_units + b.lent_units), 60u);
    CHECK(c.job->lent_to(0x101) > 0u);
    CHECK(c.job->lent_to(0x102) > 0u);
    CHECK(static_cast<uint64_t>(lent) * 3u <= static_cast<uint64_t>(alone) * 2u);  // at least 1.5x faster
    CHECK_EQ(static_cast<unsigned>(b.lost), 0u);
}

TEST(mc, a_lender_that_dies_mid_unit_loses_no_result) {
    MCell c(60);
    c.run(3000);
    CHECK(c.job->run(true, c.now_ms()));
    c.run(1500);  // lending under way
    CHECK(c.n[1].lender->stats().accepted > 0u);
    c.kill(1);    // held in reset with units on its cores
    for (uint32_t k = 0; k < 120000 && c.job->running(); ++k) c.run(1);
    CHECK(!c.job->running());
    const McJobActor::Result r = c.job->last();
    CHECK(r.lost > 0u);                   // the node told the job, and it ran them again
    CHECK_EQ(r.hits, expected_hits(60));  // every unit counted exactly once
    CHECK_EQ(static_cast<unsigned>(r.local_units + r.lent_units), 60u);
}

TEST(mc, a_busy_lender_refuses_and_the_job_runs_the_unit_elsewhere) {
    MCell c(40);
    c.run(3000);
    c.n[1].exec.workers = 0;  // node 1's cores are all taken by its own work
    c.n[1].board.bg_workers = 0;
    c.run_job(true);
    const McJobActor::Result r = c.job->last();
    CHECK(r.refused > 0u);
    CHECK_EQ(c.job->lent_to(0x101), 0u);
    CHECK_EQ(r.hits, expected_hits(40));
    CHECK(c.n[1].lender->stats().refused > 0u);
}

TEST(mc, a_peer_without_a_lender_is_asked_and_then_left_alone) {
    MCell c(60, /*lenders_lend=*/false);  // nodes 1 and 2 run no lender: every CALL is refused
    c.run(3000);
    c.run_job(true);
    const McJobActor::Result r = c.job->last();
    CHECK_EQ(static_cast<unsigned>(r.lent_units), 0u);
    CHECK_EQ(r.hits, expected_hits(60));
    CHECK(r.refused > 0u);
    CHECK(r.refused < 40u);  // it stopped asking instead of trying every unit on them
}

TEST(mc, a_unit_whose_answer_never_comes_is_cancelled_at_its_deadline_and_run_again) {
    // Found on the bench (M0-LOG session 35): B was reset mid-run, rebooted fast enough not to stay
    // dead, and a unit sent while it re-keyed was never answered. The node sets work units no deadline,
    // so the job stuck at 958 of 960. Here node 1 accepts units and never finishes one.
    MCell c(60);
    c.run(3000);
    c.n[1].exec.black_hole = true;
    c.run_job(true);
    const McJobActor::Result r = c.job->last();
    CHECK(r.overdue > 0u);
    CHECK_EQ(c.job->lent_to(0x101), 0u);
    CHECK_EQ(r.hits, expected_hits(60));  // every unit counted exactly once
    CHECK_EQ(static_cast<unsigned>(r.local_units + r.lent_units), 60u);
}

