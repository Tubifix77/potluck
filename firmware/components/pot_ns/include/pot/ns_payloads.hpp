// Wire payloads for READ, WRITE and REPLY — ARCHITECTURE.md §5.2, §7.2, §4 rule 2.
//
// Same rules as the M0 payloads: fields ordered widest-first so every one lands on its natural
// offset with no implicit padding, every reserved field named and zeroed, and golden byte arrays in
// tests/test_ns_wire.cpp so a silent layout change fails the build rather than the fleet.
//
// The reply carries §4 rule 2's whole tuple. That is the point of the section and it is why there
// is no compact "just the value" reply: a caller that wanted only the number would be the bug.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/frame.hpp"
#include "pot/namespace.hpp"
#include "pot/value.hpp"

namespace pot {

// ---------------------------------------------------------------------------------------------
// READ — 0x10, 8 bytes
//
//  off  size  field       notes
//   0     4   path_hash   §7.2's FNV-1a/32 of the canonical path
//   4     2   flags       reserved, zero
//   6     2   reserved0   zero
// ---------------------------------------------------------------------------------------------

struct ReadPayload {
    uint32_t path_hash;
    uint16_t flags;
    uint16_t reserved0;
};

static_assert(sizeof(ReadPayload) == 8, "READ is 8 bytes");
static_assert(offsetof(ReadPayload, path_hash) == 0, "READ offset");
static_assert(offsetof(ReadPayload, flags) == 4, "READ offset");

// ---------------------------------------------------------------------------------------------
// WRITE — 0x11, 16 bytes
//
//  off  size  field        notes
//   0     4   path_hash
//   4     1   value_type   ValueType
//   5     1   value_len    bytes used in value_raw
//   6     2   flags        bit0: from outside the cell (M13); the other bits zero
//   8     8   value_raw    the payload, little-endian, as Value holds it
//
// M13: `flags` was reserved0, always zero, so every WRITE before M13 reads as one from inside the cell. The
// presenting role sets kWriteFromOutside on every write it passes on from another system, and the owning board
// then applies the package's outside rules (Namespace::write_outside). The origin travels in the payload of the
// one opcode that needs it, under the frame's authentication like the rest -- not in the header's reserved bit
// (section 5.1), which every receiver decodes for every opcode.
// ---------------------------------------------------------------------------------------------

constexpr uint16_t kWriteFromOutside = 1u << 0;

// M14: READ's flags bit 0 -- describe the entry rather than read it. A node that holds a resource it has no actor
// type for (the PC's node presenting an app's output: pot_hostnode carries only the built-in types it runs) learns
// the declaration from one that does, as boards already learn each other's outputs from the image (M8.2's adoption).
// The answer is a REPLY with reply_to kReplyDescribe and, in place of a reading: timestamp_ms the staleness bound,
// value_type the declared type, unit and latency_class the entry's, and value_raw = owner u16, policy u8, kind u8,
// access u8 (value_len 5). A board before M14 ignores the flag and answers a reading (reply_to kOpRead), which the
// asker drops.
constexpr uint16_t kReadDescribe = 1u << 0;
constexpr uint8_t kReplyDescribe = 0x15;  // the reserved LIST opcode's number: no frame carries it as an opcode

struct WritePayload {
    uint32_t path_hash;
    uint8_t value_type;
    uint8_t value_len;
    uint16_t flags;
    uint8_t value_raw[kValueBytesMax];
};

static_assert(sizeof(WritePayload) == 16, "WRITE is 16 bytes");
static_assert(offsetof(WritePayload, path_hash) == 0, "WRITE offset");
static_assert(offsetof(WritePayload, value_type) == 4, "WRITE offset");
static_assert(offsetof(WritePayload, value_len) == 5, "WRITE offset");
static_assert(offsetof(WritePayload, flags) == 6, "WRITE offset");
static_assert(offsetof(WritePayload, value_raw) == 8, "WRITE offset");

// ---------------------------------------------------------------------------------------------
// REPLY — 0x21, 28 bytes. Answers both READ and WRITE.
//
// `reply_to` makes the frame self-describing rather than leaving the receiver to infer the shape
// from the correlation table. The table is still consulted — msg_id is what matches a reply to its
// request — but a frame whose meaning depends on state the receiver might have dropped is a frame
// that becomes unreadable in a capture, and §7.6 wants captures to be readable on their own.
//
//  off  size  field          notes
//   0     4   path_hash
//   4     4   timestamp_ms   the owner's clock, when it sampled — never our clock
//   8     4   age_ms         how old when answered, on the *answering* node's reckoning
//  12     2   unit           Unit
//  14     1   reply_to       kOpRead or kOpWrite
//  15     1   status         NsError
//  16     1   quality        Quality — §4 rule 2, never omitted
//  17     1   latency_class  §4 L0..L4
//  18     1   value_type     ValueType; None when the quality carries no value
//  19     1   value_len
//  20     8   value_raw
// ---------------------------------------------------------------------------------------------

struct ReplyPayload {
    uint32_t path_hash;
    uint32_t timestamp_ms;
    uint32_t age_ms;
    uint16_t unit;
    uint8_t reply_to;
    uint8_t status;
    uint8_t quality;
    uint8_t latency_class;
    uint8_t value_type;
    uint8_t value_len;
    uint8_t value_raw[kValueBytesMax];
};

static_assert(sizeof(ReplyPayload) == 28, "REPLY is 28 bytes");
static_assert(offsetof(ReplyPayload, path_hash) == 0, "REPLY offset");
static_assert(offsetof(ReplyPayload, timestamp_ms) == 4, "REPLY offset");
static_assert(offsetof(ReplyPayload, age_ms) == 8, "REPLY offset");
static_assert(offsetof(ReplyPayload, unit) == 12, "REPLY offset");
static_assert(offsetof(ReplyPayload, reply_to) == 14, "REPLY offset");
static_assert(offsetof(ReplyPayload, status) == 15, "REPLY offset");
static_assert(offsetof(ReplyPayload, quality) == 16, "REPLY offset");
static_assert(offsetof(ReplyPayload, latency_class) == 17, "REPLY offset");
static_assert(offsetof(ReplyPayload, value_type) == 18, "REPLY offset");
static_assert(offsetof(ReplyPayload, value_len) == 19, "REPLY offset");
static_assert(offsetof(ReplyPayload, value_raw) == 20, "REPLY offset");

// ---------------------------------------------------------------------------------------------
// CALL / CAST — 0x20 and 0x22, an 8-byte header plus opaque arguments
//
//  off  size  field       notes
//   0     4   path_hash   the actor's canonical path, hashed like any other §7.2 path
//   4     2   flags       reserved, zero
//   6     2   arg_len     bytes of argument data following this header
//   8   arg_len  args     opaque to the transport; meaning is between caller and callee
//
// THE RESULT COMES BACK AS AN ORDINARY REPLY, and that is a deliberate constraint rather than an
// oversight. A REPLY carries a Value, so a call returns at most 8 bytes. Anything larger is meant to
// travel the way every other datum in this system travels: the callee *publishes* it to a namespace
// path and the reply says only that it is there. That keeps messages small, keeps results readable
// by anything that can read the namespace, and costs the tested REPLY codec no changes at all.
//
// CAST is the same shape with no reply. The opcode carries the difference, so the wire layout is
// shared rather than duplicated.
// ---------------------------------------------------------------------------------------------

struct CallPayload {
    uint32_t path_hash;
    uint16_t flags;
    uint16_t arg_len;
};

static_assert(sizeof(CallPayload) == 8, "CALL header is 8 bytes");
static_assert(offsetof(CallPayload, path_hash) == 0, "CALL offset");
static_assert(offsetof(CallPayload, flags) == 4, "CALL offset");
static_assert(offsetof(CallPayload, arg_len) == 6, "CALL offset");

using CastPayload = CallPayload;

// How many argument bytes fit unfragmented on each transport profile (§5.3). A caller that stays
// under the v1 figure works on a mixed-version link without touching §5.4's reassembly path.
constexpr uint16_t kMaxCallArgsV1 = static_cast<uint16_t>(kMaxPayloadV1 - sizeof(CallPayload));
constexpr uint16_t kMaxCallArgsV2 = static_cast<uint16_t>(kMaxPayloadV2 - sizeof(CallPayload));

// All four fit the 226 B ESP-NOW v1 floor unfragmented, so a namespace read works on a mixed-
// version link without touching the reassembly path (§5.3, §5.4).
static_assert(sizeof(ReadPayload) <= kMaxPayloadV1, "READ must fit the v1 profile");
static_assert(sizeof(WritePayload) <= kMaxPayloadV1, "WRITE must fit the v1 profile");
static_assert(sizeof(ReplyPayload) <= kMaxPayloadV1, "REPLY must fit the v1 profile");
static_assert(sizeof(CallPayload) <= kMaxPayloadV1, "CALL header must fit the v1 profile");

// ---------------------------------------------------------------------------------------------
// Loaders and converters
// ---------------------------------------------------------------------------------------------

bool load_read(const uint8_t* payload, uint16_t len, ReadPayload& out);
bool load_write(const uint8_t* payload, uint16_t len, WritePayload& out);
bool load_reply(const uint8_t* payload, uint16_t len, ReplyPayload& out);

// CALL and CAST share a loader, since they share a layout. Unlike the fixed payloads this one also
// checks that the declared arg_len is actually present in the frame: a peer claiming more arguments
// than it sent would otherwise have the callee read past the payload. Rejected rather than clamped,
// because clamping hands the callee a silently truncated argument list, which is the same class of
// mistake as serving a stale value unmarked.
bool load_call(const uint8_t* payload, uint16_t len, CallPayload& out);

// Pack a Reading into a REPLY, and unpack it again. Kept as functions rather than done inline at
// the two call sites, because §4 rule 2's tuple must survive the round trip intact and one place to
// get that wrong is better than two.
void reply_from_reading(ReplyPayload& out, uint32_t path_hash, const Reading& r, NsError status);
Reading reading_from_reply(const ReplyPayload& p);

Value value_from_wire(uint8_t type, uint8_t len, const uint8_t* raw);
void value_to_wire(const Value& v, uint8_t& type, uint8_t& len, uint8_t* raw);

}  // namespace pot
