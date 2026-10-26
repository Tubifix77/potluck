// M23: a board asks the PC to speak (pot/speak.hpp). Two real pot::Nodes on a perfect channel with a 2 ms latency:
// node 0 runs the speak actor, node 1 plays the PC (its call handler is svc/speech), and can answer done, answer
// "no", refuse the path, stay silent, or die.

#include <cstring>
#include <string>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/speak.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kSvc = path_hash("potluck://lab/svc/speech");
constexpr uint32_t kCtl = path_hash("potluck://lab/act/speaker/say");
constexpr uint32_t kOut = path_hash("potluck://lab/act/speaker/result");
const char kPhrase[] = "Hello from board C";

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
    SpeakActor* actor = nullptr;
    uint32_t now_us = 0;
    enum class Mode { Done, No, Refuse, Silent } mode = Mode::Done;
    std::vector<std::string> heard;  // the phrases the "PC" was asked to say
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, const uint8_t* args, uint16_t len) {
        KCell* c = static_cast<KCell*>(ctx);
        if (path != kSvc || c->mode == Mode::Refuse) return false;  // refused: the node answers with an error
        c->heard.emplace_back(reinterpret_cast<const char*>(args), len);
        if (c->mode == Mode::Silent) return true;
        c->n[1].node->reply_call(from, msg_id, path, Value::of_u32(c->mode == Mode::Done ? 1 : 0));
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
        SpeakConfig sc{};
        sc.svc_hash = kSvc;
        sc.provider = 0x101;
        sc.ctl_hash = kCtl;
        sc.out_hash = kOut;
        sc.text_len = static_cast<uint8_t>(sizeof(kPhrase) - 1);
        std::memcpy(sc.text, kPhrase, sc.text_len);
        actor = new SpeakActor(*n[0].node, sc);
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
    void say() { n[0].node->ns().publish(kCtl, Value::of_u32(1), now_us / 1000); }  // a write of act/<name>/say
    uint32_t result() {
        Reading r;
        n[0].node->read(kOut, r);
        uint32_t v = 99;
        r.value.as_u32(v);
        return v;
    }
};

}  // namespace

TEST(speak, a_write_makes_the_pc_say_the_phrase_and_the_result_is_done) {
    KCell c;
    c.run(3000);  // the two find each other
    CHECK_EQ(c.result(), kSpeakNotYet);
    c.say();
    c.run(100);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(1));
    CHECK(c.heard[0] == kPhrase);  // the package's phrase, as the CALL's arguments
    CHECK_EQ(c.result(), kSpeakDone);
    c.say();  // the same value again is a new request
    c.run(100);
    CHECK_EQ(c.heard.size(), static_cast<size_t>(2));
    CHECK_EQ(c.actor->stats().done, 2u);
}

TEST(speak, the_result_is_republished_so_copies_on_other_boards_are_read_again) {
    // Found 2026-10-25: declared with no staleness bound and published only on a change, a copy of the result on
    // another board was never read again and kept its first value.
    KCell c;
    c.run(3000);
    const NsEntry* e = c.n[0].node->ns().find(kOut);
    CHECK(e != nullptr);
    CHECK_EQ(e->staleness_bound_ms, 10000u);
    const uint32_t before = e->update_count;
    c.run(5000);  // nothing asked: still published every 2 s
    CHECK(e->update_count >= before + 2);
    CHECK_EQ(c.result(), kSpeakNotYet);
}

TEST(speak, a_service_that_says_no_or_does_not_serve_the_path_is_refused) {
    KCell c;
    c.run(3000);
    c.mode = KCell::Mode::No;
    c.say();
    c.run(100);
    CHECK_EQ(c.result(), kSpeakRefused);
    c.mode = KCell::Mode::Refuse;
    c.say();
    c.run(100);
    CHECK_EQ(c.result(), kSpeakRefused);
    CHECK_EQ(c.actor->stats().refused, 2u);
}

TEST(speak, the_pc_off_is_gone_at_once_and_a_silent_pc_is_gone_at_the_deadline) {
    KCell c;
    c.run(3000);
    c.n[1].down = true;  // the PC switched off
    c.run(2000);         // declared dead
    c.say();
    c.run(10);
    CHECK_EQ(c.result(), kSpeakGone);  // said at once: nothing is sent to a provider membership does not count alive
    CHECK_EQ(c.actor->stats().calls, 0u);

    KCell s;
    s.run(3000);
    s.mode = KCell::Mode::Silent;
    s.say();
    s.run(1000);
    s.say();  // a request while one is out: counted, not queued
    s.run(kSpeakDeadlineMs);
    CHECK_EQ(s.result(), kSpeakGone);
    CHECK_EQ(s.actor->stats().busy, 1u);
    CHECK_EQ(s.heard.size(), static_cast<size_t>(1));
    s.mode = KCell::Mode::Done;  // the PC answers again: the next request is done
    s.say();
    s.run(100);
    CHECK_EQ(s.result(), kSpeakDone);
}

TEST(speak, a_config_is_judged_by_its_length_and_its_phrase) {
    uint8_t cfg[kSpeakCfgFixed + 4] = {};
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(cfg + at, &v, 4); };
    put32(0, kSvc);
    cfg[4] = 0x01;
    cfg[5] = 0x01;
    put32(6, kCtl);
    put32(10, kOut);
    cfg[14] = 4;
    std::memcpy(cfg + kSpeakCfgFixed, "hey!", 4);
    ActorDecl d{};
    d.node_id = 0x100;
    d.type = ActorType::Speak;
    d.cfg = cfg;
    d.cfg_len = sizeof(cfg);
    SpeakConfig sc{};
    CHECK(speak_config(d, sc));
    CHECK_EQ(sc.provider, static_cast<uint16_t>(0x101));
    CHECK(std::memcmp(sc.text, "hey!", 4) == 0);
    d.cfg_len = sizeof(cfg) - 1;  // the phrase cut short
    CHECK(!speak_config(d, sc));
    cfg[14] = 0;  // no phrase
    d.cfg_len = kSpeakCfgFixed;
    CHECK(!speak_config(d, sc));
}
