#include "pot/trust.hpp"

#include <cstdio>
#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace pot {

namespace {

constexpr uint8_t kMagic[4] = {'P', 'N', 'C', '1'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRoleNode = 1;
constexpr char kDomain[] = "potluck-node-cert-v1";  // sizeof includes the NUL, as the host signs it

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Exactly 2*n hex digits into n bytes.
bool hex_exact(const char* s, size_t slen, uint8_t* out, size_t n) {
    if (slen != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        const int hi = hex_nibble(s[2 * i]);
        const int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

size_t hex_out(const uint8_t* b, size_t n, char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kHex[b[i] >> 4];
        out[2 * i + 1] = kHex[b[i] & 0x0F];
    }
    return 2 * n;
}

}  // namespace

const char* cert_error_name(CertError e) {
    switch (e) {
        case CertError::Ok: return "ok";
        case CertError::Length: return "bad_length";
        case CertError::Magic: return "bad_magic";
        case CertError::Version: return "bad_version";
        case CertError::Role: return "bad_role";
        case CertError::OtherCa: return "other_ca";
        case CertError::BadSignature: return "bad_signature";
        case CertError::WrongNode: return "wrong_node";
        case CertError::WrongKey: return "wrong_key";
        case CertError::NoKey: return "no_key";
    }
    return "unknown";
}

void ca_fingerprint(const uint8_t ca_pub[kEdPubLen], uint8_t fp[kCaFpLen]) {
    uint8_t h[64];
    crypto_sha512(h, ca_pub, kEdPubLen);
    std::memcpy(fp, h, kCaFpLen);
}

CertError node_cert_check(const uint8_t* raw, size_t len, const uint8_t ca_pub[kEdPubLen], NodeCert& out) {
    if (raw == nullptr || len != kNodeCertLen) return CertError::Length;
    if (std::memcmp(raw, kMagic, sizeof(kMagic)) != 0) return CertError::Magic;
    if (raw[4] != kVersion) return CertError::Version;
    if (raw[5] != kRoleNode) return CertError::Role;
    uint8_t fp[kCaFpLen];
    ca_fingerprint(ca_pub, fp);
    if (std::memcmp(raw + 8, fp, kCaFpLen) != 0) return CertError::OtherCa;
    uint8_t msg[sizeof(kDomain) + kNodeCertSignedLen];
    std::memcpy(msg, kDomain, sizeof(kDomain));
    std::memcpy(msg + sizeof(kDomain), raw, kNodeCertSignedLen);
    if (crypto_ed25519_check(raw + kNodeCertSignedLen, ca_pub, msg, sizeof(msg)) != 0) {
        return CertError::BadSignature;
    }
    out.node_id = rd16(raw + 6);
    std::memcpy(out.ca_fp, raw + 8, kCaFpLen);
    out.issued = rd32(raw + 12);
    std::memcpy(out.node_pub, raw + 16, kEdPubLen);
    return CertError::Ok;
}

void identity_from_seed(Identity& id, const uint8_t seed[kEdSeedLen]) {
    uint8_t s[kEdSeedLen];
    uint8_t sk[64];
    std::memcpy(s, seed, kEdSeedLen);
    std::memcpy(id.seed, seed, kEdSeedLen);
    crypto_ed25519_key_pair(sk, id.pub, s);  // wipes s
    crypto_wipe(sk, sizeof(sk));
    id.has_key = true;
    id.enrolled = false;  // a new key invalidates any certificate for the old one
}

CertError identity_install_cert(Identity& id, uint16_t node_id, const uint8_t ca_pub[kEdPubLen],
                                const uint8_t* cert, size_t len) {
    if (!id.has_key) return CertError::NoKey;
    NodeCert c{};
    const CertError e = node_cert_check(cert, len, ca_pub, c);
    if (e != CertError::Ok) return e;
    if (c.node_id != node_id) return CertError::WrongNode;
    if (std::memcmp(c.node_pub, id.pub, kEdPubLen) != 0) return CertError::WrongKey;
    std::memcpy(id.ca_pub, ca_pub, kEdPubLen);
    std::memcpy(id.cert, cert, kNodeCertLen);
    id.enrolled = true;
    return CertError::Ok;
}

EnrolRequest parse_enrol_line(const char* line, size_t len) {
    EnrolRequest r;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) --len;
    static const char kPrefix[] = "POT! ";
    const size_t plen = sizeof(kPrefix) - 1;
    if (len < plen || std::memcmp(line, kPrefix, plen) != 0) return r;  // None
    const char* p = line + plen;
    const char* end = line + len;
    // Split the rest on single spaces into at most three tokens.
    const char* tok[3] = {};
    size_t tlen[3] = {};
    size_t n = 0;
    while (p < end) {
        if (n == 3) {
            r.cmd = EnrolCmd::Bad;
            return r;
        }
        const char* q = p;
        while (q < end && *q != ' ') ++q;
        tok[n] = p;
        tlen[n] = static_cast<size_t>(q - p);
        ++n;
        p = (q < end) ? q + 1 : q;
    }
    if (n == 1 && tlen[0] == 2 && std::memcmp(tok[0], "id", 2) == 0) {
        r.cmd = EnrolCmd::Id;
        return r;
    }
    if (n == 3 && tlen[0] == 4 && std::memcmp(tok[0], "cert", 4) == 0 &&
        hex_exact(tok[1], tlen[1], r.ca_pub, kEdPubLen) && hex_exact(tok[2], tlen[2], r.cert, kNodeCertLen)) {
        r.cmd = EnrolCmd::Cert;
        return r;
    }
    r.cmd = EnrolCmd::Bad;
    return r;
}

size_t format_enrol_id(const Identity& id, uint16_t node_id, char* out, size_t cap) {
    // {"t":"enrol","node":65535,"pub":"<64>","enrolled":1,"ca_fp":"<8>"}
    char pub[2 * kEdPubLen + 1] = {};
    char fp[2 * kCaFpLen + 1] = {};
    if (id.has_key) hex_out(id.pub, kEdPubLen, pub);
    if (id.enrolled) {
        uint8_t f[kCaFpLen];
        ca_fingerprint(id.ca_pub, f);
        hex_out(f, kCaFpLen, fp);
    }
    const int n = std::snprintf(out, cap, "{\"t\":\"enrol\",\"node\":%u,\"pub\":\"%s\",\"enrolled\":%d,\"ca_fp\":\"%s\"}",
                                static_cast<unsigned>(node_id), pub, id.enrolled ? 1 : 0, fp);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

size_t format_enrol_result(const char* result, char* out, size_t cap) {
    const int n = std::snprintf(out, cap, "{\"t\":\"enrol\",\"result\":\"%s\"}", result);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

}  // namespace pot
