// M0 payload schemas for the §5.2 membership opcodes.
//
// §5 fixes the header and the opcode numbers but not these payload bodies, so this file is their
// authoritative definition — the byte offsets in the comments are the specification, and the
// static_asserts below are its enforcement. tests/test_payloads.cpp holds golden byte arrays so a
// silent layout change fails the build rather than the fleet.
//
// Two rules produce every layout here:
//   1. Fields are ordered widest-first so each lands on its natural offset. No compiler on any
//      target may insert padding, because a padding byte is an unspecified byte on the wire.
//   2. Every reserved field is named, sized, and zeroed. "Spare bytes" that nobody zeroes are how
//      a v2 field arrives holding v1 garbage.
//
// All of these fit one unfragmented frame inside the 226 B ESP-NOW v1 profile (§5.3), which is
// required: §5.4 forbids fragmenting HELLO and HEARTBEAT.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/frame.hpp"

namespace pot {

// ---------------------------------------------------------------------------------------------
// HELLO — 0x01, 24 bytes
//
//  off  size  field           notes
//   0     4   boot_epoch      monotonic per-boot counter, NVS-backed; identifies an incarnation
//   4     4   caps            capability bitfield, reserved for M1+; zero at M0
//   8     2   node_id         echo of hdr.src, so a mis-provisioned node is visible not silent
//  10     2   reserved0       zero
//  12     1   espnow_version  1 or 2, from esp_now_get_version() — §5.3 profile pinning
//  13     1   hb_period_cs    this node's heartbeat period in centiseconds (10 == 100 ms, §8.2)
//  14     1   hb_miss_limit   misses before this node declares a peer dead (6, §8.2)
//  15     1   flags           bit0 = want HELLO_ACK
//  16     8   pubkey_fp       truncated pubkey fingerprint; zero at M0, filled at M5
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kHelloFlagWantAck = 1u << 0;

struct HelloPayload {
    uint32_t boot_epoch;
    uint32_t caps;
    uint16_t node_id;
    uint16_t reserved0;
    uint8_t espnow_version;
    uint8_t hb_period_cs;
    uint8_t hb_miss_limit;
    uint8_t flags;
    uint8_t pubkey_fp[8];
};

static_assert(sizeof(HelloPayload) == 24, "HELLO is 24 bytes");
static_assert(offsetof(HelloPayload, boot_epoch) == 0, "HELLO offset");
static_assert(offsetof(HelloPayload, caps) == 4, "HELLO offset");
static_assert(offsetof(HelloPayload, node_id) == 8, "HELLO offset");
static_assert(offsetof(HelloPayload, reserved0) == 10, "HELLO offset");
static_assert(offsetof(HelloPayload, espnow_version) == 12, "HELLO offset");
static_assert(offsetof(HelloPayload, hb_period_cs) == 13, "HELLO offset");
static_assert(offsetof(HelloPayload, hb_miss_limit) == 14, "HELLO offset");
static_assert(offsetof(HelloPayload, flags) == 15, "HELLO offset");
static_assert(offsetof(HelloPayload, pubkey_fp) == 16, "HELLO offset");

// ---------------------------------------------------------------------------------------------
// HELLO_ACK — 0x02, 12 bytes
//
//  off  size  field           notes
//   0     4   boot_epoch      the acking node's epoch
//   4     2   node_id         the acking node's id
//   6     1   espnow_version  the acking node's ESP-NOW version — the other half of §5.3 pinning
//   7     1   decision        kAdmitOk or a kAdmitRefused* code
//   8     1   hb_period_cs    the acking node's period; the two ends need not match
//   9     1   hb_miss_limit   the acking node's miss limit
//  10     2   reserved0       zero
// ---------------------------------------------------------------------------------------------

struct HelloAckPayload {
    uint32_t boot_epoch;
    uint16_t node_id;
    uint8_t espnow_version;
    uint8_t decision;
    uint8_t hb_period_cs;
    uint8_t hb_miss_limit;
    uint16_t reserved0;
};

static_assert(sizeof(HelloAckPayload) == 12, "HELLO_ACK is 12 bytes");
static_assert(offsetof(HelloAckPayload, boot_epoch) == 0, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, node_id) == 4, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, espnow_version) == 6, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, decision) == 7, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, hb_period_cs) == 8, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, hb_miss_limit) == 9, "HELLO_ACK offset");
static_assert(offsetof(HelloAckPayload, reserved0) == 10, "HELLO_ACK offset");

// ---------------------------------------------------------------------------------------------
// HEARTBEAT — 0x03, 48 bytes. Liveness plus this link's statistics (§8.2), so a capture of the
// heartbeat stream alone reconstructs both ends' view of the link.
//
// On the RTT fields: clocks are not synchronised, so this payload carries no remote timestamp.
// It carries a remote *duration* — turnaround_us, measured entirely on the replying node's own
// clock — because durations are comparable across unsynced clocks and instants are not. See
// link_stats.hpp for the full measurement chain and what it refuses to compute.
//
//  off  size  field            notes
//   0     4   uptime_ms        sender's uptime; with boot_epoch, distinguishes reboot from silence
//   4     4   boot_epoch       sender's incarnation
//   8     4   hb_seq           sender's heartbeat counter for this peer; gap detection
//  12     4   tx_frames        frames the sender submitted to its transport for this peer
//  16     4   tx_cb_ok         send-callbacks reporting a MAC-layer ACK
//  20     4   tx_cb_fail       send-callbacks reporting failure
//  24     4   rx_frames        frames the sender accepted from this peer
//  28     4   rx_lost_seqgap   inbound losses the sender inferred from seq gaps
//  32     4   turnaround_us    if IS_REPLY: recv-to-submit delay on the sender's clock; else 0
//  36     2   ack_of_msg_id    if IS_REPLY: the msg_id being answered; else 0
//  38     2   rtt_min_us_d8    sender's observed min RTT to this peer, ÷8; 0xFFFF = none yet
//  40     2   rtt_max_us_d8    sender's observed max RTT to this peer, ÷8; 0xFFFF = none yet
//  42     2   free_dram_kib    heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) >> 10
//  44     1   espnow_version   repeated from HELLO so a capture is self-describing mid-stream
//  45     1   hb_flags         bit0 = IS_REPLY
//  46     2   reserved0        zero
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kHbFlagIsReply = 1u << 0;

// Sentinel for "no sample yet" in the ÷8 quantised RTT fields. A real sample that would quantise
// to this value is clamped to 0xFFFE instead, so the sentinel is never ambiguous.
constexpr uint16_t kRttUnknownD8 = 0xFFFF;

struct HeartbeatPayload {
    uint32_t uptime_ms;
    uint32_t boot_epoch;
    uint32_t hb_seq;
    uint32_t tx_frames;
    uint32_t tx_cb_ok;
    uint32_t tx_cb_fail;
    uint32_t rx_frames;
    uint32_t rx_lost_seqgap;
    uint32_t turnaround_us;
    uint16_t ack_of_msg_id;
    uint16_t rtt_min_us_d8;
    uint16_t rtt_max_us_d8;
    uint16_t free_dram_kib;
    uint8_t espnow_version;
    uint8_t hb_flags;
    uint16_t reserved0;
};

static_assert(sizeof(HeartbeatPayload) == 48, "HEARTBEAT is 48 bytes");
static_assert(offsetof(HeartbeatPayload, uptime_ms) == 0, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, boot_epoch) == 4, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, hb_seq) == 8, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, tx_frames) == 12, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, tx_cb_ok) == 16, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, tx_cb_fail) == 20, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, rx_frames) == 24, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, rx_lost_seqgap) == 28, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, turnaround_us) == 32, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, ack_of_msg_id) == 36, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, rtt_min_us_d8) == 38, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, rtt_max_us_d8) == 40, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, free_dram_kib) == 42, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, espnow_version) == 44, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, hb_flags) == 45, "HEARTBEAT offset");
static_assert(offsetof(HeartbeatPayload, reserved0) == 46, "HEARTBEAT offset");

// ---------------------------------------------------------------------------------------------
// BYE — 0x04, 8 bytes
//
//  off  size  field       notes
//   0     4   boot_epoch  the departing node's incarnation
//   4     2   node_id     the departing node's id
//   6     1   reason      kByeReason*
//   7     1   reserved0   zero
// ---------------------------------------------------------------------------------------------

struct ByePayload {
    uint32_t boot_epoch;
    uint16_t node_id;
    uint8_t reason;
    uint8_t reserved0;
};

static_assert(sizeof(ByePayload) == 8, "BYE is 8 bytes");
static_assert(offsetof(ByePayload, boot_epoch) == 0, "BYE offset");
static_assert(offsetof(ByePayload, node_id) == 4, "BYE offset");
static_assert(offsetof(ByePayload, reason) == 6, "BYE offset");
static_assert(offsetof(ByePayload, reserved0) == 7, "BYE offset");

// ---------------------------------------------------------------------------------------------
// ERR — 0x7F, 8-byte fixed part followed by detail_len bytes of UTF-8 detail (not NUL-terminated).
// §5.2: "never silently dropped" — so the detail is for a human reading a capture, and the code
// is for a machine. Codes below 0x0100 are pot::FrameError values (see opcodes.hpp).
//
//  off  size  field       notes
//   0     2   code        kErr* / kErrFrameBase + FrameError
//   2     2   ref_msg_id  msg_id of the frame that caused this, or 0
//   4     1   detail_len  bytes of detail following the fixed part
//   5     3   reserved0   zero
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kErrDetailMax = 64;

struct ErrPayload {
    uint16_t code;
    uint16_t ref_msg_id;
    uint8_t detail_len;
    uint8_t reserved0[3];
};

static_assert(sizeof(ErrPayload) == 8, "ERR fixed part is 8 bytes");
static_assert(offsetof(ErrPayload, code) == 0, "ERR offset");
static_assert(offsetof(ErrPayload, ref_msg_id) == 2, "ERR offset");
static_assert(offsetof(ErrPayload, detail_len) == 4, "ERR offset");
static_assert(offsetof(ErrPayload, reserved0) == 5, "ERR offset");

// Every M0 payload must fit the v1 profile unfragmented, because §5.4 forbids fragmenting HELLO
// and HEARTBEAT and there is no reason for the others to need it either.
static_assert(sizeof(HelloPayload) <= kMaxPayloadV1, "§5.4: HELLO must not fragment");
static_assert(sizeof(HeartbeatPayload) <= kMaxPayloadV1, "§5.4: HEARTBEAT must not fragment");
static_assert(sizeof(ErrPayload) + kErrDetailMax <= kMaxPayloadV1, "ERR must fit the v1 profile");

// ---------------------------------------------------------------------------------------------
// Load helpers. A received payload lives in a transport buffer whose alignment nothing promises,
// so these copy rather than cast. On both targets the copy compiles to the same loads a cast
// would emit; unlike a cast it is also defined behaviour on an odd address.
//
// Each returns false when fewer bytes arrived than the opcode's fixed part — the caller answers
// with kErrPayloadTooShort rather than reading past the buffer.
// ---------------------------------------------------------------------------------------------

bool load_hello(const uint8_t* payload, uint16_t len, HelloPayload& out);
bool load_hello_ack(const uint8_t* payload, uint16_t len, HelloAckPayload& out);
bool load_heartbeat(const uint8_t* payload, uint16_t len, HeartbeatPayload& out);
bool load_bye(const uint8_t* payload, uint16_t len, ByePayload& out);
bool load_err(const uint8_t* payload, uint16_t len, ErrPayload& out);

// Quantise a µs duration into the ÷8 wire representation, clamping so that a real sample can
// never collide with kRttUnknownD8.
uint16_t quantise_us_d8(uint32_t us);
uint32_t dequantise_us_d8(uint16_t d8);

}  // namespace pot
