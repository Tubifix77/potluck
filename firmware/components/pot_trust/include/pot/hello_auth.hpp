// Section 9.3, M5 step 3: the signed HELLO and the pairwise session key. Portable and host-tested.
//
// A signed HELLO is the ordinary 24-byte HelloPayload followed by the sender's 112-byte node
// certificate and a 64-byte Ed25519 signature: 200 bytes, inside ESP-NOW's 226-byte v1 payload. There
// is no room for a separate ephemeral key, so the session key comes from the identity keys themselves:
//
//   signature  Ed25519 by the node key over  "potluck-hello-v1\0" || sender MAC (6) || payload[0:136]
//   session    X25519(own identity, peer identity), each Ed25519 key mapped to its Montgomery form
//              (secret: SHA-512(seed)[0:32], exactly the scalar Ed25519 signs with; public: the
//              birational map), then
//              key = SHA-512("potluck-session-v1\0" || shared || id_lo || epoch_lo || id_hi || epoch_hi)[0:32]
//              with the two (node id, boot epoch) pairs ordered by node id, so both ends agree.
//
// Both boot epochs are in the key, so every reboot of either end rotates it -- the rotation section
// 9.3 asks for. What that does not give is forward secrecy: anyone who later steals a node's seed
// can recompute every past session key of that node. Section 9.5 already states "No forward secrecy
// across epoch boundaries in v1"; this construction is why, and it is a deliberate size trade, not an
// oversight. Using one key for both signing and Diffie-Hellman is the same pairing libsodium offers
// (crypto_sign_ed25519_sk_to_curve25519); the two uses are domain-separated by construction.
//
// The MAC is inside the signature so a recorded HELLO cannot be replayed from another radio to claim
// the sender's identity at a second address.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/trust.hpp"

namespace pot {

constexpr size_t kHelloBaseLen = 24;  // sizeof(HelloPayload)
constexpr size_t kHelloSignedLen = kHelloBaseLen + kNodeCertLen + kEdSigLen;  // 200
constexpr size_t kSessionKeyLen = 32;
constexpr size_t kHelloDigestLen = 16;

enum class HelloAuthError : uint8_t {
    Ok = 0,
    Unsigned,      // a plain 24-byte HELLO: an unenrolled node, or pre-M5 firmware
    BadLength,     // neither 24 nor 200 bytes
    Cert,          // the certificate failed; see cert_error
    IdMismatch,    // the certificate's node id is not the HELLO's (or the frame's) sender
    BadSignature,  // the HELLO itself is not signed by the certified key
    WeakKey,       // the key agreement produced all zeros: a small-order public key
    NotEnrolled,   // we have no CA to check against
    RateLimited,   // too many verifications this second; dropped unexamined
};
const char* hello_auth_error_name(HelloAuthError e);

// Append the certificate and signature to a 24-byte HELLO. `out` receives all 200 bytes. Requires an
// enrolled identity.
bool hello_sign(const Identity& id, const uint8_t mac[6], const uint8_t hello[kHelloBaseLen],
                uint8_t out[kHelloSignedLen]);

struct HelloAuth {
    HelloAuthError error = HelloAuthError::Unsigned;
    CertError cert_error = CertError::Ok;
    uint8_t peer_pub[kEdPubLen] = {};
};

// Check a received HELLO payload: the certificate under `ca_pub`, its node id equal to `sender_id`,
// and the signature over the sender's MAC and the payload.
HelloAuth hello_verify(const uint8_t ca_pub[kEdPubLen], const uint8_t mac[6], const uint8_t* payload,
                       size_t len, uint16_t sender_id);

// A short digest of exactly what was verified (MAC and all 200 bytes), so a node can recognise a
// repeat of a HELLO it has already checked and skip the two signature verifications.
void hello_digest(const uint8_t mac[6], const uint8_t* payload, size_t len, uint8_t out[kHelloDigestLen]);

// The pairwise key. False (and a zeroed key) for a small-order peer key.
bool session_key(const Identity& me, uint16_t my_id, uint32_t my_epoch, const uint8_t peer_pub[kEdPubLen],
                 uint16_t peer_id, uint32_t peer_epoch, uint8_t key[kSessionKeyLen]);

// ---- Step 5: the signed SAFE_STATE (ARCHITECTURE section 8.3) ----
//
// On the radio an enrolled node sends 76 bytes: the 8-byte SafeStatePayload (counter, reason,
// reserved), the sender's boot epoch, and an Ed25519 signature by its certified key over
// "potluck-safe-state-v1\0" || node_id || epoch || the 8 bytes. Receivers fence replays with the
// high-water (epoch, counter) per sender, persisted (section 8.3). CAN builds keep the unsigned
// 8-byte frame: a signature cannot ride in one classic CAN frame (M0-LOG session 21).
constexpr size_t kSafeStateBaseLen = 8;
constexpr size_t kSafeStateSignedLen = kSafeStateBaseLen + 4 + kEdSigLen;  // 76

bool safe_state_sign(const Identity& id, uint16_t node_id, uint32_t epoch, const uint8_t base[kSafeStateBaseLen],
                     uint8_t sig[kEdSigLen]);
bool safe_state_verify(const uint8_t pub[kEdPubLen], uint16_t node_id, uint32_t epoch,
                       const uint8_t base[kSafeStateBaseLen], const uint8_t sig[kEdSigLen]);

// M5.1 CR-1: the signed CHANNEL move -- the 12-byte payload, then Ed25519 over
// "potluck-channel-v1\0" || node_id || the 12 bytes.
constexpr size_t kChannelBaseLen = 12;
constexpr size_t kChannelSignedLen = kChannelBaseLen + kEdSigLen;  // 76
bool channel_sign(const Identity& id, uint16_t node_id, const uint8_t base[kChannelBaseLen], uint8_t sig[kEdSigLen]);
bool channel_verify(const uint8_t pub[kEdPubLen], uint16_t node_id, const uint8_t base[kChannelBaseLen],
                    const uint8_t sig[kEdSigLen]);

}  // namespace pot
