// Section 9.3, M5 step 3: the signed HELLO, the session key, and a cell that admits only nodes its CA
// enrolled. The primitives first, then real Nodes on a perfect simulated channel.

#include <cstring>
#include <memory>
#include <vector>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "pot/hello_auth.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/trust.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

// A CA that can issue certificates in-test. The format is pinned separately by the golden
// certificate shared with the host tool (test_trust.cpp / test_enrol.py); this only has to produce it.
struct TestCa {
    uint8_t seed[32];
    uint8_t sk[64];
    uint8_t pub[32];
    explicit TestCa(uint8_t salt) {
        for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(salt + 3 * i);
        uint8_t s[32];
        std::memcpy(s, seed, 32);
        crypto_ed25519_key_pair(sk, pub, s);
    }
    void issue(uint16_t node_id, const uint8_t node_pub[32], uint8_t cert[kNodeCertLen], uint32_t issued = 0) const {
        std::memset(cert, 0, kNodeCertLen);
        cert[0] = 'P';
        cert[1] = 'N';
        cert[2] = 'C';
        cert[3] = '1';
        cert[4] = 1;
        cert[5] = 1;
        cert[6] = static_cast<uint8_t>(node_id);
        cert[7] = static_cast<uint8_t>(node_id >> 8);
        ca_fingerprint(pub, cert + 8);
        for (int k = 0; k < 4; ++k) cert[12 + k] = static_cast<uint8_t>(issued >> (8 * k));
        std::memcpy(cert + 16, node_pub, 32);
        static const char kDomain[] = "potluck-node-cert-v1";
        uint8_t msg[sizeof(kDomain) + 48];
        std::memcpy(msg, kDomain, sizeof(kDomain));
        std::memcpy(msg + sizeof(kDomain), cert, 48);
        crypto_ed25519_sign(cert + 48, sk, msg, sizeof(msg));
    }
};

Identity enrolled_identity(const TestCa& ca, uint16_t node_id, uint8_t salt, uint32_t issued = 0) {
    Identity id;
    uint8_t seed[32];
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(salt ^ (7 * i));
    identity_from_seed(id, seed);
    uint8_t cert[kNodeCertLen];
    ca.issue(node_id, id.pub, cert, issued);
    CHECK(identity_install_cert(id, node_id, ca.pub, cert, kNodeCertLen) == CertError::Ok);
    return id;
}

uint8_t kMacA[6] = {0x02, 0, 0, 0, 0, 0xA1};
uint8_t kMacB[6] = {0x02, 0, 0, 0, 0, 0xB2};

void base_hello(uint16_t node_id, uint32_t epoch, uint8_t out[kHelloBaseLen]) {
    HelloPayload h{};
    h.boot_epoch = epoch;
    h.node_id = node_id;
    h.espnow_version = 2;
    h.hb_period_cs = 10;
    h.hb_miss_limit = 6;
    std::memcpy(out, &h, kHelloBaseLen);
}

// ---- a cell whose nodes carry identities ----

struct AuthCell;
struct AuthNode {
    AuthCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    std::unique_ptr<Identity> id;  // stable address: the Node keeps a pointer
    Node* node = nullptr;
    NodeHal hal{};
    std::vector<Event> events;
    bool muted = false;  // drops everything this node sends
    uint8_t chan = 1;    // M8.1 bench: frames reach only nodes on the sender's channel
};

struct AuthCell {
    std::vector<AuthNode> nodes;
    uint32_t now_us = 0;
    std::vector<uint8_t> last_hello_from[8];  // the most recent HELLO payload each node broadcast
    // The most recent unicast WRITE on the air, exactly as sent: what an attacker would record.
    std::vector<uint8_t> last_write;
    size_t last_write_from = 0;
    std::vector<uint8_t> last_safe_state;  // likewise, the most recent SAFE_STATE broadcast
    std::vector<uint8_t> persisted[8];     // what each node asked to persist (its high-water table)
    std::vector<std::pair<size_t, size_t>> deaf;  // ADR-009: pairs out of each other's range

    // `ids[i]` null means: no identity at all (pre-M5 behaviour). `require` applies to every node
    // that has one.
    bool channels = false;  // M8.1 bench: give every node a retunable radio (set before build)
    void build(std::vector<std::unique_ptr<Identity>> ids, bool require, std::vector<uint32_t> epochs = {}) {
        nodes.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            AuthNode& t = nodes[i];
            t.cell = this;
            t.index = i;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(0x10 + i);
            t.id = std::move(ids[i]);
            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x100 + i);
            cfg.boot_epoch = epochs.empty() ? 1 : epochs[i];
            std::memcpy(cfg.mac, t.mac, kMacLen);
            cfg.hello_interval_ms = 500;
            t.hal.ctx = &t;
            t.hal.send = &AuthCell::send;
            t.hal.now_ms = [](void* c) { return static_cast<AuthNode*>(c)->cell->now_us / 1000; };
            t.hal.now_us = [](void* c) { return static_cast<AuthNode*>(c)->cell->now_us; };
            t.hal.on_event = [](void* c, const Event& e) { static_cast<AuthNode*>(c)->events.push_back(e); };
            if (channels) t.hal.set_channel = [](void* c, uint8_t ch) { static_cast<AuthNode*>(c)->chan = ch; };
            t.hal.persist_safe_state_floors = [](void* c, const void* table, size_t bytes) {
                AuthNode* n = static_cast<AuthNode*>(c);
                const uint8_t* b = static_cast<const uint8_t*>(table);
                if (n->index < 8) n->cell->persisted[n->index].assign(b, b + bytes);
            };
            t.node = new Node(cfg, t.hal);
            if (t.id) t.node->set_trust(t.id.get(), require);
        }
    }
    ~AuthCell() {
        for (AuthNode& t : nodes) delete t.node;
    }
    void start_all() {
        for (AuthNode& t : nodes) t.node->start();
    }
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (AuthNode& t : nodes) t.node->tick(now_us / 1000);
        }
    }
    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        AuthNode* from = static_cast<AuthNode*>(ctx);
        AuthCell* c = from->cell;
        if (from->muted) return 0;
        Frame f;
        if (parse(data, len, f, kMaxPayloadV2) == FrameError::Ok && f.hdr.opcode == kOpHello && from->index < 8) {
            c->last_hello_from[from->index].assign(f.payload, f.payload + f.payload_len);
        }
        if (parse(data, len, f, kMaxPayloadV2) == FrameError::Ok && f.hdr.opcode == kOpSafeState) {
            c->last_safe_state.assign(data, data + len);
        }
        if (parse(data, len, f, kMaxPayloadV2) == FrameError::Ok && f.hdr.opcode == kOpWrite) {
            c->last_write.assign(data, data + len);
            c->last_write_from = from->index;
        }
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            if (i == from->index) continue;
            bool is_deaf = false;
            for (const auto& d : c->deaf) {
                if ((d.first == from->index && d.second == i) || (d.first == i && d.second == from->index)) is_deaf = true;
            }
            if (is_deaf) continue;
            if (c->nodes[i].chan != from->chan) continue;
            if (bcast || std::memcmp(c->nodes[i].mac, mac, kMacLen) == 0) {
                c->nodes[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast) from->node->on_tx_done(mac, true, c->now_us);
        return 0;
    }
};

size_t alive(Node& n) { return n.peers().count_in_state(PeerState::Alive); }

const PeerLink* peer_by_id(Node& n, uint16_t id) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const PeerLink& p = n.peers().slot(i);
        if (p.state != PeerState::Free && p.node_id == id) return &p;
    }
    return nullptr;
}

size_t refusals_of(const AuthNode& t, uint16_t claimed, HelloAuthError why) {
    size_t n = 0;
    for (const Event& e : t.events) {
        if (e.kind == EventKind::PeerRefused && e.node_id == claimed && e.detail_a == static_cast<uint32_t>(why)) ++n;
    }
    return n;
}

// Inject a HELLO frame from `mac` claiming `src`, with an arbitrary payload.
void inject_hello(Node& to, const uint8_t mac[6], uint16_t src, const uint8_t* payload, size_t len, uint32_t now_us) {
    EncodeSpec s;
    s.src = src;
    s.dst = kNodeBroadcast;
    s.opcode = kOpHello;
    uint8_t buf[300];
    size_t n = 0;
    CHECK(encode(s, payload, static_cast<uint16_t>(len), buf, sizeof(buf), n) == FrameError::Ok);
    to.on_rx(mac, buf, n, now_us, -50);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The primitives
// ---------------------------------------------------------------------------------------------

TEST(auth, a_signed_hello_verifies_and_fits_one_esp_now_frame) {
    TestCa ca(1);
    Identity a = enrolled_identity(ca, 0x6300, 0x11);
    uint8_t base[kHelloBaseLen], signed_hello[kHelloSignedLen];
    base_hello(0x6300, 7, base);
    CHECK(hello_sign(a, kMacA, base, signed_hello));
    CHECK(kHelloSignedLen <= kMaxPayloadV1);
    const HelloAuth r = hello_verify(ca.pub, kMacA, signed_hello, kHelloSignedLen, 0x6300);
    CHECK(r.error == HelloAuthError::Ok);
    CHECK(std::memcmp(r.peer_pub, a.pub, 32) == 0);
}

TEST(auth, every_byte_of_a_signed_hello_matters) {
    TestCa ca(1);
    Identity a = enrolled_identity(ca, 0x6300, 0x11);
    uint8_t base[kHelloBaseLen], sh[kHelloSignedLen];
    base_hello(0x6300, 7, base);
    CHECK(hello_sign(a, kMacA, base, sh));
    for (size_t i = 0; i < kHelloSignedLen; ++i) {
        uint8_t bad[kHelloSignedLen];
        std::memcpy(bad, sh, sizeof(bad));
        bad[i] ^= 0x01;
        CHECK(hello_verify(ca.pub, kMacA, bad, kHelloSignedLen, 0x6300).error != HelloAuthError::Ok);
    }
}

TEST(auth, a_recorded_hello_does_not_work_from_another_radio_or_under_another_name) {
    TestCa ca(1);
    Identity a = enrolled_identity(ca, 0x6300, 0x11);
    uint8_t base[kHelloBaseLen], sh[kHelloSignedLen];
    base_hello(0x6300, 7, base);
    CHECK(hello_sign(a, kMacA, base, sh));
    CHECK(hello_verify(ca.pub, kMacB, sh, kHelloSignedLen, 0x6300).error == HelloAuthError::BadSignature);
    CHECK(hello_verify(ca.pub, kMacA, sh, kHelloSignedLen, 0x7368).error == HelloAuthError::IdMismatch);
    TestCa other(99);
    const HelloAuth r = hello_verify(other.pub, kMacA, sh, kHelloSignedLen, 0x6300);
    CHECK(r.error == HelloAuthError::Cert);
    CHECK(r.cert_error == CertError::OtherCa);
    CHECK(hello_verify(ca.pub, kMacA, base, kHelloBaseLen, 0x6300).error == HelloAuthError::Unsigned);
    CHECK(hello_verify(ca.pub, kMacA, sh, 100, 0x6300).error == HelloAuthError::BadLength);
}

TEST(auth, both_ends_derive_the_same_session_key_and_a_reboot_changes_it) {
    TestCa ca(1);
    Identity a = enrolled_identity(ca, 0x6300, 0x11);
    Identity b = enrolled_identity(ca, 0x7368, 0x22);
    uint8_t ka[32], kb[32], kb2[32];
    CHECK(session_key(a, 0x6300, 5, b.pub, 0x7368, 9, ka));
    CHECK(session_key(b, 0x7368, 9, a.pub, 0x6300, 5, kb));
    CHECK(std::memcmp(ka, kb, 32) == 0);
    CHECK(session_key(b, 0x7368, 10, a.pub, 0x6300, 5, kb2));  // b rebooted
    CHECK(std::memcmp(ka, kb2, 32) != 0);
    uint8_t zero[32] = {};
    CHECK(std::memcmp(ka, zero, 32) != 0);
}

TEST(auth, the_x25519_secret_matches_the_ed25519_public_key) {
    // session_key() takes the Montgomery secret from SHA-512(seed) and the peer's Montgomery public
    // key from the birational map. Both ends agree only if those correspond; check it directly.
    TestCa ca(1);
    Identity a = enrolled_identity(ca, 0x6300, 0x11);
    uint8_t h[64], x_from_secret[32], x_from_public[32];
    crypto_sha512(h, a.seed, 32);
    crypto_x25519_public_key(x_from_secret, h);
    crypto_eddsa_to_x25519(x_from_public, a.pub);
    CHECK(std::memcmp(x_from_secret, x_from_public, 32) == 0);
}

// ---------------------------------------------------------------------------------------------
// A cell
// ---------------------------------------------------------------------------------------------

TEST(auth, two_enrolled_nodes_admit_each_other_with_a_shared_key) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
    c.build(std::move(ids), true);
    c.start_all();
    c.advance_ms(3000);
    Node& a = *c.nodes[0].node;
    Node& b = *c.nodes[1].node;
    CHECK_EQ(alive(a), static_cast<size_t>(1));
    CHECK_EQ(alive(b), static_cast<size_t>(1));
    const Node::PeerAuth* ab = a.peer_auth(peer_by_id(a, 0x101));
    const Node::PeerAuth* ba = b.peer_auth(peer_by_id(b, 0x100));
    CHECK(ab != nullptr && ab->verified);
    CHECK(ba != nullptr && ba->verified);
    CHECK(std::memcmp(ab->key, ba->key, 32) == 0);
    // Verified once; every later HELLO (every 500 ms here) is recognised, not re-verified.
    CHECK_EQ(a.auth_counters().verified, 1u);
    CHECK(a.auth_counters().cache_hits >= 4u);
    CHECK_EQ(a.auth_counters().refused, 0u);
    CHECK_EQ(c.last_hello_from[0].size(), kHelloSignedLen);  // and what went on the air was signed
}

TEST(auth, an_unenrolled_node_is_refused_and_logged_once_per_ten_seconds) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
    auto unenrolled = std::make_unique<Identity>();
    uint8_t seed[32] = {5};
    identity_from_seed(*unenrolled, seed);  // has a key, never enrolled
    ids.push_back(std::move(unenrolled));
    c.build(std::move(ids), true);
    c.start_all();
    c.advance_ms(12000);
    // A and B see each other and nobody else.
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(1));
    CHECK_EQ(alive(*c.nodes[1].node), static_cast<size_t>(1));
    CHECK(peer_by_id(*c.nodes[0].node, 0x102) == nullptr);
    // The unenrolled node sent unsigned HELLOs, every 500 ms, refused every time ...
    CHECK_EQ(c.last_hello_from[2].size(), kHelloBaseLen);
    CHECK(c.nodes[0].node->auth_counters().refused >= 20u);
    // ... but logged as an event at most once per 10 s: two in 12 s.
    CHECK_EQ(refusals_of(c.nodes[0], 0x102, HelloAuthError::Unsigned), static_cast<size_t>(2));
    // And it, having no CA, refuses everyone in turn: an unenrolled node joins no cell.
    CHECK_EQ(alive(*c.nodes[2].node), static_cast<size_t>(0));
    CHECK(refusals_of(c.nodes[2], 0x100, HelloAuthError::NotEnrolled) >= 1u);
}

TEST(auth, a_node_enrolled_by_another_ca_is_refused_with_the_reason) {
    TestCa ca(1), rogue(77);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(rogue, 0x101, 0x22)));
    c.build(std::move(ids), true);
    c.start_all();
    c.advance_ms(2000);
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(0));
    bool saw = false;
    for (const Event& e : c.nodes[0].events) {
        if (e.kind == EventKind::PeerRefused && e.node_id == 0x101 &&
            e.detail_a == static_cast<uint32_t>(HelloAuthError::Cert) &&
            e.detail_b == static_cast<uint32_t>(CertError::OtherCa)) {
            saw = true;
        }
    }
    CHECK(saw);
}

TEST(auth, a_certificate_cannot_be_worn_under_another_node_id) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    // Genuinely enrolled -- as 0x2222. It runs as 0x101.
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x2222, 0x22)));
    c.build(std::move(ids), true);
    c.start_all();
    c.advance_ms(2000);
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(0));
    CHECK(refusals_of(c.nodes[0], 0x101, HelloAuthError::IdMismatch) >= 1u);
}

TEST(auth, an_old_incarnations_hello_replayed_after_a_reboot_changes_nothing) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
    c.build(std::move(ids), true, {1, 1});
    c.start_all();
    c.advance_ms(1000);
    const std::vector<uint8_t> old_hello = c.last_hello_from[1];  // B, epoch 1
    CHECK_EQ(old_hello.size(), kHelloSignedLen);

    // B reboots: same identity, epoch 2.
    AuthNode& bn = c.nodes[1];
    delete bn.node;
    NodeConfig cfg;
    cfg.node_id = 0x101;
    cfg.boot_epoch = 2;
    std::memcpy(cfg.mac, bn.mac, kMacLen);
    cfg.hello_interval_ms = 500;
    bn.node = new Node(cfg, bn.hal);
    bn.node->set_trust(bn.id.get(), true);
    bn.node->start();
    c.advance_ms(1500);
    Node& a = *c.nodes[0].node;
    const Node::PeerAuth* pa = a.peer_auth(peer_by_id(a, 0x101));
    CHECK(pa != nullptr && pa->verified);
    CHECK_EQ(pa->epoch, 2u);
    uint8_t key_before[32];
    std::memcpy(key_before, pa->key, 32);
    const uint32_t stale_before = a.auth_counters().stale_epoch;

    // The attacker replays B's genuine epoch-1 HELLO from B's own MAC.
    inject_hello(a, bn.mac, 0x101, old_hello.data(), old_hello.size(), c.now_us);
    CHECK_EQ(a.auth_counters().stale_epoch, stale_before + 1);
    CHECK_EQ(pa->epoch, 2u);
    CHECK(std::memcmp(pa->key, key_before, 32) == 0);
}

TEST(auth, forged_hellos_cannot_buy_more_than_the_verification_budget) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    c.build(std::move(ids), true);
    c.start_all();
    c.advance_ms(10);
    // Ten forged HELLOs in the same millisecond, each with a valid certificate (copied from a real
    // node) and a garbage signature, so each would cost a full check.
    Identity real = enrolled_identity(ca, 0x0BAD, 0x33);
    uint8_t base[kHelloBaseLen], sh[kHelloSignedLen];
    base_hello(0x0BAD, 1, base);
    CHECK(hello_sign(real, kMacB, base, sh));
    sh[kHelloSignedLen - 1] ^= 0xFF;
    Node& a = *c.nodes[0].node;
    for (int i = 0; i < 10; ++i) inject_hello(a, kMacB, 0x0BAD, sh, sizeof(sh), c.now_us);
    CHECK_EQ(a.auth_counters().refused, 4u);       // kVerifyBurst
    CHECK_EQ(a.auth_counters().rate_limited, 6u);
    c.advance_ms(1000);                             // refills
    inject_hello(a, kMacB, 0x0BAD, sh, sizeof(sh), c.now_us);
    CHECK_EQ(a.auth_counters().refused, 5u);
}

TEST(auth, the_host_cable_is_trusted_without_a_signature_and_nothing_else_is) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    c.build(std::move(ids), true);
    // Rebuild node 0 with a trusted MAC.
    AuthNode& an = c.nodes[0];
    delete an.node;
    NodeConfig cfg;
    cfg.node_id = 0x100;
    cfg.boot_epoch = 1;
    std::memcpy(cfg.mac, an.mac, kMacLen);
    const uint8_t host_mac[6] = {0x02, 0, 0, 0, 0, 0xFE};
    cfg.has_trusted_mac = true;
    std::memcpy(cfg.trusted_mac, host_mac, 6);
    an.node = new Node(cfg, an.hal);
    an.node->set_trust(an.id.get(), true);
    an.node->start();
    uint8_t base[kHelloBaseLen];
    base_hello(0xFFFE, 1, base);
    inject_hello(*an.node, host_mac, 0xFFFE, base, sizeof(base), c.now_us);
    CHECK(peer_by_id(*an.node, 0xFFFE) != nullptr);
    base_hello(0x0042, 1, base);
    inject_hello(*an.node, kMacB, 0x0042, base, sizeof(base), c.now_us);  // same bytes, not the cable
    CHECK(peer_by_id(*an.node, 0x0042) == nullptr);
}

TEST(auth, the_host_cable_carries_v2_sized_frames_and_an_unpinned_radio_peer_does_not) {
    // M11: a firmware chunk of ~1 KB arrives over the cable. Section 5.3's 226 B floor is for a radio
    // peer whose ESP-NOW version is unknown; the cable is serial and carries the v2 MTU.
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    c.build(std::move(ids), true);
    AuthNode& an = c.nodes[0];
    delete an.node;
    NodeConfig cfg;
    cfg.node_id = 0x100;
    cfg.boot_epoch = 1;
    std::memcpy(cfg.mac, an.mac, kMacLen);
    const uint8_t host_mac[6] = {0x02, 0, 0, 0, 0, 0xFE};
    cfg.has_trusted_mac = true;
    std::memcpy(cfg.trusted_mac, host_mac, 6);
    an.node = new Node(cfg, an.hal);
    an.node->set_trust(an.id.get(), true);
    an.node->start();
    static int served = 0;
    static size_t served_len = 0;
    served = 0;
    an.node->set_deploy_server(
        [](void*, uint16_t, uint16_t, uint8_t, const uint8_t*, uint16_t len, uint8_t* reply, size_t cap) -> size_t {
            ++served;
            served_len = len;
            if (cap < 1) return 0;
            reply[0] = 0;
            return 1;
        },
        nullptr);
    uint8_t base[kHelloBaseLen];
    base_hello(0xFFFE, 1, base);
    inject_hello(*an.node, host_mac, 0xFFFE, base, sizeof(base), c.now_us);
    CHECK(peer_by_id(*an.node, 0xFFFE) != nullptr);

    std::vector<uint8_t> payload(1032, 0x5A);
    payload[0] = 0x00;  // a target that is the node itself
    payload[1] = 0x01;
    EncodeSpec spec;
    spec.src = 0xFFFE;
    spec.dst = 0x100;
    spec.opcode = kOpFwChunk;
    spec.msg_id = 7;
    spec.ack_req = true;
    std::vector<uint8_t> wire(encoded_size(spec, static_cast<uint16_t>(payload.size())));
    size_t n = 0;
    CHECK(encode(spec, payload.data(), static_cast<uint16_t>(payload.size()), wire.data(), wire.size(), n) == FrameError::Ok);
    an.node->on_rx(host_mac, wire.data(), n, c.now_us, 0);
    CHECK_EQ(served, 1);
    CHECK_EQ(served_len, payload.size());
    const uint32_t bad_before = an.node->counters().rx_bad_frame;
    an.node->on_rx(kMacB, wire.data(), n, c.now_us, 0);  // the same bytes from a radio MAC
    CHECK_EQ(served, 1);
    CHECK_EQ(an.node->counters().rx_bad_frame, bad_before + 1);
}

TEST(auth, every_signature_and_key_operation_goes_through_run_heavy) {
    // On the board the link task cannot hold an Ed25519 operation on its stack, so the node must
    // hand every one to run_heavy. Count them: if one ever runs inline again, the board overflows.
    static int calls = 0;
    calls = 0;
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
    c.build(std::move(ids), true);
    for (AuthNode& t : c.nodes) {
        t.hal.run_heavy = [](void*, void (*fn)(void*), void* arg) {
            ++calls;
            fn(arg);
        };
        // NodeHal is copied into the Node at construction, so rebuild each with the hooked HAL.
        delete t.node;
        NodeConfig cfg;
        cfg.node_id = static_cast<uint16_t>(0x100 + t.index);
        cfg.boot_epoch = 1;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        t.node = new Node(cfg, t.hal);
        t.node->set_trust(t.id.get(), true);
    }
    c.start_all();
    c.advance_ms(3000);
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(1));
    // Per node: one signing (the want-ack HELLO, cached after) and one verification of the peer.
    CHECK_EQ(calls, 4);
}

TEST(auth, without_require_an_unsigned_peer_is_still_admitted) {
    // The migration path: an enrolled node that does not yet require auth still forms a cell with
    // pre-M5 firmware, and still signs its own HELLOs.
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(nullptr);
    c.build(std::move(ids), false);
    c.start_all();
    c.advance_ms(2000);
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(1));
    CHECK_EQ(alive(*c.nodes[1].node), static_cast<size_t>(1));  // and the old firmware takes 200 B HELLOs
    CHECK_EQ(c.last_hello_from[0].size(), kHelloSignedLen);
}

// ---------------------------------------------------------------------------------------------
// Step 4: tagged frames and the replay window, in a cell
// ---------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kSetpoint = path_hash("act/setpoint");

void declare_setpoint(Node& n, uint16_t owner) {
    NsDecl d;
    d.path_hash = kSetpoint;
    d.owner_node = owner;
    d.type = ValueType::I32;
    d.access = Access::ReadWrite;
    d.latency_class = kClassL3;
    CHECK(n.ns().declare(d) == NsError::Ok);
}

int32_t setpoint_of(Node& n) {
    Reading r;
    n.read(kSetpoint, r);
    int32_t v = -1;
    Quality q;
    r.get_i32(v, q);
    return v;
}

// Two enrolled nodes, admitted to each other; 0x101 owns a writable setpoint.
struct WriteCell {
    TestCa ca{1};
    AuthCell c;
    WriteCell() {
        std::vector<std::unique_ptr<Identity>> ids;
        ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
        ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
        c.build(std::move(ids), true);
        declare_setpoint(*c.nodes[1].node, 0x101);
        c.start_all();
        c.advance_ms(2000);
    }
    Node& a() { return *c.nodes[0].node; }
    Node& b() { return *c.nodes[1].node; }
};

}  // namespace

TEST(auth, frames_between_verified_peers_are_tagged_and_check) {
    WriteCell w;
    CHECK(w.a().auth_counters().tags_ok > 0u);  // probes and replies, at least
    CHECK(w.b().auth_counters().tags_ok > 0u);
    CHECK_EQ(w.b().auth_counters().bad_tag, 0u);
    CHECK_EQ(w.b().auth_counters().replayed, 0u);
    CHECK_EQ(w.b().auth_counters().untagged, 0u);
    w.a().request_write(0x101, kSetpoint, Value::of_i32(42));
    w.c.advance_ms(50);
    CHECK_EQ(setpoint_of(w.b()), 42);
    Frame f;
    CHECK(parse(w.c.last_write.data(), w.c.last_write.size(), f, kMaxPayloadV2) == FrameError::Ok);
    CHECK(f.has_auth());  // and what went on the air carried a tag
}

TEST(auth, a_replayed_write_is_rejected_and_changes_nothing) {
    WriteCell w;
    w.a().request_write(0x101, kSetpoint, Value::of_i32(42));
    w.c.advance_ms(50);
    const std::vector<uint8_t> recorded = w.c.last_write;
    w.a().request_write(0x101, kSetpoint, Value::of_i32(7));  // the owner moves on
    w.c.advance_ms(50);
    CHECK_EQ(setpoint_of(w.b()), 7);
    const uint32_t served = w.b().ns_counters().writes_served;
    // The attacker replays the recorded "42" from A's own MAC.
    w.b().on_rx(w.c.nodes[0].mac, recorded.data(), recorded.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().auth_counters().replayed, 1u);
    CHECK_EQ(w.b().ns_counters().writes_served, served);
    CHECK_EQ(setpoint_of(w.b()), 7);
}

TEST(auth, a_tampered_or_untagged_write_is_rejected) {
    WriteCell w;
    w.a().request_write(0x101, kSetpoint, Value::of_i32(5));
    w.c.advance_ms(50);
    std::vector<uint8_t> bad = w.c.last_write;
    bad[kHeaderSize + 6] ^= 0x01;  // inside the payload
    // Bump seq past everything seen so the window says New and only the tag can refuse it.
    bad[8] = 0xF0;  // header seq, low byte first
    bad[9] = 0xEE;
    w.b().on_rx(w.c.nodes[0].mac, bad.data(), bad.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().auth_counters().bad_tag, 1u);
    CHECK_EQ(setpoint_of(w.b()), 5);

    // A plain, untagged WRITE claiming to be from A.
    WritePayload wp{};
    wp.path_hash = kSetpoint;
    const Value v = Value::of_i32(666);
    value_to_wire(v, wp.value_type, wp.value_len, wp.value_raw);
    EncodeSpec spec;
    spec.src = 0x100;
    spec.dst = 0x101;
    spec.opcode = kOpWrite;
    spec.seq = 60000;
    uint8_t buf[128];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&wp), static_cast<uint16_t>(sizeof(wp)), buf, sizeof(buf), n) ==
          FrameError::Ok);
    w.b().on_rx(w.c.nodes[0].mac, buf, n, w.c.now_us, -50);
    CHECK_EQ(w.b().auth_counters().untagged, 1u);
    CHECK_EQ(setpoint_of(w.b()), 5);
}

TEST(auth, a_beacon_cannot_announce_a_reboot_only_a_signed_hello_can) {
    WriteCell w;
    BeaconPayload bp{};
    bp.node_id = 0x100;
    bp.hb_seq = 5;
    bp.boot_epoch = 99;  // a reboot that never happened
    EncodeSpec spec;
    spec.src = 0x100;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpHeartbeat;
    spec.lclass = kClassL3;
    spec.priority = 1;
    uint8_t buf[64];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&bp), static_cast<uint16_t>(sizeof(bp)), buf, sizeof(buf), n) ==
          FrameError::Ok);
    const uint32_t reboots = w.b().counters().reboots_seen;
    w.b().on_rx(w.c.nodes[0].mac, buf, n, w.c.now_us, -50);
    CHECK_EQ(w.b().auth_counters().beacon_epoch_ignored, 1u);
    CHECK_EQ(w.b().counters().reboots_seen, reboots);
}

TEST(auth, a_write_replayed_long_after_it_was_recorded_is_rejected_by_its_tag) {
    WriteCell w;
    w.a().request_write(0x101, kSetpoint, Value::of_i32(42));
    w.c.advance_ms(50);
    const std::vector<uint8_t> recorded = w.c.last_write;
    w.a().request_write(0x101, kSetpoint, Value::of_i32(7));
    w.c.advance_ms(120000);  // two minutes of probes: far more than 64 frames from A to B
    const uint32_t served = w.b().ns_counters().writes_served;
    w.b().on_rx(w.c.nodes[0].mac, recorded.data(), recorded.size(), w.c.now_us, -50);
    // Below the window, so RFC 4303 reads it as the next subspace; its tag was made for the old one.
    CHECK_EQ(w.b().auth_counters().bad_tag, 1u);
    CHECK_EQ(w.b().ns_counters().writes_served, served);
    CHECK_EQ(setpoint_of(w.b()), 7);
}

// ---------------------------------------------------------------------------------------------
// Step 5: the signed, epoch-fenced SAFE_STATE, and its high-water surviving a power cycle
// ---------------------------------------------------------------------------------------------

namespace {

size_t count_events(const AuthNode& t, EventKind k, uint32_t a = 0xFFFFFFFFu) {
    size_t n = 0;
    for (const Event& e : t.events) {
        if (e.kind == k && (a == 0xFFFFFFFFu || e.detail_a == a)) ++n;
    }
    return n;
}

// Replace node i with a fresh incarnation: same identity and MAC, a new boot epoch, RAM state gone --
// what a power cycle leaves. `restore` hands it the table it persisted before.
void power_cycle(AuthCell& c, size_t i, uint32_t epoch, bool restore) {
    AuthNode& t = c.nodes[i];
    delete t.node;
    t.events.clear();
    NodeConfig cfg;
    cfg.node_id = static_cast<uint16_t>(0x100 + i);
    cfg.boot_epoch = epoch;
    std::memcpy(cfg.mac, t.mac, kMacLen);
    cfg.hello_interval_ms = 500;
    t.node = new Node(cfg, t.hal);
    t.node->set_trust(t.id.get(), true);
    if (restore && !c.persisted[i].empty()) {
        t.node->restore_safe_state_floors(c.persisted[i].data(), c.persisted[i].size());
    }
    t.node->start();
}

}  // namespace

TEST(auth, a_signed_safe_state_is_accepted_once_and_its_replay_is_refused) {
    WriteCell w;
    CHECK(w.a().send_safe_state(3) == 1u);
    Frame f;
    CHECK(parse(w.c.last_safe_state.data(), w.c.last_safe_state.size(), f, kMaxPayloadV2) == FrameError::Ok);
    CHECK_EQ(static_cast<size_t>(f.payload_len), kSafeStateSignedLen);  // 76 B, one ESP-NOW frame
    CHECK_EQ(w.b().safe_state_counters().accepted, 1u);
    CHECK_EQ(count_events(w.c.nodes[1], EventKind::SafeStateRx), static_cast<size_t>(1));
    CHECK(!w.c.persisted[1].empty());  // and the receiver asked for its high-water to be persisted
    const std::vector<uint8_t> recorded = w.c.last_safe_state;
    w.b().on_rx(w.c.nodes[0].mac, recorded.data(), recorded.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().safe_state_counters().replayed, 1u);
    CHECK_EQ(count_events(w.c.nodes[1], EventKind::SafeStateRx), static_cast<size_t>(1));
}

TEST(auth, a_genuine_safe_state_replayed_after_a_full_power_cycle_is_refused) {
    WriteCell w;
    CHECK(w.a().send_safe_state(3) == 1u);
    const std::vector<uint8_t> recorded = w.c.last_safe_state;  // a genuine frame, epoch 1
    // The whole mesh loses power. Both come back with new epochs; B restores what it persisted.
    power_cycle(w.c, 0, 2, true);
    power_cycle(w.c, 1, 2, true);
    w.c.advance_ms(2000);
    CHECK(w.b().peer_auth(peer_by_id(w.b(), 0x100)) != nullptr);
    w.b().on_rx(w.c.nodes[0].mac, recorded.data(), recorded.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().safe_state_counters().replayed, 1u);
    CHECK_EQ(w.b().safe_state_counters().accepted, 0u);
    CHECK_EQ(count_events(w.c.nodes[1], EventKind::SafeStateRx), static_cast<size_t>(0));
    // A genuine SAFE_STATE from the new incarnation still gets through.
    CHECK(w.a().send_safe_state(4) == 1u);
    CHECK_EQ(w.b().safe_state_counters().accepted, 1u);
}

TEST(auth, without_the_persisted_high_water_the_same_replay_would_succeed) {
    // The control for the test above: the fence is the persisted table, not the reboot itself.
    WriteCell w;
    CHECK(w.a().send_safe_state(3) == 1u);
    const std::vector<uint8_t> recorded = w.c.last_safe_state;
    power_cycle(w.c, 0, 2, false);
    power_cycle(w.c, 1, 2, false);  // B forgets
    w.c.advance_ms(2000);
    w.b().on_rx(w.c.nodes[0].mac, recorded.data(), recorded.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().safe_state_counters().accepted, 1u);
}

TEST(auth, an_unsigned_or_forged_safe_state_is_refused) {
    WriteCell w;
    // Unsigned, from a verified peer: refused under require.
    SafeStatePayload sp{};
    sp.counter = 50;
    sp.reason = 1;
    EncodeSpec spec;
    spec.src = 0x100;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpSafeState;
    spec.lclass = kClassL0;
    spec.priority = 31;
    uint8_t buf[128];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&sp), static_cast<uint16_t>(sizeof(sp)), buf, sizeof(buf), n) ==
          FrameError::Ok);
    w.b().on_rx(w.c.nodes[0].mac, buf, n, w.c.now_us, -50);
    CHECK_EQ(w.b().safe_state_counters().unsigned_refused, 1u);
    // Signed, but the signature is garbage.
    CHECK(w.a().send_safe_state(3) == 1u);
    std::vector<uint8_t> forged = w.c.last_safe_state;
    forged[kHeaderSize] = 9;                  // counter 9: above the high-water, so only the signature stops it
    forged[forged.size() - 1] ^= 0x55;
    w.c.advance_ms(200);                      // past the per-source rate limit
    w.b().on_rx(w.c.nodes[0].mac, forged.data(), forged.size(), w.c.now_us, -50);
    CHECK_EQ(w.b().safe_state_counters().bad_signature, 1u);
    CHECK_EQ(w.b().safe_state_counters().accepted, 1u);
}

// ---- M5.1 CR-1 under auth: a signed move is followed; a replayed or forged one is not ----

TEST(auth, a_signed_channel_move_is_followed_and_a_replay_of_it_is_not) {
    WriteCell w;
    CHECK(w.a().move_cell(6, 200));
    w.c.advance_ms(300);
    CHECK_EQ(static_cast<int>(w.b().channel()), 6);
    CHECK_EQ(w.b().channel_counters().moves_followed, 1u);
    CHECK_EQ(w.b().channel_counters().moves_refused, 0u);  // the two repeats are recognised, not refused
}

TEST(auth, an_unsigned_channel_move_is_refused_under_require) {
    WriteCell w;
    ChannelPayload cp{};
    cp.channel = 9;
    cp.reason = 1;
    cp.delay_ms = 100;
    cp.epoch = 1;
    cp.move_id = 1;
    EncodeSpec spec;
    spec.src = 0x100;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpChannel;
    uint8_t buf[64];
    size_t n = 0;
    CHECK(encode(spec, reinterpret_cast<const uint8_t*>(&cp), static_cast<uint16_t>(sizeof(cp)), buf, sizeof(buf), n) ==
          FrameError::Ok);
    w.b().on_rx(w.c.nodes[0].mac, buf, n, w.c.now_us, -50);
    w.c.advance_ms(200);
    CHECK_EQ(w.b().channel_counters().moves_refused, 1u);
    CHECK_EQ(static_cast<int>(w.b().channel()), 1);
}

// ---- M5.1 CR-2 / ADR-009 under authentication ----

TEST(auth, through_the_relay_admission_is_verified_end_to_end_and_a_write_lands) {
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    for (uint16_t i = 0; i < 3; ++i) {
        ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, static_cast<uint16_t>(0x100 + i),
                                                                   static_cast<uint8_t>(0x11 * (i + 1)))));
    }
    c.build(std::move(ids), true);
    c.deaf.push_back({0, 2});
    declare_setpoint(*c.nodes[2].node, 0x102);
    c.nodes[1].node->set_relay(true);
    c.start_all();
    c.advance_ms(5000);
    Node& a = *c.nodes[0].node;
    Node& g = *c.nodes[2].node;
    const PeerLink* ag = peer_by_id(a, 0x102);
    CHECK(ag != nullptr && a.peer_relayed(ag));
    // A verified G's HELLO -- whose signature covers G's MAC, carried by the relay -- and the two
    // derived the same key, which the relay does not have.
    const Node::PeerAuth* pa = a.peer_auth(ag);
    const Node::PeerAuth* pg = g.peer_auth(peer_by_id(g, 0x100));
    CHECK(pa != nullptr && pa->verified);
    CHECK(pg != nullptr && pg->verified);
    if (pa == nullptr || pg == nullptr) return;  // a failed CHECK does not stop the test; do not dereference
    CHECK(std::memcmp(pa->key, pg->key, 32) == 0);
    a.request_write(0x102, kSetpoint, Value::of_i32(77));
    c.advance_ms(100);
    CHECK_EQ(setpoint_of(g), 77);
    CHECK_EQ(g.auth_counters().bad_tag, 0u);
    CHECK(g.auth_counters().tags_ok > 0u);
}

TEST(auth, a_board_erased_and_re_enrolled_is_admitted_again_though_its_epoch_restarted) {
    // Found on the bench (M0-LOG session 27): board B's flash was erased for M6.1's step 0 and B was
    // re-enrolled. The erase took its boot-epoch counter too, so B came back at a LOWER epoch than A
    // and C remembered, under a new key and a newer certificate -- and they refused it as a ghost of
    // its old self, for as long as they stayed up, with every frame between them failing its tag.
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11, 1000)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22, 1000)));
    c.build(std::move(ids), true, {1, 50});
    c.start_all();
    c.advance_ms(1500);
    Node& a = *c.nodes[0].node;
    CHECK(c.nodes[1].node->send_safe_state(3) == 1u);
    CHECK_EQ(a.safe_state_counters().accepted, 1u);  // A now holds a floor for B at epoch 50
    const std::vector<uint8_t> old_hello = c.last_hello_from[1];
    const std::vector<uint8_t> old_safe_state = c.last_safe_state;

    // B: flash erased, a new key, enrolled again later, epoch counter back to 1.
    AuthNode& bn = c.nodes[1];
    delete bn.node;
    bn.id = std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x33, 2000));
    NodeConfig cfg;
    cfg.node_id = 0x101;
    cfg.boot_epoch = 1;
    std::memcpy(cfg.mac, bn.mac, kMacLen);
    cfg.hello_interval_ms = 500;
    bn.node = new Node(cfg, bn.hal);
    bn.node->set_trust(bn.id.get(), true);
    bn.node->start();
    c.advance_ms(3000);

    const PeerLink* pb = peer_by_id(a, 0x101);
    const Node::PeerAuth* pa = a.peer_auth(pb);
    CHECK(pa != nullptr && pa->verified);
    CHECK_EQ(pa->epoch, 1u);
    CHECK_EQ(pa->issued, 2000u);
    CHECK(pb != nullptr && pb->state == PeerState::Alive && pb->boot_epoch == 1u);
    // Tagged frames flow both ways under the new key.
    CHECK_EQ(a.auth_counters().bad_tag, 0u);
    CHECK_EQ(bn.node->auth_counters().bad_tag, 0u);
    CHECK(bn.node->auth_counters().tags_ok > 0u);
    // B's safety messages are heard again: the floor set under its old key restarts.
    CHECK(bn.node->send_safe_state(5) == 1u);
    CHECK_EQ(a.safe_state_counters().accepted, 2u);

    // The old enrolment's HELLO, replayed: a higher epoch, an older certificate. Refused, and the
    // new key stands.
    uint8_t key_now[32];
    std::memcpy(key_now, pa->key, 32);
    const uint32_t stale_before = a.auth_counters().stale_epoch;
    inject_hello(a, bn.mac, 0x101, old_hello.data(), old_hello.size(), c.now_us);
    CHECK_EQ(a.auth_counters().stale_epoch, stale_before + 1);
    CHECK_EQ(pa->issued, 2000u);
    CHECK(std::memcmp(pa->key, key_now, 32) == 0);
    // And its SAFE_STATE, replayed: it was signed by the old key, so it cannot pass.
    const uint32_t accepted = a.safe_state_counters().accepted;
    a.on_rx(bn.mac, old_safe_state.data(), old_safe_state.size(), c.now_us, -50);
    CHECK_EQ(a.safe_state_counters().accepted, accepted);
}

// ---------------------------------------------------------------------------------------------
// M8.1 bench (M0-LOG session 31): the extender rebooted onto the house mesh's other channel
// ---------------------------------------------------------------------------------------------

TEST(auth, a_channel_authority_that_reboots_onto_another_channel_is_found_and_readmitted) {
    // The bench: B (the extender, whose channel is its router's) rebooted and its station joined the
    // mesh's channel-11 access point; A and C, on 1, never readmitted it. Here node 0 is B.
    TestCa ca(1);
    AuthCell c;
    c.channels = true;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x101, 0x22)));
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x102, 0x33)));
    c.build(std::move(ids), true);
    c.nodes[0].node->set_channel_fixed(true);
    c.start_all();
    c.advance_ms(3000);
    CHECK_EQ(alive(*c.nodes[1].node), static_cast<size_t>(2));

    // B goes dark (its reboot), long enough for the others' one sweep to find nothing.
    c.nodes[0].muted = true;
    c.advance_ms(600 + 10 * 300 + 1000);
    // ... and comes back as a new incarnation, on another channel, owning it.
    c.nodes[0].muted = false;
    c.nodes[0].chan = 9;
    power_cycle(c, 0, 2, false);
    c.nodes[0].node->set_channel_fixed(true);
    c.nodes[0].node->set_channel_now(9);

    c.advance_ms(10 * 2000 + 11 * 300 + 5000);
    for (size_t i = 1; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(c.nodes[i].chan), 9);
        CHECK_EQ(alive(*c.nodes[i].node), static_cast<size_t>(2));
        const Node::PeerAuth* pa = c.nodes[i].node->peer_auth(peer_by_id(*c.nodes[i].node, 0x100));
        CHECK(pa != nullptr && pa->verified && pa->epoch == 2u);
    }
    CHECK_EQ(alive(*c.nodes[0].node), static_cast<size_t>(2));
}

TEST(auth, a_host_that_signs_on_the_cable_is_verified_so_it_can_be_relayed) {
    // M10: the PC's pot_hostnode is enrolled like a board and signs its HELLO. The board it is cabled
    // to trusts the cable either way, but verifies a signed HELLO -- only a verified member's HELLO is
    // relayed to the rest of the cell (ADR-009), and only a verified one gets a pair key.
    TestCa ca(1);
    AuthCell c;
    std::vector<std::unique_ptr<Identity>> ids;
    ids.push_back(std::make_unique<Identity>(enrolled_identity(ca, 0x100, 0x11)));
    c.build(std::move(ids), true);
    AuthNode& an = c.nodes[0];
    delete an.node;
    NodeConfig cfg;
    cfg.node_id = 0x100;
    cfg.boot_epoch = 1;
    std::memcpy(cfg.mac, an.mac, kMacLen);
    const uint8_t host_mac[6] = {0x02, 0, 0, 0, 0, 0xFE};
    cfg.has_trusted_mac = true;
    std::memcpy(cfg.trusted_mac, host_mac, 6);
    an.node = new Node(cfg, an.hal);
    an.node->set_trust(an.id.get(), true);
    an.node->start();
    Identity host = enrolled_identity(ca, 0x00FE, 0x44);
    uint8_t base[kHelloBaseLen], sh[kHelloSignedLen];
    base_hello(0x00FE, 1, base);
    CHECK(hello_sign(host, host_mac, base, sh));
    inject_hello(*an.node, host_mac, 0x00FE, sh, sizeof(sh), c.now_us);
    const PeerLink* p = peer_by_id(*an.node, 0x00FE);
    CHECK(p != nullptr);
    const Node::PeerAuth* pa = an.node->peer_auth(p);
    CHECK(pa != nullptr && pa->verified);
}

