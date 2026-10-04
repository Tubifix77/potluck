// Section 9.3, M5: the node certificate and enrolment, node side. The golden certificate is built by
// host/potluck/tests/test_enrol.py; if either side changes the format, one of the two suites fails.

#include <cstring>
#include <string>

#include "monocypher-ed25519.h"
#include "pot/trust.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

void hex(const char* s, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out[i] = static_cast<uint8_t>((v(s[2 * i]) << 4) | v(s[2 * i + 1]));
    }
}

const char* kCaPub = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
const char* kNodePub = "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
const char* kCert =
    "504e433101016873ed4242ea803bb16a29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc932296"
    "6dd75fc3b9d6983034e6c4a8fd64108939a15bffb2c2570cfdd4aae75d3cbc8cf41272e9d91bde77a3f3595a3c2b"
    "34fe30aac46ab9f37bf6569e71a1b8f7cb466e0c";

struct Golden {
    uint8_t ca[32];
    uint8_t pub[32];
    uint8_t cert[kNodeCertLen];
    uint8_t node_seed[32];
    Golden() {
        hex(kCaPub, ca, 32);
        hex(kNodePub, pub, 32);
        hex(kCert, cert, kNodeCertLen);
        for (uint8_t i = 0; i < 32; ++i) node_seed[i] = static_cast<uint8_t>(32 + i);  // as test_enrol.py
    }
};

}  // namespace

TEST(trust, the_host_tools_golden_certificate_verifies_here) {
    Golden g;
    NodeCert c{};
    CHECK(node_cert_check(g.cert, kNodeCertLen, g.ca, c) == CertError::Ok);
    CHECK_EQ(c.node_id, static_cast<uint16_t>(0x7368));
    CHECK_EQ(c.issued, 1790000000u);
    CHECK(std::memcmp(c.node_pub, g.pub, 32) == 0);
    uint8_t fp[4];
    ca_fingerprint(g.ca, fp);
    CHECK(std::memcmp(fp, c.ca_fp, 4) == 0);
}

TEST(trust, a_seed_gives_the_same_public_key_as_the_host_tool) {
    Golden g;
    Identity id;
    identity_from_seed(id, g.node_seed);
    CHECK(id.has_key);
    CHECK(!id.enrolled);
    CHECK(std::memcmp(id.pub, g.pub, 32) == 0);
}

TEST(trust, every_flipped_byte_is_refused) {
    Golden g;
    for (size_t i = 0; i < kNodeCertLen; ++i) {
        uint8_t bad[kNodeCertLen];
        std::memcpy(bad, g.cert, kNodeCertLen);
        bad[i] ^= 0x01;
        NodeCert c{};
        CHECK(node_cert_check(bad, kNodeCertLen, g.ca, c) != CertError::Ok);
    }
}

TEST(trust, each_refusal_is_named) {
    Golden g;
    NodeCert c{};
    CHECK(node_cert_check(g.cert, kNodeCertLen - 1, g.ca, c) == CertError::Length);
    uint8_t other_ca[32] = {};
    CHECK(node_cert_check(g.cert, kNodeCertLen, other_ca, c) == CertError::OtherCa);
    uint8_t bad[kNodeCertLen];
    std::memcpy(bad, g.cert, kNodeCertLen);
    bad[0] = 'X';
    CHECK(node_cert_check(bad, kNodeCertLen, g.ca, c) == CertError::Magic);
    std::memcpy(bad, g.cert, kNodeCertLen);
    bad[100] ^= 0x80;
    CHECK(node_cert_check(bad, kNodeCertLen, g.ca, c) == CertError::BadSignature);
    CHECK(std::string(cert_error_name(CertError::WrongNode)) == "wrong_node");
}

TEST(trust, a_node_accepts_only_its_own_certificate) {
    Golden g;
    Identity id;
    CHECK(identity_install_cert(id, 0x7368, g.ca, g.cert, kNodeCertLen) == CertError::NoKey);
    identity_from_seed(id, g.node_seed);
    // Right key, wrong node id: a certificate for 0x7368 does not enrol 0x8160.
    CHECK(identity_install_cert(id, 0x8160, g.ca, g.cert, kNodeCertLen) == CertError::WrongNode);
    CHECK(!id.enrolled);
    // Right node id, but this node holds a different key.
    Identity other;
    uint8_t seed2[32] = {7};
    identity_from_seed(other, seed2);
    CHECK(identity_install_cert(other, 0x7368, g.ca, g.cert, kNodeCertLen) == CertError::WrongKey);
    CHECK(!other.enrolled);
    // The real thing.
    CHECK(identity_install_cert(id, 0x7368, g.ca, g.cert, kNodeCertLen) == CertError::Ok);
    CHECK(id.enrolled);
    CHECK(std::memcmp(id.ca_pub, g.ca, 32) == 0);
    // A new key un-enrols: the old certificate binds a key the node no longer holds.
    identity_from_seed(id, seed2);
    CHECK(!id.enrolled);
}

TEST(trust, the_console_commands_parse_and_nothing_else_does) {
    const std::string cert_line = std::string("POT! cert ") + kCaPub + " " + kCert + "\r\n";
    EnrolRequest r = parse_enrol_line(cert_line.c_str(), cert_line.size());
    CHECK(r.cmd == EnrolCmd::Cert);
    Golden g;
    CHECK(std::memcmp(r.ca_pub, g.ca, 32) == 0);
    CHECK(std::memcmp(r.cert, g.cert, kNodeCertLen) == 0);

    CHECK(parse_enrol_line("POT! id\n", 8).cmd == EnrolCmd::Id);
    CHECK(parse_enrol_line("I (123) pot.m0: hello", 21).cmd == EnrolCmd::None);
    CHECK(parse_enrol_line("POT! id extra", 13).cmd == EnrolCmd::Bad);
    CHECK(parse_enrol_line("POT! cert 00 11", 15).cmd == EnrolCmd::Bad);  // wrong lengths
    const std::string odd = std::string("POT! cert ") + kCaPub + " " + std::string(kCert).substr(0, 222) + "zz";
    CHECK(parse_enrol_line(odd.c_str(), odd.size()).cmd == EnrolCmd::Bad);  // not hex
}

TEST(trust, the_id_reply_carries_the_key_and_the_cluster) {
    Golden g;
    Identity id;
    identity_from_seed(id, g.node_seed);
    char buf[200];
    size_t n = format_enrol_id(id, 0x7368, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(std::string(buf, n) == std::string("{\"t\":\"enrol\",\"node\":29544,\"pub\":\"") + kNodePub +
                                     "\",\"enrolled\":0,\"ca_fp\":\"\"}");
    CHECK(identity_install_cert(id, 0x7368, g.ca, g.cert, kNodeCertLen) == CertError::Ok);
    n = format_enrol_id(id, 0x7368, buf, sizeof(buf));
    CHECK(std::string(buf, n).find("\"enrolled\":1,\"ca_fp\":\"ed4242ea\"") != std::string::npos);
    CHECK(format_enrol_id(id, 0x7368, buf, 20) == 0);  // never a truncated line
}
