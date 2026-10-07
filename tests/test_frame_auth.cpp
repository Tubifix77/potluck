// Section 9.3/9.4, M5 step 4: the frame tag and the RFC 4303-style anti-replay window, on their own.
// The cell-level tests (a replayed WRITE rejected on the wire) are in test_auth.cpp.

#include <cstring>

#include "pot/frame_auth.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t ext(uint16_t hi, uint16_t lo) { return (static_cast<uint32_t>(hi) << 16) | lo; }

// Feed a low-order seq the way the node does: guess the high bits, check, accept.
bool feed(ReplayWindow& w, uint16_t lo, uint16_t* hi_out = nullptr) {
    const uint16_t hi = replay_seq_hi(w, lo);
    if (hi_out) *hi_out = hi;
    if (replay_check(w, ext(hi, lo)) != ReplayVerdict::New) return false;
    replay_accept(w, ext(hi, lo));
    return true;
}

}  // namespace

TEST(frame_auth, in_order_frames_pass_and_each_is_accepted_once) {
    ReplayWindow w;
    for (uint16_t s = 100; s < 300; ++s) CHECK(feed(w, s));
    CHECK(!feed(w, 299));  // the newest, again
    CHECK(!feed(w, 250));  // inside the window, already seen
}

TEST(frame_auth, out_of_order_inside_the_window_passes_and_older_than_it_does_not) {
    ReplayWindow w;
    CHECK(feed(w, 1000));
    CHECK(feed(w, 1005));
    CHECK(feed(w, 1002));  // late, inside the window, unseen: fine (ESP-NOW retries reorder)
    CHECK(!feed(w, 1002));
    for (uint16_t s = 1006; s < 1100; ++s) CHECK(feed(w, s));
    // Now more than 64 behind the top. RFC 4303 A2.2 reads a seq below the window's bottom as the
    // NEXT subspace, so the window alone says "new" -- and the tag, computed with the wrong high
    // bits, is what refuses it. On the node such a replay is therefore counted as bad_tag.
    CHECK_EQ(replay_seq_hi(w, 1001), static_cast<uint16_t>(1));
    CHECK(replay_check(w, ext(0, 1001)) == ReplayVerdict::TooOld);  // its true position is too old
}

TEST(frame_auth, the_high_bits_follow_a_wrap_of_the_16_bit_seq) {
    ReplayWindow w;
    uint16_t hi = 0;
    for (uint32_t s = 65500; s <= 65535; ++s) CHECK(feed(w, static_cast<uint16_t>(s)));
    CHECK(feed(w, 0, &hi));  // RFC 4303 A2.2 case A: below the window's bottom means the next subspace
    CHECK_EQ(hi, static_cast<uint16_t>(1));
    CHECK(feed(w, 1, &hi));
    CHECK_EQ(hi, static_cast<uint16_t>(1));
    // A late frame from just before the wrap: case B, previous subspace, still inside the window.
    CHECK(!feed(w, 65535));  // already seen
    ReplayWindow w2;
    CHECK(feed(w2, 65534));
    CHECK(feed(w2, 3));            // wrapped, a little loss
    CHECK(feed(w2, 65535, &hi));   // late arrival from before the wrap
    CHECK_EQ(hi, static_cast<uint16_t>(0));
}

TEST(frame_auth, a_frame_recorded_a_full_wrap_ago_fails_its_tag) {
    // The same 16-bit seq, 65,536 frames later: the window cannot tell them apart, the tag can,
    // because the implicit high bits are inside it.
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    uint8_t frame[40];
    for (int i = 0; i < 40; ++i) frame[i] = static_cast<uint8_t>(3 * i);
    uint8_t tag0[kFrameTagLen];
    frame_tag(key, 0, frame, sizeof(frame), tag0);
    CHECK(frame_tag_ok(key, 0, frame, sizeof(frame), tag0));
    CHECK(!frame_tag_ok(key, 1, frame, sizeof(frame), tag0));
}

TEST(frame_auth, the_tag_covers_every_byte_and_depends_on_the_key) {
    uint8_t key[32] = {9}, other[32] = {8};
    uint8_t frame[30];
    for (int i = 0; i < 30; ++i) frame[i] = static_cast<uint8_t>(i);
    uint8_t tag[kFrameTagLen];
    frame_tag(key, 7, frame, sizeof(frame), tag);
    for (size_t i = 0; i < sizeof(frame); ++i) {
        frame[i] ^= 0x01;
        CHECK(!frame_tag_ok(key, 7, frame, sizeof(frame), tag));
        frame[i] ^= 0x01;
    }
    CHECK(!frame_tag_ok(other, 7, frame, sizeof(frame), tag));
    CHECK(!frame_tag_ok(key, 7, frame, sizeof(frame) - 1, tag));
}
