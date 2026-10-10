// M7: building guest blobs, author certificates and version-2 images for the tests, the way the host
// tools do (host/potluck/potluck/guest.py, deploy.py). Keys are derived from fixed seeds, the same ones
// the Python tests use: the CA from bytes 0..31, the guest author from 96..127.

#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "monocypher-ed25519.h"
#include "pot/deploy.hpp"
#include "pot/guest.hpp"
#include "pot/trust.hpp"

namespace guestfx {

using Bytes = std::vector<uint8_t>;

inline void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

struct Key {
    uint8_t sk[64];
    uint8_t pub[32];
    explicit Key(uint8_t first) {
        uint8_t seed[32];
        for (uint8_t i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(first + i);
        crypto_ed25519_key_pair(sk, pub, seed);  // wipes the seed it is given
    }
};

// A key certificate (pot_trust's format, as potluck.enrol.build_cert makes it).
inline Bytes make_cert(const Key& ca, uint8_t role, uint16_t id, const uint8_t subject_pub[32],
                       uint32_t issued = 1790000000u) {
    Bytes body = {'P', 'N', 'C', '1', 1, role};
    put16(body, id);
    uint8_t fp[4];
    pot::ca_fingerprint(ca.pub, fp);
    body.insert(body.end(), fp, fp + 4);
    put32(body, issued);
    body.insert(body.end(), subject_pub, subject_pub + 32);
    static const char kDomain[] = "potluck-node-cert-v1";  // and its NUL
    Bytes msg(kDomain, kDomain + sizeof(kDomain));
    msg.insert(msg.end(), body.begin(), body.end());
    uint8_t sig[64];
    crypto_ed25519_sign(sig, ca.sk, msg.data(), msg.size());
    body.insert(body.end(), sig, sig + 64);
    return body;
}

// The author's signature over "potluck-guest-v1\0" || SHA-512(module).
inline Bytes author_sig(const Key& author, const uint8_t* module, size_t len) {
    static const char kDomain[] = "potluck-guest-v1";
    uint8_t msg[17 + 64];
    std::memcpy(msg, kDomain, 17);
    crypto_sha512(msg + 17, module, len);
    uint8_t sig[64];
    crypto_ed25519_sign(sig, author.sk, msg, sizeof(msg));
    return Bytes(sig, sig + 64);
}

struct Out {
    uint32_t hash;
    pot::ValueType type;
};

struct Keys {
    Key ca{0};
    Key author{96};
    Bytes cert;  // the author's, role 3, author id 7
    Keys() : cert(make_cert(ca, pot::kCertRoleGuest, 7, author.pub)) {}
};

inline Bytes make_blob(const Keys& k, const uint8_t* module, size_t len, std::initializer_list<uint32_t> inputs,
                       std::initializer_list<Out> outputs, uint32_t fuel = 100000, uint8_t pages = 1) {
    Bytes b;
    put32(b, pot::kGuestMagic);
    b.push_back(pot::kGuestVersion);
    b.push_back(static_cast<uint8_t>(inputs.size()));
    b.push_back(static_cast<uint8_t>(outputs.size()));
    b.push_back(pages);
    put32(b, fuel);
    for (uint32_t h : inputs) put32(b, h);
    for (const Out& o : outputs) {
        put32(b, o.hash);
        b.push_back(static_cast<uint8_t>(o.type));
    }
    b.insert(b.end(), k.cert.begin(), k.cert.end());
    const Bytes sig = author_sig(k.author, module, len);
    b.insert(b.end(), sig.begin(), sig.end());
    b.insert(b.end(), module, module + len);
    return b;
}

// A guest actor's declaration: portable over `nodes` (gravity 1 each), then the guest index.
inline Bytes guest_decl(uint32_t key, uint16_t period_ms, std::initializer_list<uint16_t> nodes, uint8_t guest) {
    Bytes cfg;
    put32(cfg, key);
    put16(cfg, period_ms);
    cfg.push_back(static_cast<uint8_t>(nodes.size()));
    for (uint16_t n : nodes) {
        put16(cfg, n);
        cfg.push_back(1);
    }
    cfg.push_back(guest);
    Bytes d;
    put16(d, pot::kPortableNode);
    d.push_back(static_cast<uint8_t>(pot::ActorType::Guest));
    d.push_back(static_cast<uint8_t>(cfg.size()));
    d.insert(d.end(), cfg.begin(), cfg.end());
    return d;
}

// A version-2 image: the declarations (each already encoded), then the guest section.
inline Bytes make_image(std::initializer_list<Bytes> decls, std::initializer_list<Bytes> guests, uint32_t counter = 9) {
    Bytes body;
    for (const Bytes& d : decls) body.insert(body.end(), d.begin(), d.end());
    Bytes img;
    put32(img, pot::kImageMagic);
    img.push_back(pot::kImageVersionGuests);
    img.push_back(static_cast<uint8_t>(decls.size()));
    put16(img, 0);
    put32(img, counter);
    for (int i = 0; i < 8; ++i) img.push_back(static_cast<uint8_t>(0xA0 + i));
    put32(img, static_cast<uint32_t>(body.size()));
    img.insert(img.end(), body.begin(), body.end());
    img.push_back(static_cast<uint8_t>(guests.size()));
    for (const Bytes& g : guests) {
        put32(img, static_cast<uint32_t>(g.size()));
        img.insert(img.end(), g.begin(), g.end());
    }
    return img;
}

// ---- hand-assembled modules (the binary format of WebAssembly 1.0; every size here is under 128, so each
// LEB128 is one byte) ----

inline void section(Bytes& m, uint8_t id, const Bytes& content) {
    m.push_back(id);
    m.push_back(static_cast<uint8_t>(content.size()));
    m.insert(m.end(), content.begin(), content.end());
}
inline void name(Bytes& b, const char* s) {
    b.push_back(static_cast<uint8_t>(std::strlen(s)));
    b.insert(b.end(), s, s + std::strlen(s));
}

// "flaky": tick(now) publishes now to output 0 (i32), then traps if input 0 is non-zero -- so a trapped
// tick's publication can be seen to be discarded.
//   import potluck.input_i32 (i32)->i32 [0], potluck.publish_i32 (i32,i32)->() [1]; tick (i32)->() [2]
inline Bytes flaky_module() {
    Bytes m = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    section(m, 1, {0x03, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x00});
    Bytes imp = {0x02};
    name(imp, "potluck");
    name(imp, "input_i32");
    imp.insert(imp.end(), {0x00, 0x00});
    name(imp, "potluck");
    name(imp, "publish_i32");
    imp.insert(imp.end(), {0x00, 0x01});
    section(m, 2, imp);
    section(m, 3, {0x01, 0x02});
    Bytes exp = {0x01};
    name(exp, "tick");
    exp.insert(exp.end(), {0x00, 0x02});
    section(m, 7, exp);
    const Bytes body = {0x00,                    // no locals
                        0x41, 0x00, 0x20, 0x00,  // i32.const 0, local.get 0
                        0x10, 0x01,              // call publish_i32
                        0x41, 0x00, 0x10, 0x00,  // i32.const 0, call input_i32
                        0x04, 0x40, 0x00, 0x0b,  // if (no result) unreachable end
                        0x0b};
    Bytes code = {0x01, static_cast<uint8_t>(body.size())};
    code.insert(code.end(), body.begin(), body.end());
    section(m, 10, code);
    return m;
}

// "liar": imports potluck.input_f32 with the wrong type, (i32)->() -- refused when linked.
inline Bytes liar_module() {
    Bytes m = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    section(m, 1, {0x01, 0x60, 0x01, 0x7f, 0x00});
    Bytes imp = {0x01};
    name(imp, "potluck");
    name(imp, "input_f32");
    imp.insert(imp.end(), {0x00, 0x00});
    section(m, 2, imp);
    section(m, 3, {0x01, 0x00});
    Bytes exp = {0x01};
    name(exp, "tick");
    exp.insert(exp.end(), {0x00, 0x01});
    section(m, 7, exp);
    section(m, 10, {0x01, 0x02, 0x00, 0x0b});
    return m;
}

}  // namespace guestfx
