// M8, section 7.5: an MCU actor calling a host's named service, and degrading correctly when the host
// is off. Two real pot::Nodes on a perfect channel with a 2 ms latency: node 0 runs the client, node 1
// plays the host (its call handler is the "service"), and can be killed, revived, silenced or made to
// refuse.

#include <cstring>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/svc_client.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kSvc = path_hash("potluck://lab/svc/time");
constexpr uint32_t kOut = path_hash("potluck://lab/act/clock/out");

struct SCell;
struct SNode {
    SCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    uint32_t epoch = 1;
    bool down = false;
    Node* node = nullptr;
    NodeHal hal{};
};

struct SCell {
    SNode n[2];
    SvcClient* client = nullptr;
    uint32_t now_us = 0;
    // The "service" on node 1.
    enum class Mode { Answer, Silent, Refuse } mode = Mode::Answer;
    uint64_t answer = 1000;
    std::vector<std::pair<uint16_t, uint16_t>> owed;  // (from, msg_id) the silent host never answers
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, const uint8_t*, uint16_t) {
        SCell* c = static_cast<SCell*>(ctx);
        if (path != kSvc || c->mode == Mode::Refuse) return false;  // refused: the node answers with an error
        if (c->mode == Mode::Silent) {
            c->owed.emplace_back(from, msg_id);
            return true;
        }
        c->n[1].node->reply_call(from, msg_id, path, Value::of_u64(++c->answer));
        return true;
    }
    static void on_result(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, Node::CallOutcome o,
                          const Value& v) {
        static_cast<SCell*>(ctx)->client->on_result(from, msg_id, path, o, v);
    }

    void boot(size_t i) {
        SNode& t = n[i];
        delete t.node;
        NodeConfig cfg;
        cfg.node_id = static_cast<uint16_t>(0x100 + i);
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        t.hal.ctx = &t;
        t.hal.send = &SCell::send;
        t.hal.now_ms = [](void* c) { return static_cast<SNode*>(c)->cell->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<SNode*>(c)->cell->now_us; };
        t.node = new Node(cfg, t.hal);
        if (i == 1) t.node->set_call_handler(&SCell::on_call, this);
        t.node->start();
        t.down = false;
    }

    explicit SCell(uint16_t period_ms = 500) {
        for (size_t i = 0; i < 2; ++i) {
            n[i].cell = this;
            n[i].index = i;
            n[i].mac[0] = 0x02;
            n[i].mac[5] = static_cast<uint8_t>(0x20 + i);
        }
        boot(0);
        boot(1);
        SvcClientConfig sc{kSvc, 0x101, kOut, period_ms, 1};
        client = new SvcClient(*n[0].node, sc);
        n[0].node->set_call_result(&SCell::on_result, this);
        CHECK(client->start(now_us / 1000));
    }
    ~SCell() {
        delete client;
        for (SNode& t : n) delete t.node;
    }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        SNode* from = static_cast<SNode*>(ctx);
        SCell* c = from->cell;
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
                    SNode& to = n[f.to];
                    if (!to.down && !n[f.from].down) to.node->on_rx(n[f.from].mac, f.data.data(), f.data.size(), now_us, -50);
                } else {
                    ++i;
                }
            }
            for (SNode& t : n) {
                if (t.down) continue;
                t.node->tick(now_us / 1000);
            }
            client->tick(now_us / 1000);
        }
    }
    Reading out() {
        Reading r;
        n[0].node->read(kOut, r);
        return r;
    }
};

uint64_t u64_of(const Reading& r) {
    uint64_t v = 0;
    std::memcpy(&v, r.value.raw, sizeof(v));
    return v;
}

}  // namespace

TEST(svc_client, the_service_answers_and_the_answer_is_published) {
    SCell c;
    c.run(3000);
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Serving));
    CHECK(c.client->stats().answered >= 4u);
    const Reading r = c.out();
    CHECK(r.quality == Quality::Good);
    CHECK_EQ(u64_of(r), c.answer);
}

TEST(svc_client, the_host_goes_away_and_the_output_ages_honestly_then_recovers) {
    SCell c;
    c.run(3000);
    const uint64_t last = c.answer;
    c.n[1].down = true;
    c.run(4000);  // past the 600 ms death window and the 1.5 s staleness bound
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Degraded));
    CHECK(c.client->stats().not_sent > 0u);
    const Reading r = c.out();
    CHECK(r.quality == Quality::Stale);  // the host's last answer, never presented as fresh
    CHECK_EQ(u64_of(r), last);
    CHECK(r.age_ms >= 3000u);

    // The host comes back in a new incarnation: the first answer is published at once.
    ++c.n[1].epoch;
    c.boot(1);
    c.run(3000);
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Serving));
    CHECK_EQ(c.client->stats().recoveries, 1u);
    CHECK(c.out().quality == Quality::Good);
    CHECK(u64_of(c.out()) > last);
}

TEST(svc_client, a_call_in_flight_when_the_host_dies_is_written_off_not_waited_on) {
    SCell c(5000);  // a long period, so a call is outstanding when the host goes
    c.run(4900);    // first call at t=0, answered; the next is due at 5 s
    c.mode = SCell::Mode::Silent;  // the host accepts the next call and sits on it ...
    c.run(200);     // t = 5.1 s: the second call is in flight ...
    CHECK_EQ(c.owed.size(), static_cast<size_t>(1));
    c.n[1].down = true;  // ... and the host dies under it
    c.run(1000);
    CHECK_EQ(c.client->stats().lost, 1u);
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Degraded));
}

TEST(svc_client, a_host_alive_but_silent_cannot_hold_the_client) {
    SCell c;
    c.run(2000);
    c.mode = SCell::Mode::Silent;
    c.run(5000);
    CHECK(c.client->stats().timed_out >= 2u);  // each silent call given up after its deadline
    CHECK(c.client->stats().calls >= 5u);      // and the client kept calling
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Degraded));
    CHECK(c.out().quality == Quality::Stale);
    c.mode = SCell::Mode::Answer;
    c.run(3000);
    CHECK_EQ(static_cast<int>(c.client->state()), static_cast<int>(SvcClient::State::Serving));
}

TEST(svc_client, a_host_that_does_not_serve_the_path_is_counted_as_refused) {
    SCell c;
    c.mode = SCell::Mode::Refuse;
    c.run(2000);
    CHECK(c.client->stats().refused >= 3u);
    CHECK_EQ(c.client->stats().answered, 0u);
    CHECK(c.out().quality != Quality::Good);
}
