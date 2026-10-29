// M28: a board asks the PC to play a pattern of notes (pot/tone.hpp). Two real pot::Nodes on a perfect channel with a
// 2 ms latency: node 0 runs the tone actor, node 1 plays the PC (its call handler is svc/sound), and can answer
// playing, answer "no such pattern", refuse the path, stay silent, or die. Node 0 also owns the alarm the actor follows.

#include <cstring>
#include <string>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/tone.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kSvc = path_hash("potluck://lab/svc/sound");
constexpr uint32_t kCtl = path_hash("potluck://lab/act/bell/play");
constexpr uint32_t kOut = path_hash("potluck://lab/act/bell/result");
constexpr uint32_t kAlarm = path_hash("potluck://lab/node-0100/test/alarm");

struct KCell;
struct KNode {
    KCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    uint32_t epoch = 1;
    bool down = false;
    Node* node = nullptr;
    NodeHal hal{};
};

struct KCell {
    KNode n[2];
    ToneActor* actor = nullptr;
    uint32_t now_us = 0;
    enum class Mode { Done, No, Refuse, Silent } mode = Mode::Done;
    std::vector<int32_t> heard;  // the patterns the "PC" was asked to play
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, const uint8_t* args, uint16_t len) {
        KCell* c = static_cast<KCell*>(ctx);
        if (path != kSvc || c->mode == Mode::Refuse) return false;  // refused: the node answers with an error
        int32_t p = 0;
        if (len == 4) std::memcpy(&p, args, 4);
        c->heard.push_back(len == 4 ? p : -1);
        if (c->mode == Mode::Silent) return true;
        c->n[1].node->reply_call(from, msg_id, path, Value::of_u32(c->mode == Mode::Done && p <= 8 ? 1 : 0));
        return true;
    }
    static void on_result(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, Node::CallOutcome o,
                          const Value& v) {
        static_cast<KCell*>(ctx)->actor->on_call_result(from, msg_id, path, o, v);
    }

    void boot(size_t i) {
        KNode& t = n[i];
        delete t.node;
        NodeConfig cfg;
        cfg.node_id = static_cast<uint16_t>(0x100 + i);
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        t.hal.ctx = &t;
        t.hal.send = &KCell::send;
        t.hal.now_ms = [](void* c) { return static_cast<KNode*>(c)->cell->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<KNode*>(c)->cell->now_us; };
        t.node = new Node(cfg, t.hal);
        if (i == 1) t.node->set_call_handler(&KCell::on_call, this);
        t.node->start();
        t.down = false;
    }

    KCell() {
        for (size_t i = 0; i < 2; ++i) {
            n[i].cell = this;
            n[i].index = i;
            n[i].mac[0] = 0x02;
            n[i].mac[5] = static_cast<uint8_t>(0x40 + i);
        }
        boot(0);
        boot(1);
        NsDecl al;
        al.path_hash = kAlarm;
        al.owner_node = 0x100;
        al.type = ValueType::I32;
        al.unit = Unit::None;
        al.kind = ResourceKind::Sampled;
        al.access = Access::Read;
        al.latency_class = kClassL4;
        al.staleness_bound_ms = 10000;
        CHECK_EQ(static_cast<int>(n[0].node->ns().declare(al)), static_cast<int>(NsError::Ok));
        ToneConfig tc{};
        tc.svc_hash = kSvc;
        tc.provider = 0x101;
        tc.play_hash = kCtl;
        tc.out_hash = kOut;
        tc.source_hash = kAlarm;
        tc.pattern = 4;
        actor = new ToneActor(*n[0].node, tc);
        n[0].node->set_call_result(&KCell::on_result, this);
        CHECK(actor->start(now_us / 1000));
    }
    ~KCell() {
        delete actor;
        for (KNode& t : n) delete t.node;
    }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        KNode* from = static_cast<KNode*>(ctx);
        KCell* c = from->cell;
        if (from->down) return 0;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        const size_t to = 1 - from->index;
        if (bcast || std::memcmp(c->n[to].mac, mac, kMacLen) == 0) {
            c->air.push_back(Flight{c->now_us + 2000, from->index, to, std::vector<uint8_t>(data, data + len)});
        }
        return 0;
    }
    void run(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (size_t i = 0; i < air.size();) {
                if (air[i].due_us <= now_us) {
                    Flight f = std::move(air[i]);
                    air.erase(air.begin() + static_cast<std::ptrdiff_t>(i));
                    KNode& to = n[f.to];
                    if (!to.down && !n[f.from].down) to.node->on_rx(n[f.from].mac, f.data.data(), f.data.size(), now_us, -50);
                } else {
                    ++i;
                }
            }
            for (KNode& t : n) {
                if (!t.down) t.node->tick(now_us / 1000);
            }
            actor->tick(now_us / 1000);
        }
    }
    void play(int32_t p) { n[0].node->ns().publish(kCtl, Value::of_i32(p), now_us / 1000); }  // a write of play
    void alarm(int32_t v) { n[0].node->ns().publish(kAlarm, Value::of_i32(v), now_us / 1000); }
    uint32_t result() {
        Reading r;
        n[0].node->read(kOut, r);
        uint32_t v = 99;
        r.value.as_u32(v);
        return v;
    }
};

}  // namespace

TEST(tone, a_write_plays_the_pattern_and_the_result_is_playing) {
    KCell c;
    c.run(3000);
    CHECK_EQ(c.result(), kToneNotYet);
    c.play(3);
    c.run(100);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(1));
    CHECK_EQ(c.heard[0], 3);  // the pattern, as the CALL's four argument bytes
    CHECK_EQ(c.result(), kTonePlaying);
    c.play(0);  // stop
    c.run(100);
    CHECK_EQ(c.heard.back(), 0);
    c.play(9);  // no such pattern: the PC says no
    c.run(100);
    CHECK_EQ(c.result(), kToneRefused);
}

TEST(tone, a_request_while_a_call_is_out_waits_and_only_the_newest_goes) {
    KCell c;
    c.run(3000);
    c.mode = KCell::Mode::Silent;
    c.play(2);
    c.run(10);
    c.play(5);
    c.run(10);  // seen by the actor: it waits
    c.play(0);  // "stop" after "play" replaces it -- the one that must not be lost
    c.run(kToneDeadlineMs + 100);  // the first call written off: the waiting one goes
    CHECK_EQ(c.heard.size(), static_cast<size_t>(2));
    CHECK_EQ(c.heard[1], 0);
    CHECK_EQ(c.actor->stats().replaced, 1u);
    c.mode = KCell::Mode::Done;
    c.play(1);
    c.run(kToneDeadlineMs + 100);
    CHECK_EQ(c.result(), kTonePlaying);
}

TEST(tone, it_follows_an_alarm_quiet_at_start_its_own_pattern_when_set_and_stop_when_cleared) {
    KCell c;
    c.alarm(0);
    c.run(3000);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(0));  // quiet at start: nothing to stop
    c.alarm(1);
    c.run(1200);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(1));
    CHECK_EQ(c.heard[0], 4);  // the actor's pattern, not the alarm's value
    c.run(3000);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(1));  // the same value again is no new request
    c.alarm(0);
    c.run(1200);
    CHECK_EQ(c.heard.back(), 0);
}

TEST(tone, the_pc_off_is_gone_at_once_and_the_result_is_republished) {
    KCell c;
    c.run(3000);
    const NsEntry* e = c.n[0].node->ns().find(kOut);
    CHECK(e != nullptr);
    CHECK_EQ(e->staleness_bound_ms, 10000u);
    const uint32_t before = e->update_count;
    c.run(5000);
    CHECK(e->update_count >= before + 2);  // copies on other boards are read again
    c.n[1].down = true;
    c.run(2000);
    c.play(1);
    c.run(10);
    CHECK_EQ(c.result(), kToneGone);
    CHECK_EQ(c.actor->stats().calls, 0u);
}

TEST(tone, a_config_is_judged_by_its_length_and_its_pattern) {
    uint8_t cfg[kToneCfgLen] = {};
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(cfg + at, &v, 4); };
    put32(0, kSvc);
    cfg[4] = 0x01;
    cfg[5] = 0x01;
    put32(6, kCtl);
    put32(10, kOut);
    put32(14, 0);
    cfg[18] = 8;
    ActorDecl d{};
    d.node_id = 0x100;
    d.type = ActorType::Tone;
    d.cfg = cfg;
    d.cfg_len = sizeof(cfg);
    ToneConfig tc{};
    CHECK(tone_config(d, tc));
    CHECK_EQ(tc.provider, static_cast<uint16_t>(0x101));
    CHECK_EQ(tc.source_hash, 0u);
    cfg[18] = 9;  // no such pattern
    CHECK(!tone_config(d, tc));
    cfg[18] = 0;
    d.cfg_len = sizeof(cfg) - 1;
    CHECK(!tone_config(d, tc));
}
