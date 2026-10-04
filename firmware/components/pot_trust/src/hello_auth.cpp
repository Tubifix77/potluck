#include "pot/hello_auth.hpp"

#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace pot {

namespace {

constexpr char kHelloDomain[] = "potluck-hello-v1";      // sizeof includes the NUL
constexpr char kSessionDomain[] = "potluck-session-v1";  // likewise
constexpr size_t kSignedPart = kHelloBaseLen + kNodeCertLen;  // 136

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// domain || mac || payload[0:136]
size_t hello_message(const uint8_t mac[6], const uint8_t* signed_part, uint8_t* msg) {
    std::memcpy(msg, kHelloDomain, sizeof(kHelloDomain));
    std::memcpy(msg + sizeof(kHelloDomain), mac, 6);
    std::memcpy(msg + sizeof(kHelloDomain) + 6, signed_part, kSignedPart);
    return sizeof(kHelloDomain) + 6 + kSignedPart;
}

}  // namespace

const char* hello_auth_error_name(HelloAuthError e) {
    switch (e) {
        case HelloAuthError::Ok: return "ok";
        case HelloAuthError::Unsigned: return "unsigned";
        case HelloAuthError::BadLength: return "bad_length";
        case HelloAuthError::Cert: return "bad_cert";
        case HelloAuthError::IdMismatch: return "id_mismatch";
        case HelloAuthError::BadSignature: return "bad_signature";
        case HelloAuthError::WeakKey: return "weak_key";
        case HelloAuthError::NotEnrolled: return "not_enrolled";
        case HelloAuthError::RateLimited: return "rate_limited";
    }
    return "unknown";
}

bool hello_sign(const Identity& id, const uint8_t mac[6], const uint8_t hello[kHelloBaseLen],
                uint8_t out[kHelloSignedLen]) {
    if (!id.has_key || !id.enrolled) return false;
    std::memcpy(out, hello, kHelloBaseLen);
    std::memcpy(out + kHelloBaseLen, id.cert, kNodeCertLen);
    uint8_t msg[sizeof(kHelloDomain) + 6 + kSignedPart];
    const size_t n = hello_message(mac, out, msg);
    uint8_t sk[64];
    uint8_t pub[kEdPubLen];
    uint8_t seed[kEdSeedLen];
    std::memcpy(seed, id.seed, kEdSeedLen);
    crypto_ed25519_key_pair(sk, pub, seed);  // wipes seed
    crypto_ed25519_sign(out + kSignedPart, sk, msg, n);
    crypto_wipe(sk, sizeof(sk));
    return true;
}

HelloAuth hello_verify(const uint8_t ca_pub[kEdPubLen], const uint8_t mac[6], const uint8_t* payload,
                       size_t len, uint16_t sender_id) {
    HelloAuth r;
    if (len == kHelloBaseLen) {
        r.error = HelloAuthError::Unsigned;
        return r;
    }
    if (len != kHelloSignedLen) {
        r.error = HelloAuthError::BadLength;
        return r;
    }
    NodeCert c{};
    r.cert_error = node_cert_check(payload + kHelloBaseLen, kNodeCertLen, ca_pub, c);
    if (r.cert_error != CertError::Ok) {
        r.error = HelloAuthError::Cert;
        return r;
    }
    const uint16_t hello_id = static_cast<uint16_t>(payload[8] | (payload[9] << 8));  // HelloPayload::node_id
    if (c.node_id != sender_id || hello_id != sender_id) {
        r.error = HelloAuthError::IdMismatch;
        return r;
    }
    uint8_t msg[sizeof(kHelloDomain) + 6 + kSignedPart];
    const size_t n = hello_message(mac, payload, msg);
    if (crypto_ed25519_check(payload + kSignedPart, c.node_pub, msg, n) != 0) {
        r.error = HelloAuthError::BadSignature;
        return r;
    }
    std::memcpy(r.peer_pub, c.node_pub, kEdPubLen);
    r.error = HelloAuthError::Ok;
    return r;
}

void hello_digest(const uint8_t mac[6], const uint8_t* payload, size_t len, uint8_t out[kHelloDigestLen]) {
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, mac, 6);
    crypto_sha512_update(&ctx, payload, len);
    uint8_t h[64];
    crypto_sha512_final(&ctx, h);
    std::memcpy(out, h, kHelloDigestLen);
}

bool session_key(const Identity& me, uint16_t my_id, uint32_t my_epoch, const uint8_t peer_pub[kEdPubLen],
                 uint16_t peer_id, uint32_t peer_epoch, uint8_t key[kSessionKeyLen]) {
    uint8_t h[64];
    crypto_sha512(h, me.seed, kEdSeedLen);  // h[0:32] is the Ed25519 signing scalar, before clamping
    uint8_t peer_x[32];
    crypto_eddsa_to_x25519(peer_x, peer_pub);
    uint8_t shared[32];
    crypto_x25519(shared, h, peer_x);  // crypto_x25519 clamps the scalar itself
    crypto_wipe(h, sizeof(h));
    uint8_t zero[32] = {};
    if (crypto_verify32(shared, zero) == 0) {
        std::memset(key, 0, kSessionKeyLen);
        return false;
    }
    const bool me_first = my_id < peer_id;
    uint8_t ids[12];
    put16(ids, me_first ? my_id : peer_id);
    put32(ids + 2, me_first ? my_epoch : peer_epoch);
    put16(ids + 6, me_first ? peer_id : my_id);
    put32(ids + 8, me_first ? peer_epoch : my_epoch);
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, reinterpret_cast<const uint8_t*>(kSessionDomain), sizeof(kSessionDomain));
    crypto_sha512_update(&ctx, shared, sizeof(shared));
    crypto_sha512_update(&ctx, ids, sizeof(ids));
    crypto_sha512_final(&ctx, h);
    std::memcpy(key, h, kSessionKeyLen);
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(h, sizeof(h));
    return true;
}

}  // namespace pot
