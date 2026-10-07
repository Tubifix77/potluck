#include "pot/frame_auth.hpp"

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace pot {

namespace {
constexpr char kTagDomain[] = "potluck-tag-v1";  // sizeof includes the NUL
}

void frame_tag(const uint8_t key[32], uint16_t seq_hi, const uint8_t* frame, size_t len,
               uint8_t tag[kFrameTagLen]) {
    crypto_sha512_hmac_ctx ctx;
    crypto_sha512_hmac_init(&ctx, key, 32);
    crypto_sha512_hmac_update(&ctx, reinterpret_cast<const uint8_t*>(kTagDomain), sizeof(kTagDomain));
    const uint8_t hi[2] = {static_cast<uint8_t>(seq_hi), static_cast<uint8_t>(seq_hi >> 8)};
    crypto_sha512_hmac_update(&ctx, hi, sizeof(hi));
    crypto_sha512_hmac_update(&ctx, frame, len);
    uint8_t mac[64];
    crypto_sha512_hmac_final(&ctx, mac);
    for (size_t i = 0; i < kFrameTagLen; ++i) tag[i] = mac[i];
    crypto_wipe(mac, sizeof(mac));
}

bool frame_tag_ok(const uint8_t key[32], uint16_t seq_hi, const uint8_t* frame, size_t len,
                  const uint8_t tag[kFrameTagLen]) {
    uint8_t want[kFrameTagLen];
    frame_tag(key, seq_hi, frame, len, want);
    uint8_t diff = 0;
    for (size_t i = 0; i < kFrameTagLen; ++i) diff = static_cast<uint8_t>(diff | (want[i] ^ tag[i]));
    return diff == 0;
}

uint16_t replay_seq_hi(const ReplayWindow& w, uint16_t seq_lo) {
    if (!w.valid) return 0;  // a fresh key: both ends start the high bits at zero
    const uint16_t tl = static_cast<uint16_t>(w.top);
    const uint16_t th = static_cast<uint16_t>(w.top >> 16);
    const uint16_t bl = static_cast<uint16_t>(tl - (kReplayWindow - 1));  // mod 2^16
    if (tl >= kReplayWindow - 1) {
        return (seq_lo >= bl) ? th : static_cast<uint16_t>(th + 1);  // Case A
    }
    return (seq_lo >= bl) ? static_cast<uint16_t>(th - 1) : th;  // Case B
}

ReplayVerdict replay_check(const ReplayWindow& w, uint32_t ext_seq) {
    if (!w.valid || ext_seq > w.top) return ReplayVerdict::New;
    const uint32_t behind = w.top - ext_seq;
    if (behind >= kReplayWindow) return ReplayVerdict::TooOld;
    return ((w.bits >> behind) & 1u) != 0 ? ReplayVerdict::Duplicate : ReplayVerdict::New;
}

void replay_accept(ReplayWindow& w, uint32_t ext_seq) {
    if (!w.valid) {
        w.valid = true;
        w.top = ext_seq;
        w.bits = 1;
        return;
    }
    if (ext_seq > w.top) {
        const uint32_t shift = ext_seq - w.top;
        w.bits = (shift >= 64) ? 0 : (w.bits << shift);
        w.bits |= 1;
        w.top = ext_seq;
        return;
    }
    const uint32_t behind = w.top - ext_seq;
    if (behind < kReplayWindow) w.bits |= (uint64_t{1} << behind);
}

}  // namespace pot
