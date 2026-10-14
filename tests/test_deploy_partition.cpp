// A package deployed while the cell is split, and the cell's leases in a split (the owner's question, 2026-10-14:
// "how does a severed potluck network behave" -- tests/test_reconcile.cpp's [partition8] answered it for portable
// actors; these answer it for deploys and leases).
//
// Real pot::Node objects and the boards' own PackageAgent (pot/package_agent.hpp: the deploy server, the push to the
// peers and catch-up -- moved out of m0_main.cpp for these tests) on a simulated channel that can be cut into groups,
// with a PC as a Node of its own on one board's "cable". What stays a stand-in is the firmware's glue: taking a package
// into the running actors (here: noting what runs), the trial's 100 heartbeats (counted from the node's own beacons,
// as the firmware does), and a reboot (the node rebuilt with its store kept, as flash is).
//
// What the tests record, every simulated millisecond: no board's anti-downgrade floor goes down, no board's running
// counter goes down, and -- once the cell has settled -- every board runs the same package (counter and digest),
// confirmed. Found by them and fixed (M0-LOG session 43): a receive cut off mid-transfer by the split kept the board
// from ever asking for the package; two packages signed at one counter (one per side) left the cell mixed for good.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/lease.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/package_agent.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// A package: the LED on every node, and two opaque actors of an external type (0x80) whose 255-byte configs make the
// image span two 512-byte chunks, so a transfer can be cut off in the middle. `tag` is the digest's first byte: two
// packages at one counter differ in it.
std::vector<uint8_t> make_pkg(uint32_t counter, uint8_t tag) {
    std::vector<uint8_t> body;
    put16(body, kEveryNode);
    body.push_back(static_cast<uint8_t>(ActorType::Led));
    body.push_back(kLedCfgLen);
    const uint8_t led[kLedCfgLen] = {0, tag, 0, 0, 0xE8, 0x03};  // steady, 1000 ms
    body.insert(body.end(), led, led + kLedCfgLen);
    for (int k = 0; k < 2; ++k) {
        put16(body, kEveryNode);
        body.push_back(0x80);
        body.push_back(255);
        for (int i = 0; i < 255; ++i) body.push_back(static_cast<uint8_t>(tag + i + k));
    }
    std::vector<uint8_t> img;
    put32(img, kImageMagic);
    img.push_back(kImageVersion);
    img.push_back(3);
    put16(img, 0);
    put32(img, counter);
    img.push_back(tag);
    for (int i = 1; i < 8; ++i) img.push_back(static_cast<uint8_t>(0x10 * i));
    put32(img, static_cast<uint32_t>(body.size()));
    img.insert(img.end(), body.begin(), body.end());
    return img;
}

struct RamStore : SlotStore {
    DeployState state{};
    std::map<uint8_t, std::vector<uint8_t>> slots;
    bool load_state(DeployState& out) override {
        out = state;
        return true;
    }
    bool save_state(const DeployState& s) override {
        state = s;
        return true;
    }
    bool write_slot(uint8_t slot, const uint8_t* data, size_t len) override {
        slots[slot] = std::vector<uint8_t>(data, data + len);
        return true;
    }
    bool read_slot(uint8_t slot, uint8_t* buf, size_t cap, size_t& len) override {
        auto it = slots.find(slot);
        if (it == slots.end() || it->second.size() > cap) return false;
        std::memcpy(buf, it->second.data(), it->second.size());
        len = it->second.size();
        return true;
    }
};

struct PCell;

struct PNode {
    PCell* cell = nullptr;
    size_t index = 0;
    uint16_t id = 0;
    uint32_t epoch = 1;
    bool pc = false;
    int cable = -1;  // a PC: the board it is cabled to; a board: the PC on its cable, -1 none
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
    // A board's flash: kept across a reboot.
    RamStore store;
    // A board's RAM: rebuilt at a reboot.
    DeployState state{};
    std::vector<uint8_t> buf = std::vector<uint8_t>(2048);
    DeployReceiver* rx = nullptr;
    PackageAgent* agent = nullptr;
    bool trial = false;
    uint32_t trial_base = 0;
    uint32_t refuse_counter = 0;  // this board cannot run that package (the firmware's apply_live refusing it)
    uint32_t reboots = 0;
    // What the checks watch.
    uint32_t max_floor = 0, max_running = 0;
    // A PC: its deploy steps' answers.
    std::vector<std::pair<uint8_t, DeployReply>> results;
    int timeouts = 0;
    LeaseTable leases;  // a board's lease table (the firmware keeps one on every board, used when it is the arbiter)
};

struct PCell {
    std::vector<PNode> n;
    std::vector<std::pair<size_t, size_t>> deaf;
    uint32_t now_us = 0;
    static constexpr uint32_t kLatencyMs = 3;
    static constexpr uint32_t kTrialHeartbeats = 100;  // CONFIG_POT_TRIAL_HEARTBEATS' default
    static constexpr uint8_t kMaxTrialBoots = 3;       // CONFIG_POT_TRIAL_MAX_BOOTS' default
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        bool bcast;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;
    std::vector<std::string> violations;
    size_t boards = 0;

    uint32_t now_ms() const { return now_us / 1000; }

    // A board's id is 0x100 + its index; a PC's is 0x00fe, 0x00fd, ... (below kLeaseMinBoardId, as hosts are).
    void build(size_t n_boards, std::vector<int> pcs_on = {0}) {
        boards = n_boards;
        n.resize(n_boards + pcs_on.size());
        for (size_t i = 0; i < n.size(); ++i) {
            PNode& t = n[i];
            t.cell = this;
            t.index = i;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(i);
            if (i < n_boards) {
                t.id = static_cast<uint16_t>(0x100 + i);
            } else {
                t.pc = true;
                t.mac[1] = 0xFE;
                t.id = static_cast<uint16_t>(0x00FE - (i - n_boards));
                t.cable = pcs_on[i - n_boards];
                n[static_cast<size_t>(t.cable)].cable = static_cast<int>(i);
            }
        }
        for (PNode& t : n) boot(t);
    }

    ~PCell() {
        for (PNode& t : n) {
            delete t.agent;
            delete t.rx;
            delete t.node;
        }
    }

    // ---- a board's firmware, the parts that are not the agent's ----
    static bool committed(void* ctx, DeployStatus* refused) {
        PNode& t = *static_cast<PNode*>(ctx);
        const size_t extent = image_extent(t.rx->image(), t.rx->image_len());
        DeployImage img{};
        if (extent == 0 || !parse_image(t.rx->image(), extent, img, nullptr) || img.rollback_counter == t.refuse_counter) {
            revert_now(t.state);  // nothing changed; the slot just written must not boot either
            t.store.save_state(t.state);
            *refused = DeployStatus::BadImage;
            return false;
        }
        t.state.trial_boots = 1;  // this run is the trial's first boot
        t.store.save_state(t.state);
        t.trial = true;
        t.trial_base = t.node->tx_tally().beacons;
        t.agent->set_running(img.rollback_counter, img.package_digest);
        return true;  // live
    }

    void boot(PNode& t) {
        delete t.agent;
        delete t.rx;
        delete t.node;
        t.agent = nullptr;
        t.rx = nullptr;
        NodeConfig cfg;
        cfg.node_id = t.id;
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        // A PC waits on its deploy steps as `potluck apply` does (send_package: 30 s for the COMMIT, which the cabled
        // board answers only once it has passed the package on), not a node's 2 s.
        if (t.pc) cfg.ns_request_timeout_ms = 30000;
        t.hal.ctx = &t;
        t.hal.send = &PCell::send;
        t.hal.now_ms = [](void* c) { return static_cast<PNode*>(c)->cell->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<PNode*>(c)->cell->now_us; };
        t.hal.on_event = [](void* c, const Event& e) {
            PNode& s = *static_cast<PNode*>(c);
            // The firmware's hal_on_event: a peer that may have missed a package.
            if (s.agent != nullptr &&
                (e.kind == EventKind::PeerRevived || e.kind == EventKind::PeerRebooted ||
                 e.kind == EventKind::PeerDiscovered)) {
                s.agent->soon(s.cell->now_ms());
            }
        };
        t.node = new Node(cfg, t.hal);
        if (t.pc) {
            t.node->set_deploy_result(
                [](void* c, uint16_t, uint16_t, uint8_t op, bool timed_out, const uint8_t* p, uint16_t len) {
                    PNode& s = *static_cast<PNode*>(c);
                    if (timed_out) {
                        ++s.timeouts;
                        return;
                    }
                    DeployReply r{};
                    if (load_deploy_reply(p, len, r)) s.results.push_back({op, r});
                },
                &t);
            t.node->start();
            return;
        }
        // deploy_rt::boot: the trial counted before anything runs, then the active slot loaded.
        t.store.load_state(t.state);
        decide_boot(t.state, kMaxTrialBoots);
        t.store.save_state(t.state);
        PackageAgentHooks hooks;
        hooks.ctx = &t;
        hooks.committed = &PCell::committed;
        t.rx = new DeployReceiver(t.store, t.buf.data(), t.buf.size());
        const uint8_t* host = t.cable >= 0 ? n[static_cast<size_t>(t.cable)].mac : nullptr;
        t.agent = new PackageAgent(*t.node, t.store, *t.rx, t.buf.data(), t.buf.size(), t.state, hooks, host);
        if (t.state.active != kSlotNone) {
            size_t len = 0;
            DeployImage img{};
            if (t.store.read_slot(t.state.active, t.buf.data(), t.buf.size(), len) &&
                parse_image(t.buf.data(), image_extent(t.buf.data(), len), img, nullptr)) {
                t.agent->set_running(img.rollback_counter, img.package_digest);
            }
        }
        t.trial = t.state.pending != 0;
        t.trial_base = 0;
        t.node->set_deploy_server(
            [](void* c, uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len, uint8_t* reply,
               size_t cap) -> size_t {
                PNode& s = *static_cast<PNode*>(c);
                return s.agent->serve(from, msg_id, op, p, len, reply, cap, s.cell->now_ms());
            },
            &t);
        t.node->set_deploy_result(
            [](void* c, uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p, uint16_t len) {
                PNode& s = *static_cast<PNode*>(c);
                s.agent->on_result(peer, msg, op, timed_out, p, len, s.cell->now_ms());
            },
            &t);
        t.node->set_call_handler(
            [](void* c, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
                PNode& s = *static_cast<PNode*>(c);
                return s.agent->on_cast(from, path, args, len, s.cell->now_ms());
            },
            &t);
        t.node->start();
    }

    void reboot(PNode& t) {
        ++t.epoch;
        ++t.reboots;
        boot(t);
    }

    // ---- the channel ----
    bool hears(size_t a, size_t b) const {
        const PNode& x = n[a];
        const PNode& y = n[b];
        if (x.pc || y.pc) return (x.pc && x.cable == static_cast<int>(b)) || (y.pc && y.cable == static_cast<int>(a));
        for (const auto& d : deaf) {
            if ((d.first == a && d.second == b) || (d.first == b && d.second == a)) return false;
        }
        return true;
    }
    void split(const std::vector<int>& group_of) {
        deaf.clear();
        for (size_t i = 0; i < boards; ++i) {
            for (size_t j = i + 1; j < boards; ++j) {
                if (group_of[i] != group_of[j]) deaf.emplace_back(i, j);
            }
        }
    }
    void heal() { deaf.clear(); }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        PNode* from = static_cast<PNode*>(ctx);
        PCell* c = from->cell;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->n.size(); ++i) {
            if (i == from->index) continue;
            if (bcast || std::memcmp(c->n[i].mac, mac, kMacLen) == 0) {
                c->air.push_back(Flight{c->now_us + kLatencyMs * 1000, from->index, i, bcast,
                                        std::vector<uint8_t>(data, data + len)});
            }
        }
        return 0;
    }
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
            PNode& from = n[f.from];
            PNode& to = n[f.to];
            const bool heard = hears(f.from, f.to);
            if (heard) to.node->on_rx(from.mac, f.data.data(), f.data.size(), now_us, -50);
            if (!f.bcast) from.node->on_tx_done(to.mac, heard, now_us);
        }
    }

    // ---- time ----
    void check(PNode& t) {
        char why[160];
        if (t.state.counter < t.max_floor) {
            std::snprintf(why, sizeof(why), "0x%04x's floor went down %u -> %u at %u ms", t.id,
                          static_cast<unsigned>(t.max_floor), static_cast<unsigned>(t.state.counter),
                          static_cast<unsigned>(now_ms()));
            violations.push_back(why);
        }
        if (t.agent->running_counter() < t.max_running) {
            std::snprintf(why, sizeof(why), "0x%04x rolled back %u -> %u at %u ms", t.id,
                          static_cast<unsigned>(t.max_running), static_cast<unsigned>(t.agent->running_counter()),
                          static_cast<unsigned>(now_ms()));
            violations.push_back(why);
        }
        if (t.state.counter > t.max_floor) t.max_floor = t.state.counter;
        if (t.agent->running_counter() > t.max_running) t.max_running = t.agent->running_counter();
    }

    template <typename F>
    void run(uint32_t ms, F each) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            deliver();
            const uint32_t now = now_ms();
            for (PNode& t : n) {
                t.node->tick(now);
                if (t.pc) continue;
                t.agent->tick(now);
                if (t.trial && t.node->tx_tally().beacons - t.trial_base >= kTrialHeartbeats) {
                    confirm(t.state);
                    t.store.save_state(t.state);
                    t.trial = false;
                }
                if (t.agent->reboot_due(now)) reboot(t);
                check(t);
            }
            if (each()) return;
        }
    }
    void run(uint32_t ms) {
        run(ms, [] { return false; });
    }

    // ---- the PC's half: potluck apply's send_package, BEGIN, CHUNKs, COMMIT through the cabled board ----
    bool step(PNode& pc, uint8_t op, const uint8_t* p, size_t len, DeployReply& out, uint32_t max_ms) {
        const size_t before = pc.results.size();
        const int to_before = pc.timeouts;
        const uint16_t target = n[static_cast<size_t>(pc.cable)].id;
        if (pc.node->send_deploy(target, op, p, static_cast<uint16_t>(len)) == 0) return false;
        bool got = false;
        run(max_ms, [&] {
            got = pc.results.size() > before;
            return got || pc.timeouts > to_before;
        });
        if (got) out = pc.results.back().second;
        return got;
    }
    // A deploy whose COMMIT goes on to the cabled board's live peers. Returns the final reply (status Ok on success).
    DeployReply deploy(size_t pc_index, const std::vector<uint8_t>& img, uint32_t counter) {
        PNode& pc = n[pc_index];
        DeployReply r{};
        r.status = DeployStatus::NotStarted;
        DeployBegin b{};
        b.image_len = static_cast<uint32_t>(img.size());
        b.image_crc = crc32(img.data(), img.size());
        b.rollback_counter = counter;
        std::memcpy(b.package_digest, img.data() + 12, 8);
        b.flags = kDeployFlagDistribute;
        uint8_t wire[kDeployChunkHeaderLen + kDeployChunkMax];
        size_t w = store_deploy_begin(b, wire, sizeof(wire));
        if (!step(pc, kOpDeployBegin, wire, w, r, 3000) || r.status != DeployStatus::Ok) return r;
        for (size_t at = 0; at < img.size(); at += 200) {
            const size_t len = std::min<size_t>(200, img.size() - at);
            w = store_deploy_chunk(static_cast<uint32_t>(at), img.data() + at, static_cast<uint16_t>(len), wire,
                                   sizeof(wire));
            if (!step(pc, kOpDeployChunk, wire, w, r, 3000) || r.status != DeployStatus::Ok) return r;
        }
        w = store_deploy_commit(b.image_crc, wire, sizeof(wire));
        if (!step(pc, kOpDeployCommit, wire, w, r, 60000)) r.status = DeployStatus::NotStarted;
        return r;
    }

    // ---- what the cell runs ----
    bool settled_on(uint32_t counter, uint8_t tag) const {
        for (size_t i = 0; i < boards; ++i) {
            const PNode& t = n[i];
            if (t.agent->running_counter() != counter || t.agent->running_digest()[0] != tag || t.state.pending != 0 ||
                t.state.counter != counter) {
                return false;
            }
        }
        return true;
    }
    size_t on(uint32_t counter, uint8_t tag) const {
        size_t k = 0;
        for (size_t i = 0; i < boards; ++i) {
            if (n[i].agent->running_counter() == counter && n[i].agent->running_digest()[0] == tag) ++k;
        }
        return k;
    }
    // Run until the whole cell runs (counter, tag) confirmed; the time it took, or UINT32_MAX.
    uint32_t until_settled(uint32_t counter, uint8_t tag, uint32_t max_ms) {
        const uint32_t t0 = now_ms();
        bool ok = false;
        run(max_ms, [&] { return ok = settled_on(counter, tag); });
        return ok ? now_ms() - t0 : UINT32_MAX;
    }
    PackageAgent::Stats total() const {
        PackageAgent::Stats s;
        for (size_t i = 0; i < boards; ++i) {
            const PackageAgent::Stats& x = n[i].agent->stats();
            s.announces += x.announces;
            s.asked += x.asked;
            s.asked_tie += x.asked_tie;
            s.served += x.served;
            s.refused_lower += x.refused_lower;
            s.rx_abandoned += x.rx_abandoned;
        }
        return s;
    }
    void report_violations() {
        for (const std::string& v : violations) std::printf("    %s\n", v.c_str());
        CHECK(violations.empty());
    }
};

constexpr uint8_t kP = 0x30;  // a package's digest, first byte
constexpr uint8_t kQ = 0x60;  // a second package at the same counter: sorts above kP

// Eight boards and a PC on board 0, everyone on package 1 (deployed and confirmed before anything is cut).
void start_on_package_1(PCell& c, std::vector<int> pcs_on = {0}) {
    c.build(8, pcs_on);
    c.run(3000);
    const DeployReply r = c.deploy(8, make_pkg(1, 0x11), 1);
    CHECK(r.status == DeployStatus::Ok);
    CHECK_EQ(static_cast<int>(r.peers_ok), 7);
    CHECK(c.until_settled(1, 0x11, 30000) != UINT32_MAX);
}

}  // namespace

// ---- a deploy into one side ---------------------------------------------------------------------------------------

TEST(deploy_partition, a_package_sent_into_one_side_waits_there_and_reaches_the_other_after_the_heal) {
    PCell c;
    start_on_package_1(c);
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(3000);  // long past every death window: each side has declared the other dead
    const DeployReply r = c.deploy(8, make_pkg(2, kP), 2);
    CHECK(r.status == DeployStatus::Ok);
    // The cabled board passes it on to the peers it holds alive: its own side. The PC hears "3 passed on, none
    // failed" -- the other side is not in the list at all, so `apply`'s verify (not the reply) is what finds it.
    CHECK_EQ(static_cast<int>(r.peers_ok), 3);
    CHECK_EQ(static_cast<int>(r.peers_failed), 0);
    // Five minutes of split: one side confirms 2, the other stays on 1 -- neither moves, nothing rolls back.
    c.run(300000);
    CHECK_EQ(c.on(2, kP), static_cast<size_t>(4));
    CHECK_EQ(c.on(1, 0x11), static_cast<size_t>(4));
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(static_cast<int>(c.n[i].state.pending), 0);
    c.heal();
    const uint32_t t = c.until_settled(2, kP, 600000);
    CHECK(t != UINT32_MAX);
    const PackageAgent::Stats s = c.total();
    std::printf("    4+4 split, deploy to one side: whole cell on it %u ms after the heal; %u asks, %u catch-ups sent\n",
                static_cast<unsigned>(t), static_cast<unsigned>(s.asked), static_cast<unsigned>(s.served));
    CHECK_EQ(s.served, 4u);  // one catch-up per board of the other side, none twice
    // The first announce comes 5 s after the peers revive, and each board's trial takes 100 heartbeats (10 s). Before a
    // busy peer kept the asks it could not serve yet, every board behind asked the same one and three of four waited for
    // the next minute's announce: 195 s here.
    CHECK(t < 30000);
    c.run(180000);  // and it stays there
    CHECK(c.settled_on(2, kP));
    c.report_violations();
}

TEST(deploy_partition, a_push_cut_off_by_the_split_is_abandoned_and_the_board_catches_up_after_the_heal) {
    // Found by this test: the split lands while the cabled board is passing the package to a board on the far side.
    // The pusher times out and goes on; the far board's receiver stayed "in progress" -- and a board in that state
    // neither asks for a package nor serves one, so after the heal it stayed on the old package until the next deploy.
    PCell c;
    start_on_package_1(c);
    const std::vector<uint8_t> img = make_pkg(2, kP);
    // Drive the deploy by hand so the cut can land mid-push: the first far-side board (4..7) that has begun receiving.
    PNode& pc = c.n[8];
    DeployBegin b{};
    b.image_len = static_cast<uint32_t>(img.size());
    b.image_crc = crc32(img.data(), img.size());
    b.rollback_counter = 2;
    std::memcpy(b.package_digest, img.data() + 12, 8);
    b.flags = kDeployFlagDistribute;
    uint8_t wire[kDeployChunkHeaderLen + kDeployChunkMax];
    DeployReply r{};
    CHECK(c.step(pc, kOpDeployBegin, wire, store_deploy_begin(b, wire, sizeof(wire)), r, 3000));
    for (size_t at = 0; at < img.size(); at += 200) {
        const size_t len = std::min<size_t>(200, img.size() - at);
        CHECK(c.step(pc, kOpDeployChunk, wire,
                     store_deploy_chunk(static_cast<uint32_t>(at), img.data() + at, static_cast<uint16_t>(len), wire,
                                        sizeof(wire)),
                     r, 3000));
    }
    const size_t before = pc.results.size();
    CHECK(pc.node->send_deploy(c.n[0].id, kOpDeployCommit, wire,
                               static_cast<uint16_t>(store_deploy_commit(b.image_crc, wire, sizeof(wire)))) != 0);
    int cut_at = -1;
    c.run(20000, [&] {
        for (size_t i = 4; i < 8 && cut_at < 0; ++i) {
            if (c.n[i].rx->in_progress() && c.n[i].rx->image_len() > 0) {
                cut_at = static_cast<int>(i);
                c.split({0, 0, 0, 0, 1, 1, 1, 1});
            }
        }
        return pc.results.size() > before;
    });
    CHECK(cut_at >= 4);
    CHECK(pc.results.size() > before);
    r = pc.results.back().second;
    std::printf("    cut mid-push at 0x%04x: the PC hears %u passed on, %u failed\n", c.n[static_cast<size_t>(cut_at)].id,
                static_cast<unsigned>(r.peers_ok), static_cast<unsigned>(r.peers_failed));
    CHECK(r.peers_failed >= 1);  // the far side's boards still alive in the pusher's table when the cut came
    PNode& far = c.n[static_cast<size_t>(cut_at)];
    c.run(30000);
    CHECK(!far.rx->in_progress());  // abandoned after kRxStaleMs with no step
    CHECK_EQ(far.agent->stats().rx_abandoned, 1u);
    CHECK_EQ(far.agent->running_counter(), 1u);  // and nothing of the half-received package taken
    c.run(120000);
    c.heal();
    const uint32_t t = c.until_settled(2, kP, 600000);
    CHECK(t != UINT32_MAX);
    std::printf("    whole cell on package 2 %u ms after the heal\n", static_cast<unsigned>(t));
    c.report_violations();
}

TEST(deploy_partition, a_board_that_refuses_the_package_is_not_sent_it_again) {
    // A package that cannot run on one board (here: refused when taken; a crash on trial reverts the same way) raises
    // the board's floor and leaves it on the package before. Catch-up must not hand it the same package again -- and
    // the one-counter tie rule must not either: the board runs a counter below its floor, so it asks for nothing there.
    PCell c;
    start_on_package_1(c);
    c.n[5].refuse_counter = 2;
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(3000);
    CHECK(c.deploy(8, make_pkg(2, kP), 2).status == DeployStatus::Ok);
    c.run(30000);
    c.heal();
    c.run(200000);
    CHECK_EQ(c.on(2, kP), static_cast<size_t>(7));
    CHECK_EQ(c.n[5].agent->running_counter(), 1u);
    CHECK_EQ(c.n[5].state.counter, 2u);
    // It asked (it was behind) and refused what came; then never again, though every peer announces 2 each minute.
    const uint32_t asked = c.n[5].agent->stats().asked;
    CHECK(asked >= 1u);
    c.run(300000);
    CHECK_EQ(c.n[5].agent->stats().asked, asked);
    CHECK_EQ(c.n[5].agent->stats().asked_tie, 0u);
    CHECK_EQ(c.n[5].agent->running_counter(), 1u);
    c.report_violations();
}

TEST(deploy_partition, a_side_deployed_twice_wins_whatever_the_other_side_did) {
    // The higher counter wins, by the floor rule that was always there: side A takes 2 and 3, side B takes its own 2.
    PCell c;
    start_on_package_1(c, {0, 4});
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(3000);
    CHECK(c.deploy(8, make_pkg(2, kP), 2).status == DeployStatus::Ok);
    CHECK(c.deploy(9, make_pkg(2, kQ), 2).status == DeployStatus::Ok);
    c.run(20000);
    CHECK(c.deploy(8, make_pkg(3, kP), 3).status == DeployStatus::Ok);
    c.run(30000);
    c.heal();
    CHECK(c.until_settled(3, kP, 600000) != UINT32_MAX);
    c.run(180000);
    CHECK(c.settled_on(3, kP));
    c.report_violations();
}

// ---- one counter, two packages -------------------------------------------------------------------------------

TEST(deploy_partition, two_packages_at_one_counter_one_per_side_converge_on_the_higher_digest) {
    // Two PCs, one on each side of the split, each holding its side's deploy lease ([lease_partition] below), each
    // signs the cell's floor + 1 = 2. Before 2026-10-14 the heal changed nothing: every board's floor was 2 and every
    // announce said 2, so nobody asked, and the cell stayed half P and half Q for good. Now the higher digest wins.
    PCell c;
    start_on_package_1(c, {0, 4});
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(3000);
    CHECK(c.deploy(8, make_pkg(2, kP), 2).status == DeployStatus::Ok);
    CHECK(c.deploy(9, make_pkg(2, kQ), 2).status == DeployStatus::Ok);
    c.run(60000);
    CHECK_EQ(c.on(2, kP), static_cast<size_t>(4));
    CHECK_EQ(c.on(2, kQ), static_cast<size_t>(4));
    c.heal();
    const uint32_t t = c.until_settled(2, kQ, 600000);
    CHECK(t != UINT32_MAX);
    const PackageAgent::Stats s = c.total();
    std::printf("    two packages at counter 2, one per side: the cell on the higher digest %u ms after the heal "
                "(%u asks at the same counter)\n",
                static_cast<unsigned>(t), static_cast<unsigned>(s.asked_tie));
    CHECK(s.asked_tie >= 4u);  // each of P's boards at least once
    CHECK(t < 30000);
    c.run(300000);  // and nothing flips back
    CHECK(c.settled_on(2, kQ));
    c.report_violations();
}

TEST(deploy_partition, three_sides_and_three_packages_at_one_counter_end_on_one) {
    PCell c;
    start_on_package_1(c, {0, 3, 6});
    c.split({0, 0, 0, 1, 1, 1, 2, 2});
    c.run(3000);
    CHECK(c.deploy(8, make_pkg(2, 0x50), 2).status == DeployStatus::Ok);
    CHECK(c.deploy(9, make_pkg(2, 0x70), 2).status == DeployStatus::Ok);
    CHECK(c.deploy(10, make_pkg(2, 0x40), 2).status == DeployStatus::Ok);
    c.run(30000);
    c.split({0, 0, 0, 1, 1, 1, 0, 0});  // two of the sides meet first
    c.run(300000);
    CHECK_EQ(c.on(2, 0x50), static_cast<size_t>(5));  // 0x50 beat 0x40 where they met
    c.heal();
    CHECK(c.until_settled(2, 0x70, 600000) != UINT32_MAX);
    c.report_violations();
}

TEST(deploy_tie, an_announce_asks_only_for_newer_or_for_a_higher_digest_at_this_boards_floor) {
    PCell c;
    start_on_package_1(c);
    PNode& b = c.n[3];
    const uint32_t now = c.now_ms() + 100000;  // past any cooldown
    auto announce = [&](uint32_t counter, uint8_t tag, size_t len, uint32_t at) {
        uint8_t a[kPkgAnnounceLen] = {kPkgAnnounce};
        std::memcpy(a + 1, &counter, 4);
        const std::vector<uint8_t> img = make_pkg(counter, tag);
        std::memcpy(a + 5, img.data() + 12, 8);
        const uint32_t asked = b.agent->stats().asked;
        b.agent->on_cast(0x101, kPkgPath, a, static_cast<uint16_t>(len), at);
        return b.agent->stats().asked > asked;
    };
    CHECK(!announce(1, 0x11, kPkgAnnounceLen, now));           // the same package
    CHECK(!announce(1, 0x05, kPkgAnnounceLen, now));           // the same counter, a lower digest
    CHECK(!announce(1, 0x40, 5, now));                         // a board before the digest: says nothing about it
    CHECK(announce(1, 0x40, kPkgAnnounceLen, now));            // the same counter, a higher digest
    CHECK(!announce(2, 0x40, kPkgAnnounceLen, now + 1000));    // inside the cooldown, even when newer
    CHECK(announce(2, 0x01, 5, now + kPkgAskCooldownMs + 1));  // newer, whatever the digest, old or new format
    CHECK_EQ(b.agent->stats().asked_tie, 1u);
}

TEST(deploy_tie, a_commit_at_the_floor_with_a_lower_digest_is_refused_before_anything_is_written) {
    PCell c;
    start_on_package_1(c);
    PNode& b = c.n[2];
    const std::vector<uint8_t> low = make_pkg(1, 0x05);
    const std::vector<uint8_t> high = make_pkg(1, 0x40);
    const uint32_t now = c.now_ms();
    auto send = [&](const std::vector<uint8_t>& img) {
        DeployBegin bg{};
        bg.image_len = static_cast<uint32_t>(img.size());
        bg.image_crc = crc32(img.data(), img.size());
        bg.rollback_counter = 1;
        uint8_t wire[kDeployChunkHeaderLen + kDeployChunkMax];
        uint8_t reply[kDeployReplyLen];
        DeployReply r{};
        b.agent->serve(0x101, 1, kOpDeployBegin, wire, static_cast<uint16_t>(store_deploy_begin(bg, wire, sizeof(wire))),
                       reply, sizeof(reply), now);
        for (size_t at = 0; at < img.size(); at += 400) {
            const size_t len = std::min<size_t>(400, img.size() - at);
            b.agent->serve(0x101, 2, kOpDeployChunk, wire,
                           static_cast<uint16_t>(store_deploy_chunk(static_cast<uint32_t>(at), img.data() + at,
                                                                     static_cast<uint16_t>(len), wire, sizeof(wire))),
                           reply, sizeof(reply), now);
        }
        const size_t n = b.agent->serve(0x101, 3, kOpDeployCommit, wire,
                                        static_cast<uint16_t>(store_deploy_commit(bg.image_crc, wire, sizeof(wire))),
                                        reply, sizeof(reply), now);
        CHECK(n == kDeployReplyLen && load_deploy_reply(reply, n, r));
        return r.status;
    };
    const auto slots_before = b.store.slots;
    CHECK(send(low) == DeployStatus::Downgrade);
    CHECK(b.store.slots == slots_before);
    CHECK(!b.rx->in_progress());
    CHECK_EQ(b.agent->running_digest()[0], static_cast<uint8_t>(0x11));
    CHECK(send(high) == DeployStatus::Ok);  // a higher one at the floor is taken (on trial, like any package)
    CHECK_EQ(b.agent->running_digest()[0], static_cast<uint8_t>(0x40));
    CHECK_EQ(b.agent->stats().refused_lower, 1u);
}

// ---- the cell's leases in a split -----------------------------------------------------------------------------

namespace {
LeaseRequest take(uint32_t token, const char* name) {
    LeaseRequest r;
    r.kind = kLeaseDeploy;
    r.op = kLeaseTake;
    r.ttl_s = 60;
    r.token = token;
    r.name_len = static_cast<uint8_t>(std::strlen(name));
    std::memcpy(r.name, name, r.name_len);
    return r;
}
// What the firmware's LEASE handler does for a PC's request with target 0: the board on the cable picks the arbiter
// (lease_arbiter_of, the firmware's own function), and the arbiter's table answers.
LeaseReply ask_lease(PCell& c, size_t pc, const LeaseRequest& req) {
    PNode& cabled = c.n[static_cast<size_t>(c.n[pc].cable)];
    const uint16_t arb = lease_arbiter_of(cabled.node->peers(), cabled.id, c.n[pc].mac);
    PNode& a = c.n[arb - 0x100u];
    return a.leases.handle(req, c.now_ms(), a.id);
}
}  // namespace

TEST(lease_partition, each_side_of_a_split_has_an_arbiter_and_both_pcs_get_the_deploy_lease) {
    PCell c;
    c.build(8, {0, 5});
    c.run(3000);
    // Whole: both cabled boards pick 0x0100, and the second PC is refused, naming the first.
    LeaseReply r1 = ask_lease(c, 8, take(0x11, "apply on pc-1"));
    LeaseReply r2 = ask_lease(c, 9, take(0x22, "apply on pc-2"));
    CHECK(r1.status == LeaseStatus::Ok);
    CHECK(r2.status == LeaseStatus::Held);
    CHECK_EQ(r2.arbiter, static_cast<uint16_t>(0x100));
    CHECK(ask_lease(c, 8, LeaseRequest{0, kLeaseDeploy, kLeaseRelease, 0, 0x11, 0, {}}).status == LeaseStatus::Ok);
    // Split 0-3 | 4-7: board 5's side picks 0x0104, and both PCs hold "the" deploy lease at once.
    c.split({0, 0, 0, 0, 1, 1, 1, 1});
    c.run(3000);
    r1 = ask_lease(c, 8, take(0x11, "apply on pc-1"));
    r2 = ask_lease(c, 9, take(0x22, "apply on pc-2"));
    CHECK(r1.status == LeaseStatus::Ok);
    CHECK(r2.status == LeaseStatus::Ok);
    CHECK_EQ(r1.arbiter, static_cast<uint16_t>(0x100));
    CHECK_EQ(r2.arbiter, static_cast<uint16_t>(0x104));
    std::printf("    split 4+4: PC 1's lease held by 0x%04x, PC 2's by 0x%04x -- two holders\n",
                static_cast<unsigned>(r1.arbiter), static_cast<unsigned>(r2.arbiter));
    // Each would sign the floor + 1 it sees -- the same counter: [deploy_partition]'s one-counter case.
    // apply's answer (host/potluck/potluck/apply.py, lease_counts): a deploy lease counts only when its arbiter is the
    // cluster's lowest board, which only one side can reach -- here PC 2's (0x0104) does not count.
    // Healed: PC 2's renewal goes to 0x0100, which holds PC 1's -- refused, naming it (the lease is lost).
    c.heal();
    c.run(3000);
    r2 = ask_lease(c, 9, take(0x22, "apply on pc-2"));
    CHECK(r2.status == LeaseStatus::Held);
    CHECK_EQ(r2.arbiter, static_cast<uint16_t>(0x100));
    CHECK(std::strcmp(r2.name, "apply on pc-1") == 0);
    // Board 0x0104 still holds PC 2's entry in RAM until it expires; nobody asks it, since it is not the lowest.
    CHECK(std::strcmp(c.n[4].leases.holder(kLeaseDeploy, c.now_ms()), "apply on pc-2") == 0);
}
