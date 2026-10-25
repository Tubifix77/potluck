// M25: the bulk channel (pot/bulk.hpp) and its test app (pot/file_hash.hpp). Two real pot::Nodes on a perfect channel
// with a 2 ms latency: node 0 runs the file-hash actor, node 1 plays the PC -- its call handler is svc/file, and it
// pushes the file to the actor's input one chunk at a time, as the hub does (potluck.bulk).

#include <cstring>
#include <vector>

#include "monocypher-ed25519.h"
#include "pot/bulk.hpp"
#include "pot/deploy.hpp"
#include "pot/file_hash.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint32_t kSvc = path_hash("potluck://lab/svc/file");
constexpr uint32_t kIn = path_hash("potluck://lab/act/fetch/in");
constexpr uint32_t kGo = path_hash("potluck://lab/act/fetch/go");
constexpr uint32_t kBytes = path_hash("potluck://lab/act/fetch/bytes");
constexpr uint32_t kState = path_hash("potluck://lab/act/fetch/state");
constexpr uint32_t kDigest = path_hash("potluck://lab/act/fetch/digest");
constexpr uint32_t kMs = path_hash("potluck://lab/act/fetch/ms");

struct FNode {
    struct FCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
};

struct FCell {
    FNode n[2];
    FileHashActor* actor = nullptr;
    uint32_t now_us = 0;
    std::vector<uint8_t> file;   // what the "PC" offers
    bool have_file = true;
    uint32_t next = 0;           // the offset the board last said it expects
    bool pushing = false, in_flight = false;
    bool manual = false;         // the test sends the chunks itself
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t msg, uint32_t path, const uint8_t* args, uint16_t len) {
        FCell* c = static_cast<FCell*>(ctx);
        if (path != kSvc || len < 6 || args[0] != kBulkOpen) return false;
        const uint32_t in = static_cast<uint32_t>(args[1]) | (static_cast<uint32_t>(args[2]) << 8) |
                            (static_cast<uint32_t>(args[3]) << 16) | (static_cast<uint32_t>(args[4]) << 24);
        CHECK_EQ(in, kIn);
        c->n[1].node->reply_call(from, msg, path, Value::of_u32(c->have_file ? static_cast<uint32_t>(c->file.size())
                                                                                : kBulkNoFile));
        c->pushing = !c->manual && c->have_file && !c->file.empty();
        c->next = 0;
        return true;
    }
    static void on_result(void* ctx, uint16_t, uint16_t, uint32_t path, Node::CallOutcome o, const Value& v) {
        FCell* c = static_cast<FCell*>(ctx);
        if (path == kIn) {
            c->in_flight = false;
            uint32_t at = 0;
            if (o == Node::CallOutcome::Ok && v.as_u32(at)) c->next = at;
            if (c->next >= c->file.size()) c->pushing = false;
        } else {
            c->actor->on_call_result(0x100 + 1, 0, path, o, v);
        }
    }
    static bool on_board_call(void* ctx, uint16_t from, uint16_t msg, uint32_t path, const uint8_t* args, uint16_t len) {
        return static_cast<FCell*>(ctx)->actor->on_call(from, msg, path, args, len) == Actor::kCallAccepted;
    }
    static void on_board_result(void* ctx, uint16_t from, uint16_t msg, uint32_t path, Node::CallOutcome o,
                                const Value& v) {
        static_cast<FCell*>(ctx)->actor->on_call_result(from, msg, path, o, v);
    }
    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        FNode* from = static_cast<FNode*>(ctx);
        FCell* c = from->cell;
        const size_t to = 1 - from->index;
        if (std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0 || std::memcmp(c->n[to].mac, mac, kMacLen) == 0) {
            c->air.push_back(Flight{c->now_us + 2000, from->index, to, std::vector<uint8_t>(data, data + len)});
        }
        return 0;
    }

    FCell() {
        for (size_t i = 0; i < 2; ++i) {
            FNode& t = n[i];
            t.cell = this;
            t.index = i;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(0x60 + i);
            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x100 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, t.mac, kMacLen);
            cfg.hello_interval_ms = 500;
            t.hal.ctx = &t;
            t.hal.send = &FCell::send;
            t.hal.now_ms = [](void* p) { return static_cast<FNode*>(p)->cell->now_us / 1000; };
            t.hal.now_us = [](void* p) { return static_cast<FNode*>(p)->cell->now_us; };
            t.node = new Node(cfg, t.hal);
            t.node->start();
        }
        n[1].node->set_call_handler(&FCell::on_call, this);
        n[1].node->set_call_result(&FCell::on_result, this);
        n[0].node->set_call_handler(&FCell::on_board_call, this);
        n[0].node->set_call_result(&FCell::on_board_result, this);
        FileHashConfig fc{};
        fc.svc_hash = kSvc;
        fc.provider = 0x101;
        fc.in_hash = kIn;
        fc.go_hash = kGo;
        fc.bytes_hash = kBytes;
        fc.state_hash = kState;
        fc.digest_hash = kDigest;
        fc.ms_hash = kMs;
        fc.name_len = 8;
        std::memcpy(fc.name, "test.bin", 8);
        actor = new FileHashActor(*n[0].node, fc);
        CHECK(actor->start(0));
        run(3000);
    }
    ~FCell() {
        delete actor;
        for (FNode& t : n) delete t.node;
    }
    void push_one() {  // the hub's sender, one chunk in flight (the hub keeps a few)
        if (!pushing || in_flight) return;
        const uint32_t len = static_cast<uint32_t>(std::min<size_t>(kBulkChunkMax, file.size() - next));
        std::vector<uint8_t> a = {kBulkVersion, 7, 0, static_cast<uint8_t>(next), static_cast<uint8_t>(next >> 8),
                                  static_cast<uint8_t>(next >> 16), static_cast<uint8_t>(next >> 24)};
        a.insert(a.end(), file.begin() + next, file.begin() + next + len);
        in_flight = n[1].node->request_call(0x100, kIn, a.data(), static_cast<uint16_t>(a.size())) != 0;
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
            for (FNode& t : n) t.node->tick(now_us / 1000);
            actor->tick(now_us / 1000);
            push_one();
        }
    }
    void go() { n[0].node->ns().publish(kGo, Value::of_u32(1), now_us / 1000); }
    uint32_t read32(uint32_t h) {
        Reading r;
        n[0].node->read(h, r);
        uint32_t v = 99;
        r.value.as_u32(v);
        return v;
    }
    uint64_t read64(uint32_t h) {
        Reading r;
        n[0].node->read(h, r);
        uint64_t v = 0;
        r.value.as_u64(v);
        return v;
    }
};

uint64_t sha512_prefix(const std::vector<uint8_t>& data) {
    uint8_t h[64];
    crypto_sha512(h, data.data(), data.size());
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | h[i];
    return v;
}

}  // namespace

TEST(file_hash, a_file_arrives_whole_and_its_sha512_matches) {
    FCell c;
    c.file.resize(5000);
    for (size_t i = 0; i < c.file.size(); ++i) c.file[i] = static_cast<uint8_t>(i * 131 + 7);
    c.go();
    c.run(3000);
    CHECK_EQ(c.actor->state(), kFileDone);
    CHECK_EQ(c.actor->received(), 5000u);
    CHECK_EQ(c.read64(kDigest), sha512_prefix(c.file));
    CHECK_EQ(c.read32(kState), kFileDone);
    CHECK_EQ(c.read32(kBytes), 5000u);
}

TEST(file_hash, no_such_file_fails_and_a_chunk_at_the_wrong_offset_is_not_taken) {
    FCell c;
    c.have_file = false;
    c.go();
    c.run(200);
    CHECK_EQ(c.actor->state(), kFileFailed);
    // a read going on: a chunk ahead of what the board expects is answered with what it expects, and ignored
    c.have_file = true;
    c.file.assign(1000, 0x5A);
    c.go();
    c.run(10);  // asked, answered -- before the first chunk lands
    std::vector<uint8_t> ahead = {kBulkVersion, 7, 0, 0xC0, 0, 0, 0, 1, 2, 3};
    CHECK(c.n[1].node->request_call(0x100, kIn, ahead.data(), static_cast<uint16_t>(ahead.size())) != 0);
    c.run(3000);
    CHECK_EQ(c.actor->state(), kFileDone);
    CHECK_EQ(c.read64(kDigest), sha512_prefix(c.file));  // the stray chunk did not get into the hash
}

TEST(file_hash, a_resent_chunk_is_answered_but_hashed_once) {
    FCell c;
    c.file.resize(400);
    for (size_t i = 0; i < c.file.size(); ++i) c.file[i] = static_cast<uint8_t>(i);
    c.manual = true;  // the test sends by hand: the first chunk, twice
    c.go();
    c.run(10);
    std::vector<uint8_t> a = {kBulkVersion, 7, 0, 0, 0, 0, 0};
    a.insert(a.end(), c.file.begin(), c.file.begin() + 192);
    CHECK(c.n[1].node->request_call(0x100, kIn, a.data(), static_cast<uint16_t>(a.size())) != 0);
    c.run(50);
    CHECK(c.n[1].node->request_call(0x100, kIn, a.data(), static_cast<uint16_t>(a.size())) != 0);
    c.run(50);
    CHECK_EQ(c.actor->received(), 192u);
    c.next = 192;
    c.manual = false;
    c.pushing = true;
    c.run(1000);
    CHECK_EQ(c.read64(kDigest), sha512_prefix(c.file));
}
