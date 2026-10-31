#include "pot/node_port.hpp"

#include <cstdio>
#include <cstring>

#include "monocypher-ed25519.h"
#include "pot/trust.hpp"

namespace pot {

namespace {
int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool from_hex(const char* s, size_t n, uint8_t* out) {
    if (n % 2 != 0) return false;
    for (size_t i = 0; i < n / 2; ++i) {
        const int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}
void to_hex(const uint8_t* in, size_t n, char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 0x0F];
    }
}
}  // namespace

size_t node_port_challenge(const uint8_t nonce[kNodePortNonceLen], char* out, size_t cap) {
    static const char kHead[] = "potluck-node 1 ";
    const size_t n = sizeof(kHead) - 1 + 2 * kNodePortNonceLen + 1;
    if (cap < n + 1) return 0;
    std::memcpy(out, kHead, sizeof(kHead) - 1);
    to_hex(nonce, kNodePortNonceLen, out + sizeof(kHead) - 1);
    out[n - 1] = '\n';
    out[n] = '\0';
    return n;
}

NodePortLogin node_port_check(const char* line, size_t len, const uint8_t nonce[kNodePortNonceLen],
                              const uint8_t ca_pub[32]) {
    NodePortLogin r;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) --len;
    const char* sp = static_cast<const char*>(std::memchr(line, ' ', len));
    if (sp == nullptr) {
        r.why = "expected '<certificate hex> <signature hex>'";
        return r;
    }
    const size_t cert_hex = static_cast<size_t>(sp - line), sig_hex = len - cert_hex - 1;
    uint8_t cert[kNodeCertLen], sig[kEdSigLen];
    if (cert_hex != 2 * kNodeCertLen || sig_hex != 2 * kEdSigLen) {
        r.why = "a certificate is 112 bytes and a signature 64";
        return r;
    }
    if (!from_hex(line, cert_hex, cert) || !from_hex(sp + 1, sig_hex, sig)) {
        r.why = "not hex";
        return r;
    }
    NodeCert c{};
    const CertError e = node_cert_check(cert, sizeof(cert), ca_pub, c);
    if (e != CertError::Ok) {
        r.why = cert_error_name(e);
        return r;
    }
    if (c.node_id < kBridgedFirst || c.node_id > kBridgedLast) {
        r.why = "not a node the cell takes from the house network";
        return r;
    }
    uint8_t msg[sizeof(kLoginDomain) + kNodePortNonceLen + kNodeCertLen];
    std::memcpy(msg, kLoginDomain, sizeof(kLoginDomain));
    std::memcpy(msg + sizeof(kLoginDomain), nonce, kNodePortNonceLen);
    std::memcpy(msg + sizeof(kLoginDomain) + kNodePortNonceLen, cert, kNodeCertLen);
    if (crypto_ed25519_check(sig, c.node_pub, msg, sizeof(msg)) != 0) {
        r.why = "the signature does not verify with the certificate's key";
        return r;
    }
    r.node_id = c.node_id;
    return r;
}

size_t node_port_ok(uint16_t node_id, uint16_t board_id, const uint8_t board_mac[6], char* out, size_t cap) {
    char mac[13];
    to_hex(board_mac, 6, mac);
    mac[12] = '\0';
    const int n = std::snprintf(out, cap, "ok %04x %04x %s\n", static_cast<unsigned>(node_id),
                                static_cast<unsigned>(board_id), mac);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

}  // namespace pot
