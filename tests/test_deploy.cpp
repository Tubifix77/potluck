// §7.4 deploy and detach -- M3's node half: the image, the A/B trial-and-revert, the receiver, and
// the node routing DEPLOY_* between peers.
//
// The trial-and-revert tests are the ones that matter. §7.4 calls the trial "the single feature
// that makes detaching a 30-node mesh survivable": a node that confirms a broken module has no
// fallback left, and a node that never reverts is a node someone has to walk to.

#include <cstring>
#include <map>
#include <vector>

#include "guest_fixture.hpp"
#include "pot/deploy.hpp"
#include "pot/guest_examples.h"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
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

struct ActorSpec {
    uint16_t node;
    uint8_t type;
    std::vector<uint8_t> cfg;
};

std::vector<uint8_t> make_image(uint32_t counter, const std::vector<ActorSpec>& actors) {
    std::vector<uint8_t> body;
    for (const ActorSpec& a : actors) {
        put16(body, a.node);
        body.push_back(a.type);
        body.push_back(static_cast<uint8_t>(a.cfg.size()));
        body.insert(body.end(), a.cfg.begin(), a.cfg.end());
    }
    std::vector<uint8_t> img;
    put32(img, kImageMagic);
    img.push_back(kImageVersion);
    img.push_back(static_cast<uint8_t>(actors.size()));
    put16(img, 0);
    put32(img, counter);
    for (int i = 0; i < 8; ++i) img.push_back(static_cast<uint8_t>(0xA0 + i));
    put32(img, static_cast<uint32_t>(body.size()));
    img.insert(img.end(), body.begin(), body.end());
    return img;
}

std::vector<uint8_t> led_cfg(uint8_t r, uint8_t g, uint8_t b, uint8_t blink, uint16_t period) {
    std::vector<uint8_t> c{r, g, b, blink};
    put16(c, period);
    return c;
}

// A SlotStore in RAM, with the same contract as the flash one.
struct RamStore : SlotStore {
    DeployState state{};
    std::map<uint8_t, std::vector<uint8_t>> slots;
    bool fail_writes = false;

    bool load_state(DeployState& out) override { out = state; return true; }
    bool save_state(const DeployState& s) override { state = s; return true; }
    bool write_slot(uint8_t slot, const uint8_t* data, size_t len) override {
        if (fail_writes) return false;
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

// Push one image through a receiver: BEGIN, CHUNKs of `chunk`, COMMIT. Returns the COMMIT's status
// (or the first failing step's).
DeployStatus deliver(DeployReceiver& rx, const std::vector<uint8_t>& img, uint32_t counter,
                     size_t chunk = 40, uint32_t crc_override = 0) {
    DeployBegin b{};
    b.image_len = static_cast<uint32_t>(img.size());
    b.image_crc = crc32(img.data(), img.size());
    b.rollback_counter = counter;
    DeployReply r = rx.begin(b);
    if (r.status != DeployStatus::Ok) return r.status;
    for (size_t at = 0; at < img.size(); at += chunk) {
        const size_t n = std::min(chunk, img.size() - at);
        DeployChunk c{static_cast<uint32_t>(at), static_cast<uint16_t>(n), img.data() + at};
        r = rx.chunk(c);
        if (r.status != DeployStatus::Ok) return r.status;
    }
    return rx.commit(crc_override != 0 ? crc_override : b.image_crc).status;
}

}  // namespace

// ---- the image ------------------------------------------------------------------------------

TEST(deploy, crc32_is_the_common_crc32) {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    // The standard check value, which is also Python's zlib.crc32(b"123456789") -- verified there,
    // because the host computes image CRCs with zlib and the two must agree.
    CHECK_EQ(crc32(check, sizeof(check)), 0xCBF43926u);
}

TEST(deploy, an_image_parses_into_its_actors) {
    const auto img = make_image(7, {{0x6300, 1, led_cfg(160, 0, 200, 1, 1500)},
                                    {kEveryNode, 1, led_cfg(0, 255, 0, 0, 1000)}});
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    CHECK_EQ(d.rollback_counter, 7u);
    CHECK_EQ(static_cast<int>(d.actor_count), 2);
    CHECK_EQ(d.actors[0].node_id, static_cast<uint16_t>(0x6300));
    LedConfig lc{};
    CHECK(led_config(d.actors[0], lc));
    CHECK_EQ(static_cast<int>(lc.r), 160);
    CHECK_EQ(static_cast<int>(lc.b), 200);
    CHECK_EQ(static_cast<int>(lc.blink), 1);
    CHECK_EQ(static_cast<int>(lc.period_ms), 1500);
    CHECK_EQ(d.actors[1].node_id, kEveryNode);
}

TEST(deploy, malformed_images_are_refused_and_say_why) {
    const auto good = make_image(1, {{0x6300, 1, led_cfg(1, 2, 3, 0, 1000)}});
    DeployImage d{};
    const char* why = nullptr;

    auto bad_magic = good;
    bad_magic[0] ^= 0xFF;
    CHECK(!parse_image(bad_magic.data(), bad_magic.size(), d, &why));

    auto unknown = make_image(1, {{0x6300, 99, {1, 2, 3, 4}}});
    CHECK(!parse_image(unknown.data(), unknown.size(), d, &why));
    CHECK(std::strstr(why, "unknown actor type") != nullptr);

    auto wrong_len = make_image(1, {{0x6300, 1, {1, 2, 3}}});
    CHECK(!parse_image(wrong_len.data(), wrong_len.size(), d, &why));

    auto trailing = good;
    trailing.push_back(0);
    CHECK(!parse_image(trailing.data(), trailing.size(), d, &why));  // body_len now disagrees

    CHECK(!parse_image(good.data(), kImageHeaderLen - 1, d, &why));

    // An LED config outside its range parses as an image but is refused as a config.
    auto silly = make_image(1, {{0x6300, 1, led_cfg(1, 2, 3, 1, 50)}});
    CHECK(parse_image(silly.data(), silly.size(), d, &why));
    LedConfig lc{};
    CHECK(!led_config(d.actors[0], lc));
}

// ---- trial and revert -------------------------------------------------------------------------

TEST(deploy, a_trial_that_never_confirms_reverts_after_its_boots) {
    // §7.4 step 4: "Otherwise the bootloader-level counter reverts it to the previous slot."
    DeployState s{};
    s.active = kSlotA;  // confirmed A
    // Commit B on trial.
    s.previous = kSlotA;
    s.active = kSlotB;
    s.pending = 1;

    CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Trial));
    CHECK_EQ(static_cast<int>(s.trial_boots), 1);
    CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Trial));
    CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Trial));
    // The fourth boot of a trial that crashed three times goes back to A, by itself.
    CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Reverted));
    CHECK_EQ(static_cast<int>(s.active), static_cast<int>(kSlotA));
    CHECK_EQ(static_cast<int>(s.pending), 0);
    CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Confirmed));
}

TEST(deploy, a_first_deployment_that_fails_reverts_to_the_builtin_behaviour) {
    DeployState s{};
    s.active = kSlotA;
    s.pending = 1;  // previous is kSlotNone: nothing was ever confirmed
    decide_boot(s, 1);
    CHECK_EQ(static_cast<int>(decide_boot(s, 1)), static_cast<int>(BootOutcome::Reverted));
    CHECK_EQ(static_cast<int>(s.active), static_cast<int>(kSlotNone));
    CHECK_EQ(static_cast<int>(decide_boot(s, 1)), static_cast<int>(BootOutcome::NoDeployment));
}

TEST(deploy, a_confirmed_trial_stays) {
    DeployState s{};
    s.active = kSlotB;
    s.previous = kSlotA;
    s.pending = 1;
    decide_boot(s, 3);
    confirm(s);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(static_cast<int>(decide_boot(s, 3)), static_cast<int>(BootOutcome::Confirmed));
    }
    CHECK_EQ(static_cast<int>(s.active), static_cast<int>(kSlotB));
}

// ---- the receiver -------------------------------------------------------------------------------

TEST(deploy, a_delivered_image_lands_in_the_inactive_slot_on_trial) {
    RamStore store;
    uint8_t buf[kMaxImageLen];
    DeployReceiver rx(store, buf, sizeof(buf));
    const auto img = make_image(1, {{kEveryNode, 1, led_cfg(160, 0, 200, 1, 1500)}});

    CHECK_EQ(static_cast<int>(deliver(rx, img, 1)), static_cast<int>(DeployStatus::Ok));
    CHECK_EQ(static_cast<int>(store.state.active), static_cast<int>(kSlotA));
    CHECK_EQ(static_cast<int>(store.state.pending), 1);
    CHECK_EQ(static_cast<int>(store.state.previous), static_cast<int>(kSlotNone));
    CHECK(store.slots[kSlotA] == img);

    // Confirm, then a second deployment goes to the *other* slot and keeps A as the fallback.
    confirm(store.state);
    const auto img2 = make_image(2, {{kEveryNode, 1, led_cfg(0, 0, 255, 0, 1000)}});
    CHECK_EQ(static_cast<int>(deliver(rx, img2, 2)), static_cast<int>(DeployStatus::Ok));
    CHECK_EQ(static_cast<int>(store.state.active), static_cast<int>(kSlotB));
    CHECK_EQ(static_cast<int>(store.state.previous), static_cast<int>(kSlotA));
    CHECK(store.slots[kSlotA] == img);  // the fallback is untouched
}

TEST(deploy, nothing_is_deployed_over_a_trial_in_progress) {
    // Writing the inactive slot during a trial would overwrite the trial's only fallback.
    RamStore store;
    uint8_t buf[kMaxImageLen];
    DeployReceiver rx(store, buf, sizeof(buf));
    const auto img = make_image(1, {{kEveryNode, 1, led_cfg(1, 2, 3, 0, 1000)}});
    CHECK_EQ(static_cast<int>(deliver(rx, img, 1)), static_cast<int>(DeployStatus::Ok));
    const auto img2 = make_image(2, {{kEveryNode, 1, led_cfg(3, 2, 1, 0, 1000)}});
    CHECK_EQ(static_cast<int>(deliver(rx, img2, 2)), static_cast<int>(DeployStatus::TrialInProgress));
}

TEST(deploy, a_lower_rollback_counter_is_refused) {
    // §7.4 step 6, anti-downgrade.
    RamStore store;
    store.state.counter = 5;
    uint8_t buf[kMaxImageLen];
    DeployReceiver rx(store, buf, sizeof(buf));
    const auto old_img = make_image(4, {{kEveryNode, 1, led_cfg(1, 2, 3, 0, 1000)}});
    CHECK_EQ(static_cast<int>(deliver(rx, old_img, 4)), static_cast<int>(DeployStatus::Downgrade));
    CHECK(store.slots.empty());
}

TEST(deploy, a_corrupted_or_lying_delivery_is_not_committed) {
    RamStore store;
    uint8_t buf[kMaxImageLen];
    DeployReceiver rx(store, buf, sizeof(buf));
    const auto img = make_image(1, {{kEveryNode, 1, led_cfg(1, 2, 3, 0, 1000)}});
    CHECK_EQ(static_cast<int>(deliver(rx, img, 1, 40, 0xDEADBEEF)),
             static_cast<int>(DeployStatus::CrcMismatch));
    // An image whose header counter disagrees with the BEGIN's.
    CHECK_EQ(static_cast<int>(deliver(rx, img, 2)), static_cast<int>(DeployStatus::BadImage));
    // A chunk out of order.
    DeployBegin b{};
    b.image_len = static_cast<uint32_t>(img.size());
    b.image_crc = crc32(img.data(), img.size());
    b.rollback_counter = 1;
    CHECK_EQ(static_cast<int>(rx.begin(b).status), static_cast<int>(DeployStatus::Ok));
    DeployChunk c{10, 5, img.data() + 10};
    CHECK_EQ(static_cast<int>(rx.chunk(c).status), static_cast<int>(DeployStatus::BadOffset));
    CHECK(store.slots.empty());
    CHECK_EQ(static_cast<int>(store.state.active), static_cast<int>(kSlotNone));
}

TEST(deploy, wire_payloads_round_trip) {
    DeployBegin b{};
    b.image_len = 123;
    b.image_crc = 0x11223344;
    b.rollback_counter = 9;
    for (int i = 0; i < 8; ++i) b.package_digest[i] = static_cast<uint8_t>(i);
    b.flags = kDeployFlagDistribute;
    uint8_t wire[64];
    CHECK_EQ(store_deploy_begin(b, wire, sizeof(wire)), kDeployBeginLen);
    DeployBegin b2{};
    CHECK(load_deploy_begin(wire, kDeployBeginLen, b2));
    CHECK_EQ(b2.image_len, 123u);
    CHECK_EQ(b2.image_crc, 0x11223344u);
    CHECK_EQ(b2.rollback_counter, 9u);
    CHECK_EQ(static_cast<int>(b2.flags), static_cast<int>(kDeployFlagDistribute));
    CHECK(!load_deploy_begin(wire, kDeployBeginLen - 1, b2));

    DeployReply r{};
    r.status = DeployStatus::TrialInProgress;
    r.slot = kSlotB;
    r.peers_ok = 2;
    CHECK_EQ(store_deploy_reply(r, wire, sizeof(wire)), kDeployReplyLen);
    DeployReply r2{};
    CHECK(load_deploy_reply(wire, kDeployReplyLen, r2));
    CHECK_EQ(static_cast<int>(r2.status), static_cast<int>(DeployStatus::TrialInProgress));
    CHECK_EQ(static_cast<int>(r2.peers_ok), 2);
}

// ---- the node routes DEPLOY_* -------------------------------------------------------------------

namespace {

struct DCell;
struct DSlot {
    DCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
};

struct DCell {
    std::vector<DSlot> slots;
    uint32_t now_us = 0;
    bool partitioned = false;

    void build(size_t n) {
        slots.resize(n);
        for (size_t i = 0; i < n; ++i) {
            DSlot& s = slots[i];
            s.cell = this;
            s.index = i;
            s.mac[0] = 0x02;
            s.mac[5] = static_cast<uint8_t>(i);
            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x300 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, s.mac, kMacLen);
            cfg.hello_interval_ms = 500;
            s.hal.ctx = &s;
            s.hal.send = &DCell::send;
            s.hal.now_ms = [](void* c) { return static_cast<DSlot*>(c)->cell->now_us / 1000; };
            s.hal.now_us = [](void* c) { return static_cast<DSlot*>(c)->cell->now_us; };
            s.node = new Node(cfg, s.hal);
        }
    }
    ~DCell() {
        for (DSlot& s : slots) delete s.node;
    }
    void start() {
        for (DSlot& s : slots) s.node->start();
    }
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (DSlot& s : slots) s.node->tick(now_us / 1000);
        }
    }
    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        DSlot* from = static_cast<DSlot*>(ctx);
        DCell* c = from->cell;
        if (c->partitioned) return 0;
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->slots.size(); ++i) {
            if (i == from->index) continue;
            if (bcast || std::memcmp(c->slots[i].mac, mac, kMacLen) == 0) {
                c->slots[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast) from->node->on_tx_done(mac, true, c->now_us);
        return 0;
    }
};

struct ServerSide {
    RamStore store;
    uint8_t buf[kMaxImageLen];
    DeployReceiver rx{store, buf, sizeof(buf)};
    int calls = 0;
};

size_t server_fn(void* ctx, uint16_t, uint16_t, uint8_t op, const uint8_t* p, uint16_t len,
                 uint8_t* reply, size_t cap) {
    ServerSide& s = *static_cast<ServerSide*>(ctx);
    ++s.calls;
    DeployReply r{};
    r.status = DeployStatus::Malformed;
    if (op == kOpDeployBegin) {
        DeployBegin b{};
        if (load_deploy_begin(p, len, b)) r = s.rx.begin(b);
    } else if (op == kOpDeployChunk) {
        DeployChunk c{};
        if (load_deploy_chunk(p, len, c)) r = s.rx.chunk(c);
    } else if (op == kOpDeployCommit) {
        uint32_t crc = 0;
        if (load_deploy_commit(p, len, crc)) r = s.rx.commit(crc);
    }
    return store_deploy_reply(r, reply, cap);
}

struct ClientSide {
    std::vector<std::pair<uint8_t, DeployReply>> results;
    int timeouts = 0;
};

void result_fn(void* ctx, uint16_t, uint16_t, uint8_t op, bool timed_out, const uint8_t* p,
               uint16_t len) {
    ClientSide& c = *static_cast<ClientSide*>(ctx);
    if (timed_out) {
        ++c.timeouts;
        return;
    }
    DeployReply r{};
    CHECK(load_deploy_reply(p, len, r));
    c.results.push_back({op, r});
}

}  // namespace

TEST(deploy, a_node_deploys_to_a_peer_over_the_link) {
    DCell c;
    c.build(2);
    c.start();
    c.advance_ms(300);
    Node& sender = *c.slots[0].node;
    Node& target = *c.slots[1].node;
    ServerSide server;
    ClientSide client;
    target.set_deploy_server(server_fn, &server);
    sender.set_deploy_result(result_fn, &client);

    const auto img = make_image(1, {{kEveryNode, 1, led_cfg(160, 0, 200, 1, 1500)}});
    uint8_t wire[600];
    DeployBegin b{};
    b.image_len = static_cast<uint32_t>(img.size());
    b.image_crc = crc32(img.data(), img.size());
    b.rollback_counter = 1;
    size_t n = store_deploy_begin(b, wire, sizeof(wire));
    CHECK(sender.send_deploy(target.config().node_id, kOpDeployBegin, wire, static_cast<uint16_t>(n)) != 0);
    c.advance_ms(20);
    n = store_deploy_chunk(0, img.data(), static_cast<uint16_t>(img.size()), wire, sizeof(wire));
    CHECK(sender.send_deploy(target.config().node_id, kOpDeployChunk, wire, static_cast<uint16_t>(n)) != 0);
    c.advance_ms(20);
    n = store_deploy_commit(b.image_crc, wire, sizeof(wire));
    CHECK(sender.send_deploy(target.config().node_id, kOpDeployCommit, wire, static_cast<uint16_t>(n)) != 0);
    c.advance_ms(20);

    CHECK_EQ(client.results.size(), static_cast<size_t>(3));
    for (const auto& r : client.results) {
        CHECK_EQ(static_cast<int>(r.second.status), static_cast<int>(DeployStatus::Ok));
    }
    CHECK_EQ(static_cast<int>(client.results.back().first), static_cast<int>(kOpDeployCommit));
    CHECK_EQ(static_cast<int>(server.store.state.pending), 1);
    CHECK(server.store.slots[kSlotA] == img);
    CHECK_EQ(client.timeouts, 0);
}

TEST(deploy, an_unanswered_deploy_step_times_out_to_the_client_once) {
    DCell c;
    c.build(2);
    c.start();
    c.advance_ms(300);
    Node& sender = *c.slots[0].node;
    ClientSide client;
    sender.set_deploy_result(result_fn, &client);
    // No server on the target: it answers ERR, not REPLY, so the request is never matched.
    uint8_t wire[64];
    DeployBegin b{};
    b.image_len = 10;
    const size_t n = store_deploy_begin(b, wire, sizeof(wire));
    CHECK(sender.send_deploy(c.slots[1].node->config().node_id, kOpDeployBegin, wire,
                             static_cast<uint16_t>(n)) != 0);
    c.advance_ms(5000);
    CHECK_EQ(client.timeouts, 1);
    CHECK(client.results.empty());
}

TEST(deploy, only_deploy_opcodes_can_be_sent_as_deploys) {
    DCell c;
    c.build(2);
    c.start();
    c.advance_ms(300);
    uint8_t wire[4] = {0, 0, 0, 0};
    CHECK_EQ(c.slots[0].node->send_deploy(c.slots[1].node->config().node_id, kOpRead, wire, 4),
             static_cast<uint16_t>(0));
}

namespace {
struct OpSeen {
    std::vector<uint8_t> ops;
};
size_t record_fn(void* ctx, uint16_t, uint16_t, uint8_t op, const uint8_t*, uint16_t, uint8_t* reply, size_t cap) {
    static_cast<OpSeen*>(ctx)->ops.push_back(op);
    if (cap < 1) return 0;
    reply[0] = op;
    return 1;
}
}  // namespace

TEST(deploy, every_opcode_in_the_deploy_range_reaches_the_deploy_server) {
    // M11's FW_* opcodes first reached the bench answered with ERR unknown_opcode: the range check
    // admitted them, the node's dispatch did not. Every opcode the range admits must arrive.
    DCell c;
    c.build(2);
    c.start();
    c.advance_ms(300);
    OpSeen seen;
    ClientSide client;
    c.slots[1].node->set_deploy_server(record_fn, &seen);
    uint8_t wire[4] = {0x60, 0x81, 0, 0};
    std::vector<uint8_t> want;
    for (int op = 0; op < 256; ++op) {
        if (!is_deploy_opcode(static_cast<uint8_t>(op))) continue;
        want.push_back(static_cast<uint8_t>(op));
        CHECK(c.slots[0].node->send_deploy(c.slots[1].node->config().node_id, static_cast<uint8_t>(op), wire, 4) != 0);
        c.advance_ms(20);
    }
    CHECK(seen.ops == want);
    CHECK_EQ(want.size(), static_cast<size_t>(8));  // DEPLOY_BEGIN..ABORT, FW_BEGIN..STATUS
    CHECK_EQ(c.slots[1].node->counters().rx_unknown_opcode, 0u);
}

TEST(deploy, the_host_compilers_golden_image_parses_here) {
    // host/potluck/tests/test_deploy.py checks that compile_image() produces exactly these bytes;
    // this checks the node reads them as the host meant. Neither side can drift alone.
    const char* hex = "504f54440101000003000000bfba6702f4f20e6d0a00000000630106a000c801dc05";
    std::vector<uint8_t> img;
    for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2) {
        auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
        img.push_back(static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    }
    DeployImage d{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), d, &why));
    CHECK_EQ(d.rollback_counter, 3u);
    CHECK_EQ(static_cast<int>(d.actor_count), 1);
    CHECK_EQ(d.actors[0].node_id, static_cast<uint16_t>(0x6300));
    LedConfig lc{};
    CHECK(led_config(d.actors[0], lc));
    CHECK_EQ(static_cast<int>(lc.r), 160);
    CHECK_EQ(static_cast<int>(lc.g), 0);
    CHECK_EQ(static_cast<int>(lc.b), 200);
    CHECK_EQ(static_cast<int>(lc.blink), 1);
    CHECK_EQ(static_cast<int>(lc.period_ms), 1500);
}

// ---- M5 step 6: the verifier hook ----

namespace {
int g_verify_calls = 0;
size_t g_seen_image = 0, g_seen_trailer = 0;
DeployStatus verify_refuse(void*, const uint8_t*, size_t image_len, const uint8_t*, size_t trailer_len) {
    ++g_verify_calls;
    g_seen_image = image_len;
    g_seen_trailer = trailer_len;
    return DeployStatus::BadSignature;
}
DeployStatus verify_accept(void*, const uint8_t*, size_t image_len, const uint8_t*, size_t trailer_len) {
    ++g_verify_calls;
    g_seen_image = image_len;
    g_seen_trailer = trailer_len;
    return DeployStatus::Ok;
}
}  // namespace

TEST(deploy, a_refused_signature_writes_nothing_and_an_accepted_one_stores_the_image_alone) {
    const std::vector<uint8_t> img = make_image(10, {{0x6300, 1, led_cfg(1, 2, 3, 0, 0)}});
    std::vector<uint8_t> stream = img;
    stream.resize(img.size() + 176, 0xAB);  // the verifier, not the receiver, judges the trailer
    for (int round = 0; round < 2; ++round) {
        RamStore store;
        uint8_t buf[kMaxImageLen];
        DeployReceiver rx(store, buf, sizeof(buf));
        rx.set_verifier(round == 0 ? &verify_refuse : &verify_accept, nullptr);
        g_verify_calls = 0;
        const DeployStatus st = deliver(rx, stream, 10);
        CHECK_EQ(g_verify_calls, 1);
        CHECK_EQ(g_seen_image, img.size());
        CHECK_EQ(g_seen_trailer, static_cast<size_t>(176));
        if (round == 0) {
            CHECK(st == DeployStatus::BadSignature);
            CHECK(store.slots.empty());
            CHECK_EQ(static_cast<int>(store.state.pending), 0);
        } else {
            CHECK(st == DeployStatus::Ok);
            CHECK_EQ(store.slots.begin()->second.size(), img.size());  // the image, not the trailer
        }
    }
}

TEST(deploy, a_version_2_image_ends_after_its_guest_section_not_at_body_len) {
    // Found on the bench (M7, M0-LOG session 38): the receiver took 24 + body_len as the image and the
    // guest section as part of the trailer, so every signed image carrying a guest failed as bad_length.
    guestfx::Keys k;
    const std::vector<uint8_t> blob =
        guestfx::make_blob(k, guest_examples::k_endless_tick, sizeof(guest_examples::k_endless_tick), {},
                           {{path_hash("potluck://lab/act/spin/out"), ValueType::I32}});
    const std::vector<uint8_t> img =
        guestfx::make_image({guestfx::guest_decl(path_hash("potluck://lab/act/spin/out"), 1000, {0x6300}, 0)}, {blob}, 10);
    CHECK_EQ(image_extent(img.data(), img.size()), img.size());
    CHECK_EQ(image_extent(img.data(), img.size() - 1), static_cast<size_t>(0));  // the guest section is cut short
    std::vector<uint8_t> stream = img;
    stream.resize(img.size() + 176, 0xAB);
    CHECK_EQ(image_extent(stream.data(), stream.size()), img.size());
    RamStore store;
    static uint8_t buf[kMaxImageLen];
    DeployReceiver rx(store, buf, sizeof(buf));
    rx.set_verifier(&verify_accept, nullptr);
    g_verify_calls = 0;
    CHECK(deliver(rx, stream, 10) == DeployStatus::Ok);
    CHECK_EQ(g_verify_calls, 1);
    CHECK_EQ(g_seen_image, img.size());
    CHECK_EQ(g_seen_trailer, static_cast<size_t>(176));
    CHECK_EQ(store.slots.begin()->second.size(), img.size());
}
