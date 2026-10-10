// Section 9.3, M5: the node's identity, the node certificate, and enrolment. Portable and
// host-tested; trust_esp.cpp keeps the identity in NVS and draws the key from the hardware RNG.
//
// The node certificate (v1, 112 bytes, little-endian) is defined with its builder in
// host/potluck/potluck/enrol.py; the two are held together by a golden certificate that
// host/potluck/tests/test_enrol.py builds and tests/test_trust.cpp verifies.
//
//      off  len  field
//        0    4  magic      "PNC1"
//        4    1  version    1
//        5    1  role       1 = node
//        6    2  node_id
//        8    4  ca_fp      first 4 bytes of SHA-512(CA public key)
//       12    4  issued     Unix seconds; informational, nothing expires in v1
//       16   32  node_pub   Ed25519
//       48   64  ca_sig     Ed25519 by the CA over "potluck-node-cert-v1\0" || bytes[0:48]
//
// Ed25519, not P-256, because the board measured it 16x cheaper to verify (M0-LOG session 21).

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr size_t kEdPubLen = 32;
constexpr size_t kEdSeedLen = 32;
constexpr size_t kEdSigLen = 64;
constexpr size_t kCaFpLen = 4;
constexpr size_t kNodeCertLen = 112;
constexpr size_t kNodeCertSignedLen = 48;

struct NodeCert {
    uint16_t node_id;
    uint8_t ca_fp[kCaFpLen];
    uint32_t issued;
    uint8_t node_pub[kEdPubLen];
};

enum class CertError : uint8_t {
    Ok = 0,
    Length,
    Magic,
    Version,
    Role,
    OtherCa,       // signed for a different cluster (fingerprint)
    BadSignature,
    WrongNode,     // a valid certificate, for another node
    WrongKey,      // a valid certificate for this node id, binding a key this node does not hold
    NoKey,         // installing a certificate before the node has generated its key
};
const char* cert_error_name(CertError e);

void ca_fingerprint(const uint8_t ca_pub[kEdPubLen], uint8_t fp[kCaFpLen]);

// Check a certificate against a CA public key. On Ok, `out` says what it binds.
CertError node_cert_check(const uint8_t* raw, size_t len, const uint8_t ca_pub[kEdPubLen], NodeCert& out);

// The same 112-byte format certifies a deploy key (M5 step 6): role 2, node_id 0. Section 9.3's
// "Deploy key. Separate from the CA. Signs manifests." -- here it signs the compiled image itself.
constexpr uint8_t kCertRoleNode = 1;
constexpr uint8_t kCertRoleDeploy = 2;
CertError key_cert_check(const uint8_t* raw, size_t len, const uint8_t ca_pub[kEdPubLen], uint8_t role,
                         NodeCert& out);

// The deploy trailer: a deploy-key certificate, then the deploy key's Ed25519 signature over
// "potluck-image-v1\0" || SHA-512(image). Appended to the image on the wire; the slot stores the image alone.
constexpr size_t kImageTrailerLen = kNodeCertLen + kEdSigLen;  // 176
CertError image_trailer_check(const uint8_t ca_pub[kEdPubLen], const uint8_t* image, size_t image_len,
                              const uint8_t* trailer, size_t trailer_len);

// M11: the same trailer, for a firmware image -- the deploy key's signature over
// "potluck-firmware-v1\0" || counter (u32 LE) || length (u32 LE) || SHA-512(image). The digest is given,
// because a firmware image is ~1 MB and is hashed as it streams into flash; the counter is signed so a
// replayed older image cannot claim a newer number. A different domain from a package's, so neither
// signature can be passed off as the other.
constexpr size_t kFwSignedLen = 20 + 4 + 4 + 64;
void fw_signed_message(uint32_t counter, uint32_t image_len, const uint8_t sha512[64], uint8_t out[kFwSignedLen]);
CertError fw_trailer_check(const uint8_t ca_pub[kEdPubLen], uint32_t counter, uint32_t image_len,
                           const uint8_t sha512[64], const uint8_t* trailer, size_t trailer_len);

struct Identity {
    bool has_key = false;
    uint8_t seed[kEdSeedLen] = {};  // the private key; never leaves the node
    uint8_t pub[kEdPubLen] = {};
    bool enrolled = false;
    uint8_t ca_pub[kEdPubLen] = {};
    uint8_t cert[kNodeCertLen] = {};
};

// Derive the keypair from 32 random bytes. The node calls this once, with bytes from its RNG.
void identity_from_seed(Identity& id, const uint8_t seed[kEdSeedLen]);

// Accept a certificate only if it verifies under `ca_pub`, names `node_id`, and binds this node's own
// public key. On anything else the identity is unchanged.
CertError identity_install_cert(Identity& id, uint16_t node_id, const uint8_t ca_pub[kEdPubLen],
                                const uint8_t* cert, size_t len);

// The console's enrolment commands. One line each:
//   "POT! id"
//   "POT! cert <ca_pub: 64 hex> <cert: 224 hex>"
enum class EnrolCmd : uint8_t { None, Id, Cert, Bad };
struct EnrolRequest {
    EnrolCmd cmd = EnrolCmd::None;  // None: not a "POT! " line at all, leave it alone
    uint8_t ca_pub[kEdPubLen] = {};
    uint8_t cert[kNodeCertLen] = {};
};
EnrolRequest parse_enrol_line(const char* line, size_t len);

// The replies, as one JSON line each (no newline). Return the length written, 0 if it did not fit.
size_t format_enrol_id(const Identity& id, uint16_t node_id, char* out, size_t cap);
size_t format_enrol_result(const char* result, char* out, size_t cap);

}  // namespace pot
