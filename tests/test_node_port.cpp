// M29: the node port's login on a board (pot/node_port.hpp) -- the hub's door (hub_bridge.py), byte for byte.

#include <cstring>
#include <string>

#include "monocypher-ed25519.h"
#include "pot/node_port.hpp"
#include "pot/trust.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

struct Key {
    uint8_t sk[64];
    uint8_t pub[32];
    explicit Key(uint8_t salt) {
        uint8_t seed[32];
        for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(salt + 7 * i);
        crypto_ed25519_key_pair(sk, pub, seed);
    }
};

void issue(const Key& ca, uint16_t node_id, const uint8_t node_pub[32], uint8_t cert[kNodeCertLen]) {
    std::memset(cert, 0, kNodeCertLen);
    std::memcpy(cert, "PNC1", 4);
    cert[4] = 1;
    cert[5] = kCertRoleNode;
    cert[6] = static_cast<uint8_t>(node_id);
    cert[7] = static_cast<uint8_t>(node_id >> 8);
    ca_fingerprint(ca.pub, cert + 8);
    std::memcpy(cert + 16, node_pub, 32);
    static const char kDomain[] = "potluck-node-cert-v1";
    uint8_t msg[sizeof(kDomain) + 48];
    std::memcpy(msg, kDomain, sizeof(kDomain));
    std::memcpy(msg + sizeof(kDomain), cert, 48);
    crypto_ed25519_sign(cert + 48, ca.sk, msg, sizeof(msg));
}

std::string hex(const uint8_t* p, size_t n) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += k[p[i] >> 4];
        s += k[p[i] & 15];
    }
    return s;
}

// What the tablet sends: its certificate and its key's signature over the domain, the nonce and the certificate.
std::string answer(const Key& node, const uint8_t cert[kNodeCertLen], const uint8_t nonce[kNodePortNonceLen]) {
    uint8_t msg[sizeof(kLoginDomain) + kNodePortNonceLen + kNodeCertLen];
    std::memcpy(msg, kLoginDomain, sizeof(kLoginDomain));
    std::memcpy(msg + sizeof(kLoginDomain), nonce, kNodePortNonceLen);
    std::memcpy(msg + sizeof(kLoginDomain) + kNodePortNonceLen, cert, kNodeCertLen);
    uint8_t sig[64];
    crypto_ed25519_sign(sig, node.sk, msg, sizeof(msg));
    return hex(cert, kNodeCertLen) + " " + hex(sig, 64) + "\n";
}

}  // namespace

TEST(node_port, the_hubs_lines_byte_for_byte) {
    uint8_t nonce[kNodePortNonceLen];
    for (size_t i = 0; i < sizeof(nonce); ++i) nonce[i] = static_cast<uint8_t>(i);
    char buf[128];
    CHECK_EQ(node_port_challenge(nonce, buf, sizeof(buf)), 15u + 64u + 1u);
    CHECK_EQ(std::string(buf), "potluck-node 1 " + hex(nonce, sizeof(nonce)) + "\n");
    CHECK_EQ(node_port_challenge(nonce, buf, 80), 0u);  // too small: nothing written past cap
    const uint8_t mac[6] = {0xb8, 0x1f, 0x3f, 0xda, 0x81, 0x60};
    CHECK(node_port_ok(0x00FD, 0x8160, mac, buf, sizeof(buf)) > 0u);
    CHECK_EQ(std::string(buf), std::string("ok 00fd 8160 b81f3fda8160\n"));
    CHECK_EQ(sizeof(kLoginDomain), 21u);  // b"potluck node port v1\x00"
}

TEST(node_port, the_tablet_logs_in_and_strangers_do_not) {
    const Key ca(1), tablet(2), other_ca(3);
    uint8_t nonce[kNodePortNonceLen];
    for (size_t i = 0; i < sizeof(nonce); ++i) nonce[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t cert[kNodeCertLen];
    issue(ca, 0x00FD, tablet.pub, cert);
    std::string a = answer(tablet, cert, nonce);
    NodePortLogin r = node_port_check(a.data(), a.size(), nonce, ca.pub);
    CHECK_EQ(r.node_id, 0x00FD);

    // Another nonce: a replayed answer.
    uint8_t nonce2[kNodePortNonceLen];
    std::memcpy(nonce2, nonce, sizeof(nonce));
    nonce2[0] ^= 1;
    CHECK_EQ(node_port_check(a.data(), a.size(), nonce2, ca.pub).node_id, 0);
    // Another cell's CA.
    CHECK_EQ(node_port_check(a.data(), a.size(), nonce, other_ca.pub).node_id, 0);
    // A board's id, and the PC's: not nodes from the house network.
    for (uint16_t id : {static_cast<uint16_t>(0x8160), static_cast<uint16_t>(0x00FE)}) {
        uint8_t c2[kNodeCertLen];
        issue(ca, id, tablet.pub, c2);
        const std::string b = answer(tablet, c2, nonce);
        r = node_port_check(b.data(), b.size(), nonce, ca.pub);
        CHECK_EQ(r.node_id, 0);
        CHECK(std::strlen(r.why) > 0u);
    }
    // A key that is not the certificate's.
    const Key thief(9);
    a = answer(thief, cert, nonce);
    CHECK_EQ(node_port_check(a.data(), a.size(), nonce, ca.pub).node_id, 0);
    // Garbage.
    for (const char* g : {"", "nonsense", "zz zz"}) {
        CHECK_EQ(node_port_check(g, std::strlen(g), nonce, ca.pub).node_id, 0);
    }
}
