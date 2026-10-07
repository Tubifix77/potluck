// Section 9.3/9.4, M5 step 4: the frame tag and the anti-replay window. Portable and host-tested.
//
// THE TAG. Every unicast frame between two verified peers carries an 8-byte `auth_tag`:
//
//     tag = HMAC-SHA-512(session_key, "potluck-tag-v1\0" || seq_hi (2, LE) || frame[0 : 16 + payload_len])[0:8]
//
// over the header and payload exactly as sent, so it is end-to-end: it covers src, dst, opcode, seq
// and the AUTH flag itself, and NOT the sender's MAC, so a future single-hop relay (ARCHITECTURE
// section 13, M5.1) can forward a frame byte for byte and it still verifies. HMAC-SHA-512 rather than
// section 9.3's HMAC-SHA256 because Monocypher, the node's crypto library, has SHA-512 and no
// SHA-256. 64 bits is below RFC 2104 section 5's recommended 80; the 8 bytes were reserved in the v1
// frame from M0 and section 9.5 states the limitation (ledger, 2026-10-07).
//
// THE REPLAY WINDOW. The header's seq is 16 bits per (src, dst), which wraps every 65,536 frames --
// about 1.4 h at a bench link's ~13 frames/s. So, exactly as IPsec's extended sequence numbers do
// (RFC 4303 section 2.2.1 and Appendix A2), both ends keep the high 16 bits (`seq_hi`) implicitly:
// they are never sent, but they are inside the tag. A frame recorded before a wrap therefore fails
// its tag after it. Within one epoch of the low 16 bits a 64-frame sliding window, RFC 4303 section
// 3.4.3's preferred size, rejects duplicates and anything older than the window. The window moves
// only after the tag verifies (section 3.4.3: "The receive window is updated only if the integrity
// verification succeeds").
//
// Both are reset whenever the session key changes, which is whenever either end reboots (the key
// covers both boot epochs), so a frame from a previous incarnation fails on its key before any
// window is consulted.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr size_t kFrameTagLen = 8;
constexpr uint32_t kReplayWindow = 64;

void frame_tag(const uint8_t key[32], uint16_t seq_hi, const uint8_t* frame, size_t len,
               uint8_t tag[kFrameTagLen]);
// Constant-time comparison against a recomputed tag.
bool frame_tag_ok(const uint8_t key[32], uint16_t seq_hi, const uint8_t* frame, size_t len,
                  const uint8_t tag[kFrameTagLen]);

struct ReplayWindow {
    bool valid = false;  // nothing accepted yet under this key
    uint32_t top = 0;    // highest extended seq accepted: (seq_hi << 16) | seq
    uint64_t bits = 0;   // bit i set: top - i was accepted
};

// RFC 4303 Appendix A2.2: the high-order bits a received low-order seq belongs to.
uint16_t replay_seq_hi(const ReplayWindow& w, uint16_t seq_lo);

enum class ReplayVerdict : uint8_t { New, Duplicate, TooOld };
ReplayVerdict replay_check(const ReplayWindow& w, uint32_t ext_seq);
// Record an accepted frame. Call only after its tag verified.
void replay_accept(ReplayWindow& w, uint32_t ext_seq);

}  // namespace pot
