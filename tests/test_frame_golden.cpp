// Golden wire bytes for Potluck Frame v1 — ARCHITECTURE.md §5.1.
//
// These are the most important tests in the repository. Every other test asks whether the code is
// self-consistent; these ask whether the code agrees with the specification. The expected arrays
// were written by reading §5.1 and laying the bytes out by hand, not by printing what the encoder
// produced — an expectation generated from the implementation tests nothing.
//
// If one of these fails, the wire format changed. That is either a bug or a version bump, and
// §14 says the spec must outlive any one codebase, so it is never "just update the test".

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "pot/payloads.hpp"
#include "test_harness.hpp"

using namespace pot;

TEST(golden, heartbeat_frame_bytes) {
    // HEARTBEAT, src 0x0102, dst 0x0304, seq 0x0506, msg_id 0x0708, class L3, priority 5,
    // ACKREQ set, no AUTH, no FRAG, 4-byte payload.
    const uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};

    const uint8_t want[] = {
        0xD9,        // 0  magic
        0x14,        // 1  ver_flags: version 1 (0x10) | ACKREQ (0x04)
        0x02, 0x01,  // 2  src   = 0x0102 little-endian
        0x04, 0x03,  // 4  dst   = 0x0304
        0x03,        // 6  opcode = HEARTBEAT
        0x65,        // 7  lclass_pri = (L3 << 5) | 5
        0x06, 0x05,  // 8  seq   = 0x0506
        0x08, 0x07,  // 10 msg_id = 0x0708
        0x00, 0x00,  // 12 frag_off = 0
        0x04, 0x00,  // 14 total_len = 4
        0xAA, 0xBB, 0xCC, 0xDD,
    };

    EncodeSpec spec;
    spec.src = 0x0102;
    spec.dst = 0x0304;
    spec.opcode = kOpHeartbeat;
    spec.lclass = kClassL3;
    spec.priority = 5;
    spec.seq = 0x0506;
    spec.msg_id = 0x0708;
    spec.ack_req = true;

    uint8_t out[64] = {};
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, payload, sizeof(payload), out, sizeof(out), written)),
             static_cast<int>(FrameError::Ok));
    CHECK_BYTES_EQ(out, written, want, sizeof(want));
}

TEST(golden, auth_tag_is_appended_and_zeroed) {
    // §14: the auth_tag bytes are reserved from day one even though M5 is what fills them. The
    // encoder must emit eight zeroes, not eight bytes of whatever was on the stack.
    const uint8_t payload[2] = {0x11, 0x22};

    const uint8_t want[] = {
        0xD9,
        0x12,        // version 1 | AUTH (0x02)
        0x01, 0x00,  // src 1
        0x02, 0x00,  // dst 2
        0x01,        // HELLO
        0x60,        // L3, priority 0
        0x00, 0x00,  // seq 0
        0x00, 0x00,  // msg_id 0
        0x00, 0x00,  // frag_off 0
        0x02, 0x00,  // total_len 2
        0x11, 0x22,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // auth_tag, zeroed
    };

    EncodeSpec spec;
    spec.src = 1;
    spec.dst = 2;
    spec.opcode = kOpHello;
    spec.lclass = kClassL3;
    spec.auth = true;

    // Poison the buffer first: if the encoder ever forgets the memset, the tag would read 0xEE.
    uint8_t out[64];
    std::memset(out, 0xEE, sizeof(out));
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, payload, sizeof(payload), out, sizeof(out), written)),
             static_cast<int>(FrameError::Ok));
    CHECK_BYTES_EQ(out, written, want, sizeof(want));
}

TEST(golden, fragment_header_bytes) {
    // A fragment of a 100-byte message, 16 bytes in. ERR is used because §5.4 forbids fragmenting
    // HELLO, HEARTBEAT and SAFE_STATE.
    const uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};

    const uint8_t want[] = {
        0xD9,
        0x18,        // version 1 | FRAG (0x08)
        0xFF, 0xFF,  // src = 0xFFFF
        0x00, 0x00,  // dst = 0x0000 (unprovisioned)
        0x7F,        // ERR
        0x9F,        // lclass_pri = (L4 << 5) | 31
        0xFF, 0xFF,  // seq = 0xFFFF, the wrap boundary
        0x34, 0x12,  // msg_id = 0x1234
        0x10, 0x00,  // frag_off = 16
        0x64, 0x00,  // total_len = 100
        0x01, 0x02, 0x03, 0x04,
    };

    EncodeSpec spec;
    spec.src = 0xFFFF;
    spec.dst = 0x0000;
    spec.opcode = kOpErr;
    spec.lclass = kClassL4;
    spec.priority = 31;
    spec.seq = 0xFFFF;
    spec.msg_id = 0x1234;
    spec.frag = true;
    spec.frag_off = 16;
    spec.total_len = 100;

    uint8_t out[64] = {};
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, payload, sizeof(payload), out, sizeof(out), written)),
             static_cast<int>(FrameError::Ok));
    CHECK_BYTES_EQ(out, written, want, sizeof(want));
}

TEST(golden, empty_payload_frame_is_sixteen_bytes) {
    const uint8_t want[] = {
        0xD9, 0x10, 0x01, 0x00, 0xFF, 0xFF, 0x04, 0x60,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    EncodeSpec spec;
    spec.src = 1;
    spec.dst = kNodeBroadcast;
    spec.opcode = kOpBye;
    spec.lclass = kClassL3;
    spec.seq = 7;

    uint8_t out[32] = {};
    size_t written = 0;
    CHECK_EQ(static_cast<int>(encode(spec, nullptr, 0, out, sizeof(out), written)),
             static_cast<int>(FrameError::Ok));
    CHECK_EQ(written, static_cast<size_t>(16));
    CHECK_BYTES_EQ(out, written, want, sizeof(want));
}

// ------------------------------------------------------------------------------------------
// Payload layouts — payloads.hpp. Same principle: the byte arrays come from the offset tables
// in the comments, not from the structs.
// ------------------------------------------------------------------------------------------

TEST(golden, hello_payload_bytes) {
    const uint8_t want[24] = {
        0x44, 0x33, 0x22, 0x11,                          // 0  boot_epoch = 0x11223344
        0x00, 0x00, 0x00, 0x00,                          // 4  caps = 0 at M0
        0x02, 0x01,                                      // 8  node_id = 0x0102
        0x00, 0x00,                                      // 10 reserved0
        0x02,                                            // 12 espnow_version = 2
        0x0A,                                            // 13 hb_period_cs = 10 → 100 ms (§8.2)
        0x06,                                            // 14 hb_miss_limit = 6 (§8.2)
        0x01,                                            // 15 flags = want HELLO_ACK
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 16 pubkey_fp, zero until M5
    };

    HelloPayload h{};
    h.boot_epoch = 0x11223344;
    h.caps = 0;
    h.node_id = 0x0102;
    h.reserved0 = 0;
    h.espnow_version = 2;
    h.hb_period_cs = 10;
    h.hb_miss_limit = 6;
    h.flags = kHelloFlagWantAck;

    CHECK_BYTES_EQ(reinterpret_cast<const uint8_t*>(&h), sizeof(h), want, sizeof(want));

    // 10 centiseconds really is §8.2's 100 ms, and 6 misses really is 600 ms.
    CHECK_EQ(static_cast<uint32_t>(h.hb_period_cs) * 10u, 100u);
    CHECK_EQ(static_cast<uint32_t>(h.hb_period_cs) * 10u * h.hb_miss_limit, 600u);
}

TEST(golden, heartbeat_payload_bytes) {
    // Sequential values so a misplaced field shows up as an obvious shift in the hex dump.
    const uint8_t want[48] = {
        0x03, 0x02, 0x01, 0x00,  // 0  uptime_ms      = 0x00010203
        0x07, 0x06, 0x05, 0x04,  // 4  boot_epoch     = 0x04050607
        0x0B, 0x0A, 0x09, 0x08,  // 8  hb_seq         = 0x08090A0B
        0x0F, 0x0E, 0x0D, 0x0C,  // 12 tx_frames      = 0x0C0D0E0F
        0x13, 0x12, 0x11, 0x10,  // 16 tx_cb_ok       = 0x10111213
        0x17, 0x16, 0x15, 0x14,  // 20 tx_cb_fail     = 0x14151617
        0x1B, 0x1A, 0x19, 0x18,  // 24 rx_frames      = 0x18191A1B
        0x1F, 0x1E, 0x1D, 0x1C,  // 28 rx_lost_seqgap = 0x1C1D1E1F
        0x23, 0x22, 0x21, 0x20,  // 32 turnaround_us  = 0x20212223
        0x25, 0x24,              // 36 ack_of_msg_id  = 0x2425
        0x27, 0x26,              // 38 rtt_min_us_d8  = 0x2627
        0x29, 0x28,              // 40 rtt_max_us_d8  = 0x2829
        0x2B, 0x2A,              // 42 free_dram_kib  = 0x2A2B
        0x2C,                    // 44 espnow_version
        0x2D,                    // 45 hb_flags
        0x2F, 0x2E,              // 46 reserved0      = 0x2E2F
    };

    HeartbeatPayload hb{};
    hb.uptime_ms = 0x00010203;
    hb.boot_epoch = 0x04050607;
    hb.hb_seq = 0x08090A0B;
    hb.tx_frames = 0x0C0D0E0F;
    hb.tx_cb_ok = 0x10111213;
    hb.tx_cb_fail = 0x14151617;
    hb.rx_frames = 0x18191A1B;
    hb.rx_lost_seqgap = 0x1C1D1E1F;
    hb.turnaround_us = 0x20212223;
    hb.ack_of_msg_id = 0x2425;
    hb.rtt_min_us_d8 = 0x2627;
    hb.rtt_max_us_d8 = 0x2829;
    hb.free_dram_kib = 0x2A2B;
    hb.espnow_version = 0x2C;
    hb.hb_flags = 0x2D;
    hb.reserved0 = 0x2E2F;

    CHECK_BYTES_EQ(reinterpret_cast<const uint8_t*>(&hb), sizeof(hb), want, sizeof(want));
}

TEST(golden, hello_ack_and_bye_payload_bytes) {
    const uint8_t want_ack[12] = {
        0xEF, 0xBE, 0xAD, 0xDE,  // 0  boot_epoch = 0xDEADBEEF
        0x09, 0x00,              // 4  node_id = 9
        0x01,                    // 6  espnow_version = 1
        0x00,                    // 7  decision = kAdmitOk
        0x0A,                    // 8  hb_period_cs
        0x06,                    // 9  hb_miss_limit
        0x00, 0x00,              // 10 reserved0
    };
    HelloAckPayload a{};
    a.boot_epoch = 0xDEADBEEF;
    a.node_id = 9;
    a.espnow_version = 1;
    a.decision = kAdmitOk;
    a.hb_period_cs = 10;
    a.hb_miss_limit = 6;
    CHECK_BYTES_EQ(reinterpret_cast<const uint8_t*>(&a), sizeof(a), want_ack, sizeof(want_ack));

    const uint8_t want_bye[8] = {
        0x01, 0x00, 0x00, 0x00,  // 0 boot_epoch = 1
        0x2A, 0x00,              // 4 node_id = 42
        0x00,                    // 6 reason = shutdown
        0x00,                    // 7 reserved0
    };
    ByePayload b{};
    b.boot_epoch = 1;
    b.node_id = 42;
    b.reason = kByeReasonShutdown;
    CHECK_BYTES_EQ(reinterpret_cast<const uint8_t*>(&b), sizeof(b), want_bye, sizeof(want_bye));
}

TEST(golden, transport_profile_arithmetic_matches_5_3) {
    // §5.3's table gives 1446 B and 226 B of Potluck payload. Those are link MTU minus the 16-byte
    // header minus the 8 reserved auth-tag bytes — which is worth asserting, because it is the
    // arithmetic that makes enabling AUTH at M5 a no-op for every payload size already in use.
    CHECK_EQ(static_cast<int>(kEspNowV2LinkMtu - kHeaderSize - kAuthTagSize), 1446);
    CHECK_EQ(static_cast<int>(kEspNowV1LinkMtu - kHeaderSize - kAuthTagSize), 226);
    CHECK_EQ(static_cast<int>(kMaxPayloadV2), 1446);
    CHECK_EQ(static_cast<int>(kMaxPayloadV1), 226);
}
