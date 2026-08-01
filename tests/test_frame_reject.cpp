// Rejection tests: every FrameError must be reachable by a frame that deserves it, and no frame
// that deserves rejection may be accepted.
//
// The interesting cases here are not the obviously-corrupt ones. They are the frames that are
// *almost* valid — a truncated v2 frame, a fragment whose arithmetic overflows, a reserved bit
// someone set — because those are the ones a parser accepts by accident.

#include <cstring>
#include <vector>

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

// A valid unfragmented HEARTBEAT with an n-byte payload, as raw bytes to be corrupted.
std::vector<uint8_t> valid_frame(uint16_t payload_len = 8, uint8_t opcode = kOpErr) {
    EncodeSpec spec;
    spec.src = 0x0111;
    spec.dst = 0x0222;
    spec.opcode = opcode;
    spec.lclass = kClassL3;
    spec.priority = 3;
    spec.seq = 0x0333;
    spec.msg_id = 0x0444;

    std::vector<uint8_t> payload(payload_len);
    for (uint16_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> buf(kHeaderSize + payload_len);
    size_t written = 0;
    encode(spec, payload.empty() ? nullptr : payload.data(), payload_len, buf.data(), buf.size(),
           written);
    buf.resize(written);
    return buf;
}

FrameError parse_of(const std::vector<uint8_t>& b, uint16_t max_payload = kMaxPayloadV2) {
    Frame f;
    return parse(b.data(), b.size(), f, max_payload);
}

}  // namespace

TEST(reject, baseline_frame_is_accepted) {
    // Guards the rest of the file: if this fails, every "rejected" below is meaningless.
    CHECK_EQ(static_cast<int>(parse_of(valid_frame())), static_cast<int>(FrameError::Ok));
}

TEST(reject, bad_magic) {
    for (int b = 0; b < 256; ++b) {
        if (b == kMagic) continue;
        auto f = valid_frame();
        f[0] = static_cast<uint8_t>(b);
        CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::BadMagic));
    }
}

TEST(reject, bad_version) {
    // Any version but 1. Version 0 and 2 are the ones that would actually happen.
    for (int v = 0; v < 16; ++v) {
        if (v == kVersion) continue;
        auto f = valid_frame();
        f[1] = make_ver_flags(static_cast<uint8_t>(v), 0);
        CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::BadVersion));
    }
}

TEST(reject, reserved_flag_bit) {
    // §5.1: ver_flags[0] must be 0. Rejecting it is what makes the bit available later.
    auto f = valid_frame();
    f[1] = make_ver_flags(kVersion, kFlagReserved);
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::ReservedFlagSet));
}

TEST(reject, latency_class_above_L4) {
    // §4 defines L0..L4. The field holds 0..7; 5, 6 and 7 are not classes.
    for (uint8_t cls = 5; cls < 8; ++cls) {
        auto f = valid_frame();
        f[7] = make_lclass_pri(cls, 0);
        CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::BadClass));
    }
}

TEST(reject, truncated_below_the_header) {
    const auto full = valid_frame();
    for (size_t n = 0; n < kHeaderSize; ++n) {
        std::vector<uint8_t> f(full.begin(), full.begin() + n);
        Frame out;
        CHECK_EQ(static_cast<int>(parse(f.empty() ? nullptr : f.data(), f.size(), out)),
                 static_cast<int>(FrameError::TooShort));
    }
}

TEST(reject, truncated_within_the_payload) {
    // The §5.3 hazard, concretely: a receiver hands us fewer payload bytes than total_len claims.
    // This is what a v1.0 device does to an over-long v2.0 frame, and the whole reason §5.4 makes
    // total_len authoritative. Parsing such a frame would mean acting on a message that was
    // silently cut in half.
    const auto full = valid_frame(32);
    for (size_t n = kHeaderSize; n < full.size(); ++n) {
        std::vector<uint8_t> f(full.begin(), full.begin() + n);
        CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::LengthMismatch));
    }
}

TEST(reject, v2_frame_truncated_to_250_bytes_by_a_v1_receiver) {
    // The exact §5.3 scenario, at the exact boundary the documentation names.
    EncodeSpec spec;
    spec.src = 1;
    spec.dst = 2;
    spec.opcode = kOpErr;
    std::vector<uint8_t> payload(1000, 0x5A);
    std::vector<uint8_t> buf(kHeaderSize + payload.size());
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), static_cast<uint16_t>(payload.size()),
                                     buf.data(), buf.size(), written)),
             static_cast<int>(FrameError::Ok));

    buf.resize(250);  // what the v1.0 receiver delivers
    CHECK_EQ(static_cast<int>(parse_of(buf)), static_cast<int>(FrameError::LengthMismatch));
}

TEST(reject, unfragmented_frame_with_nonzero_frag_off) {
    auto f = valid_frame();
    f[12] = 4;  // frag_off = 4 without the FRAG flag
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::LengthMismatch));
}

TEST(reject, unfragmented_frame_whose_total_len_disagrees) {
    auto f = valid_frame(8);
    f[14] = 9;  // total_len = 9, but 8 payload bytes arrived
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::LengthMismatch));
    f[14] = 7;
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::LengthMismatch));
}

TEST(reject, fragment_running_past_total_len) {
    EncodeSpec spec;
    spec.opcode = kOpErr;
    spec.frag = true;
    spec.frag_off = 95;
    spec.total_len = 100;
    std::vector<uint8_t> payload(8, 0x11);
    uint8_t buf[64] = {};
    size_t written = 0;
    // The encoder refuses to build it: 95 + 8 = 103 > 100.
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), 8, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::LengthMismatch));

    // The last legal fragment ends exactly on total_len: 92 + 8 == 100.
    spec.frag_off = 92;
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), 8, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::Ok));

    // And the parser refuses to accept one built by something less careful.
    spec.frag_off = 0;
    spec.total_len = 100;
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), 8, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::Ok));
    buf[12] = 95;  // rewrite frag_off to 95, so 95 + 8 > 100
    std::vector<uint8_t> v(buf, buf + written);
    CHECK_EQ(static_cast<int>(parse_of(v)), static_cast<int>(FrameError::LengthMismatch));
}

TEST(reject, fragment_carrying_no_payload) {
    EncodeSpec spec;
    spec.opcode = kOpErr;
    spec.frag = false;
    uint8_t buf[32] = {};
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, nullptr, 0, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::Ok));
    buf[1] = make_ver_flags(kVersion, kFlagFrag);
    buf[14] = 10;  // total_len = 10 with zero payload bytes
    std::vector<uint8_t> v(buf, buf + written);
    CHECK_EQ(static_cast<int>(parse_of(v)), static_cast<int>(FrameError::LengthMismatch));
}

TEST(reject, frag_flag_on_opcodes_5_4_forbids) {
    // §5.4: "No fragmentation for SAFE_STATE, HEARTBEAT, or HELLO."
    const uint8_t forbidden[] = {kOpSafeState, kOpHeartbeat, kOpHello};
    for (uint8_t op : forbidden) {
        CHECK(opcode_forbids_frag(op));

        auto f = valid_frame(8, op);
        f[1] = make_ver_flags(kVersion, kFlagFrag);
        f[12] = 0;
        f[14] = 64;  // a plausible total_len so the length check does not fire first
        CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::FragNotAllowed));

        // The encoder refuses too, so we never emit one.
        EncodeSpec spec;
        spec.opcode = op;
        spec.frag = true;
        spec.frag_off = 0;
        spec.total_len = 64;
        uint8_t buf[64] = {};
        size_t written = 0;
        const uint8_t payload[8] = {};
        CHECK_EQ(static_cast<int>(encode(spec, payload, 8, buf, sizeof(buf), written)),
                 static_cast<int>(FrameError::FragNotAllowed));
    }

    // HELLO_ACK, BYE and ERR are not on §5.4's list and must remain fragmentable.
    const uint8_t allowed[] = {kOpHelloAck, kOpBye, kOpErr};
    for (uint8_t op : allowed) {
        CHECK(!opcode_forbids_frag(op));
    }
}

TEST(reject, auth_flag_without_room_for_the_tag) {
    auto f = valid_frame(0);  // 16 bytes total
    f[1] = make_ver_flags(kVersion, kFlagAuth);
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::MissingAuthTag));

    // Seven of the eight tag bytes present is still not enough.
    for (int i = 0; i < 7; ++i) {
        f.push_back(0);
    }
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::MissingAuthTag));

    f.push_back(0);  // the eighth
    CHECK_EQ(static_cast<int>(parse_of(f)), static_cast<int>(FrameError::Ok));
}

TEST(reject, payload_over_the_pinned_profile_cap) {
    // A v1-pinned peer (§5.3) must not be allowed to hand us a 1446-byte payload.
    EncodeSpec spec;
    spec.opcode = kOpErr;
    std::vector<uint8_t> payload(300, 0x77);
    std::vector<uint8_t> buf(kHeaderSize + payload.size());
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), 300, buf.data(), buf.size(), written)),
             static_cast<int>(FrameError::Ok));
    buf.resize(written);

    CHECK_EQ(static_cast<int>(parse_of(buf, kMaxPayloadV2)), static_cast<int>(FrameError::Ok));
    CHECK_EQ(static_cast<int>(parse_of(buf, kMaxPayloadV1)),
             static_cast<int>(FrameError::PayloadTooLong));

    // Exactly at the cap is fine; one over is not.
    std::vector<uint8_t> at_cap(kMaxPayloadV1, 0x33);
    std::vector<uint8_t> b2(kHeaderSize + at_cap.size());
    CHECK_EQ(static_cast<int>(encode(spec, at_cap.data(), kMaxPayloadV1, b2.data(), b2.size(),
                                     written)),
             static_cast<int>(FrameError::Ok));
    b2.resize(written);
    CHECK_EQ(static_cast<int>(parse_of(b2, kMaxPayloadV1)), static_cast<int>(FrameError::Ok));
}

TEST(reject, encode_argument_validation) {
    uint8_t buf[64] = {};
    size_t written = 0;
    const uint8_t payload[4] = {1, 2, 3, 4};

    EncodeSpec bad_class;
    bad_class.opcode = kOpErr;
    bad_class.lclass = 5;
    CHECK_EQ(static_cast<int>(encode(bad_class, payload, 4, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::BadArgument));

    EncodeSpec bad_pri;
    bad_pri.opcode = kOpErr;
    bad_pri.priority = 32;
    CHECK_EQ(static_cast<int>(encode(bad_pri, payload, 4, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::BadArgument));

    EncodeSpec ok;
    ok.opcode = kOpErr;
    CHECK_EQ(static_cast<int>(encode(ok, nullptr, 4, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::BadArgument));
    CHECK_EQ(static_cast<int>(encode(ok, payload, 4, nullptr, sizeof(buf), written)),
             static_cast<int>(FrameError::BadArgument));

    // Buffer exactly one byte too small.
    CHECK_EQ(static_cast<int>(encode(ok, payload, 4, buf, kHeaderSize + 3, written)),
             static_cast<int>(FrameError::BufferTooSmall));
    CHECK_EQ(static_cast<int>(encode(ok, payload, 4, buf, kHeaderSize + 4, written)),
             static_cast<int>(FrameError::Ok));

    // A failed encode must leave `written` at zero rather than at a stale value.
    CHECK_EQ(static_cast<int>(encode(ok, payload, 4, buf, 2, written)),
             static_cast<int>(FrameError::BufferTooSmall));
    CHECK_EQ(written, static_cast<size_t>(0));
}

TEST(reject, null_buffer_is_too_short_not_a_crash) {
    Frame f;
    CHECK_EQ(static_cast<int>(parse(nullptr, 0, f)), static_cast<int>(FrameError::TooShort));
    CHECK_EQ(static_cast<int>(parse(nullptr, 64, f)), static_cast<int>(FrameError::TooShort));
}

TEST(reject, every_frame_error_has_a_distinct_name) {
    const FrameError all[] = {
        FrameError::Ok,           FrameError::TooShort,       FrameError::BadMagic,
        FrameError::BadVersion,   FrameError::ReservedFlagSet, FrameError::BadClass,
        FrameError::MissingAuthTag, FrameError::LengthMismatch, FrameError::FragNotAllowed,
        FrameError::PayloadTooLong, FrameError::BufferTooSmall, FrameError::BadArgument,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        CHECK_STR_EQ(frame_error_str(all[i]) != nullptr ? frame_error_str(all[i]) : "",
                     frame_error_str(all[i]));
        CHECK(std::strcmp(frame_error_str(all[i]), "unknown") != 0);
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            CHECK(std::strcmp(frame_error_str(all[i]), frame_error_str(all[j])) != 0);
        }
    }
}
