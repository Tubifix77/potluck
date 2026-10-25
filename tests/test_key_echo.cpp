// M24: events from the PC (pot/input.hpp) and the key-echo test app (pot/key_echo.hpp). Two real pot::Nodes on a
// perfect channel with a 2 ms latency: node 0 runs the key-echo actor, node 1 plays the PC and CALLs its input with
// batches as the hub builds them.

#include <cstring>
#include <vector>

#include "pot/deploy.hpp"
#include "pot/input.hpp"
#include "pot/key_echo.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kIn = path_hash("potluck://lab/act/echo/in");
constexpr uint32_t kKey = path_hash("potluck://lab/act/echo/key");
constexpr uint32_t kCount = path_hash("potluck://lab/act/echo/count");
constexpr uint32_t kGaps = path_hash("potluck://lab/act/echo/gaps");

struct ENode {
    struct ECell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
};

struct ECell {
    ENode n[2];
    KeyEchoActor* actor = nullptr;
    uint32_t now_us = 0;
    struct Result {
        Node::CallOutcome o;
        uint32_t v;
    };
    std::vector<Result> results;  // what the "PC" heard back
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t msg_id, uint32_t path, const uint8_t* args, uint16_t len) {
        return static_cast<ECell*>(ctx)->actor->on_call(from, msg_id, path, args, len) == Actor::kCallAccepted;
    }
    static void on_result(void* ctx, uint16_t, uint16_t, uint32_t, Node::CallOutcome o, const Value& v) {
        uint32_t x = 0;
        v.as_u32(x);
        static_cast<ECell*>(ctx)->results.push_back(Result{o, x});
    }
    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        ENode* from = static_cast<ENode*>(ctx);
        ECell* c = from->cell;
        const size_t to = 1 - from->index;
        if (std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0 || std::memcmp(c->n[to].mac, mac, kMacLen) == 0) {
            c->air.push_back(Flight{c->now_us + 2000, from->index, to, std::vector<uint8_t>(data, data + len)});
        }
        return 0;
    }

    ECell() {
        for (size_t i = 0; i < 2; ++i) {
            ENode& t = n[i];
            t.cell = this;
            t.index = i;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(0x50 + i);
            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x100 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, t.mac, kMacLen);
            cfg.hello_interval_ms = 500;
            t.hal.ctx = &t;
            t.hal.send = &ECell::send;
            t.hal.now_ms = [](void* c) { return static_cast<ENode*>(c)->cell->now_us / 1000; };
            t.hal.now_us = [](void* c) { return static_cast<ENode*>(c)->cell->now_us; };
            t.node = new Node(cfg, t.hal);
            t.node->start();
        }
        n[0].node->set_call_handler(&ECell::on_call, this);
        n[1].node->set_call_result(&ECell::on_result, this);
        actor = new KeyEchoActor(*n[0].node, KeyEchoConfig{kIn, kKey, kCount, kGaps});
        CHECK(actor->start(0));
        run(3000);  // the two meet
    }
    ~ECell() {
        delete actor;
        for (ENode& t : n) delete t.node;
    }
    void run(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (size_t i = 0; i < air.size();) {
                if (air[i].due_us <= now_us) {
                    Flight f = std::move(air[i]);
                    air.erase(air.begin() + static_cast<std::ptrdiff_t>(i));
                    n[f.to].node->on_rx(n[f.from].mac, f.data.data(), f.data.size(), now_us, -50);
                } else {
                    ++i;
                }
            }
            for (ENode& t : n) t.node->tick(now_us / 1000);
            actor->tick(now_us / 1000);
        }
    }
    // A batch as the hub builds it (potluck.events.encode_batch): keys with consecutive numbers from `first`.
    static std::vector<uint8_t> keys(uint16_t first, const std::vector<uint16_t>& codes, bool down = true,
                                     uint16_t stream = 0x5151) {
        std::vector<uint8_t> b = {kInputVersion, static_cast<uint8_t>(codes.size()), static_cast<uint8_t>(stream),
                                  static_cast<uint8_t>(stream >> 8)};
        for (size_t i = 0; i < codes.size(); ++i) {
            const uint16_t seq = static_cast<uint16_t>(first + i);
            b.insert(b.end(), {static_cast<uint8_t>(seq), static_cast<uint8_t>(seq >> 8), kInputKey,
                               static_cast<uint8_t>(down ? 1 : 0), static_cast<uint8_t>(codes[i]),
                               static_cast<uint8_t>(codes[i] >> 8), 0, 0});
        }
        return b;
    }
    void send_batch(const std::vector<uint8_t>& b) {
        CHECK(n[1].node->request_call(0x100, kIn, b.data(), static_cast<uint16_t>(b.size())) != 0);
        run(50);
    }
    uint32_t read(uint32_t h) {
        Reading r;
        n[0].node->read(h, r);
        uint32_t v = 99;
        r.value.as_u32(v);
        return v;
    }
};

}  // namespace

TEST(key_echo, a_batch_is_taken_in_order_published_and_answered_with_its_last_number) {
    ECell c;
    c.send_batch(ECell::keys(1, {0x04, 0x05, 0x06}));  // a, b, c (HID usage page 7)
    CHECK_EQ(c.results.size(), size_t{1});
    CHECK(c.results[0].o == Node::CallOutcome::Ok);
    CHECK_EQ(c.results[0].v, 3u);
    CHECK_EQ(c.read(kCount), 3u);
    CHECK_EQ(c.read(kKey), 0x06u | (1u << 16));  // the last key, down
    CHECK_EQ(c.read(kGaps), 0u);
}

TEST(key_echo, a_resent_batch_is_dropped_and_a_skipped_number_is_counted) {
    ECell c;
    c.send_batch(ECell::keys(1, {0x04, 0x05}));
    c.send_batch(ECell::keys(1, {0x04, 0x05}));  // the hub did not hear the answer and sent it again
    CHECK_EQ(c.read(kCount), 2u);
    CHECK_EQ(c.actor->order().dropped, 2u);
    CHECK_EQ(c.results.back().v, 2u);
    c.send_batch(ECell::keys(2, {0x05, 0x06}));  // half old, half new: only the new one taken
    CHECK_EQ(c.read(kCount), 3u);
    c.send_batch(ECell::keys(6, {0x07}));  // 4 and 5 never came
    CHECK_EQ(c.read(kGaps), 2u);
    CHECK_EQ(c.results.back().v, 6u);
}

TEST(key_echo, a_restarted_hubs_numbers_are_taken_as_new_not_dropped) {
    // 2026-10-25 on the bench: after a hub restart all 1,200 keys of a run were dropped as already taken -- and answered
    // as taken. A new stream starts the numbers over.
    ECell c;
    c.send_batch(ECell::keys(1, {0x04, 0x05, 0x06}, true, 0x1111));
    c.send_batch(ECell::keys(1, {0x07, 0x08}, true, 0x2222));  // the hub restarted: 1 again, another stream
    CHECK_EQ(c.read(kCount), 5u);
    CHECK_EQ(c.actor->order().dropped, 0u);
    CHECK_EQ(c.read(kGaps), 0u);
}

TEST(key_echo, a_batch_that_is_not_this_version_is_refused) {
    ECell c;
    std::vector<uint8_t> b = ECell::keys(1, {0x04});
    b[0] = 1;
    c.send_batch(b);
    CHECK_EQ(c.results.size(), size_t{1});
    CHECK(c.results[0].o == Node::CallOutcome::Refused);
    CHECK_EQ(c.read(kCount), 0u);
}

TEST(input, parse_bounds_and_numbers_wrap) {
    InputEvent ev[kInputBatchMax];
    std::vector<uint8_t> b = ECell::keys(7, {0x04, 0x05});
    uint16_t stream = 0;
    CHECK_EQ(parse_input(b.data(), static_cast<uint16_t>(b.size()), ev, kInputBatchMax, &stream), 2);
    CHECK_EQ(stream, uint16_t{0x5151});
    CHECK_EQ(ev[1].seq, uint16_t{8});
    CHECK_EQ(ev[1].a, int16_t{0x05});
    CHECK_EQ(parse_input(b.data(), static_cast<uint16_t>(b.size() - 1), ev, kInputBatchMax, nullptr), -1);  // cut short
    b[1] = kInputBatchMax + 1;
    CHECK_EQ(parse_input(b.data(), static_cast<uint16_t>(b.size()), ev, kInputBatchMax, nullptr), -1);
    CHECK(2 + 4 + kInputBatchMax * kInputEventBytes <= kMaxCallArgsV1);  // passed on: the member's two bytes too
    InputOrder o;
    CHECK(o.take(9, 0xFFFE));
    CHECK(o.take(9, 0xFFFF));
    CHECK(o.take(9, 0x0000));  // past 65535 the numbers wrap
    CHECK(!o.take(9, 0xFFFF));
    CHECK_EQ(o.missed, 0u);
    CHECK_EQ(o.taken, 3u);
}
