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
    void issue(uint16_t node_id, const uint8_t node_pub[32], uint8_t cert[kNodeCertLen]) const {
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
        std::memcpy(cert + 16, node_pub, 32);
        static const char kDomain[] = "potluck-node-cert-v1";
        uint8_t msg[sizeof(kDomain) + 48];
        std::memcpy(msg, kDomain, sizeof(kDomain));
        std::memcpy(msg + sizeof(kDomain), cert, 48);
        crypto_ed25519_sign(cert + 48, sk, msg, sizeof(msg));
    }
};

Identity enrolled_identity(const TestCa& ca, uint16_t node_id, uint8_t salt) {
    Identity id;
    uint8_t seed[32];
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(salt ^ (7 * i));
    identity_from_seed(id, seed);
    uint8_t cert[kNodeCertLen];
    ca.issue(node_id, id.pub, cert);
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
};

struct AuthCell {
    std::vector<AuthNode> nodes;
    uint32_t now_us = 0;
    std::vector<uint8_t> last_hello_from[8];  // the most recent HELLO payload each node broadcast

    // `ids[i]` null means: no identity at all (pre-M5 behaviour). `require` applies to every node
    // that has one.
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
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            if (i == from->index) continue;
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
