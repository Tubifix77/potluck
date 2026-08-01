// Potluck Frame v1 codec — ARCHITECTURE.md §5.1, §5.2, §5.4.
//
// The wire format is the product; this file is its only implementation. One parser, one fuzz
// target (tests/test_frame_fuzz.cpp), one capture format. Nothing here may depend on ESP-IDF,
// FreeRTOS, the heap, or errno: it compiles on the host for tests and on Xtensa for the node.
//
// Two properties are asserted at compile time rather than trusted:
//   * the header is exactly 16 bytes at the §5.1 offsets, so a struct cast is legal;
//   * the target is little-endian, because §5.1 says the fields are.
// If either assertion ever fires, the wire format has drifted from the specification and the
// build stops. That is the intended outcome.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

// ---------------------------------------------------------------------------------------------
// Byte order. §5.1 is little-endian, so a struct cast is only correct on a little-endian target.
// ---------------------------------------------------------------------------------------------
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "Potluck Frame v1 is little-endian (ARCHITECTURE.md §5.1). Porting to a big-endian "
              "target requires an explicit byte-swapping codec, not a struct cast.");
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64))
// MSVC only targets little-endian architectures.
#else
#error "Cannot determine target byte order; Potluck Frame v1 requires little-endian (§5.1)."
#endif

// ---------------------------------------------------------------------------------------------
// Constants — §5.1
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kMagic = 0xD9;
constexpr uint8_t kVersion = 1;

constexpr size_t kHeaderSize = 16;
constexpr size_t kAuthTagSize = 8;  // truncated HMAC-SHA256, §5.4

// ver_flags, byte 1. Bits [7:4] are the version; bits [3:0] are these flags.
constexpr uint8_t kFlagFrag = 1u << 3;      // this is a fragment
constexpr uint8_t kFlagAckReq = 1u << 2;    // sender wants an ACK
constexpr uint8_t kFlagAuth = 1u << 1;      // auth tag present (§5.4)
constexpr uint8_t kFlagReserved = 1u << 0;  // must be 0
constexpr uint8_t kFlagMask = 0x0Fu;

// Node ids — §5.1
constexpr uint16_t kNodeUnprovisioned = 0x0000;
constexpr uint16_t kNodeBroadcast = 0xFFFF;

// Latency classes — §4. lclass_pri bits [7:5] carry L0..L4; 5..7 are not classes.
constexpr uint8_t kClassL0 = 0;
constexpr uint8_t kClassL1 = 1;
constexpr uint8_t kClassL2 = 2;
constexpr uint8_t kClassL3 = 3;
constexpr uint8_t kClassL4 = 4;
constexpr uint8_t kClassMax = kClassL4;
constexpr uint8_t kPriorityMax = 31;  // lclass_pri bits [4:0]

// Transport profile payload caps — §5.3. Every profile reserves kAuthTagSize even while AUTH is
// unset, so enabling frame auth at M5 cannot shrink a payload that already shipped (§14).
constexpr uint16_t kEspNowV2LinkMtu = 1470;  // ESP_NOW_MAX_DATA_LEN_V2
constexpr uint16_t kEspNowV1LinkMtu = 250;   // ESP_NOW_MAX_DATA_LEN
constexpr uint16_t kMaxPayloadV2 = kEspNowV2LinkMtu - kHeaderSize - kAuthTagSize;  // 1446
constexpr uint16_t kMaxPayloadV1 = kEspNowV1LinkMtu - kHeaderSize - kAuthTagSize;  // 226

static_assert(kMaxPayloadV2 == 1446, "§5.3 states 1446 B for the ESP-NOW v2 profile");
static_assert(kMaxPayloadV1 == 226, "§5.3 states 226 B for the ESP-NOW v1 profile");

// ---------------------------------------------------------------------------------------------
// Header — §5.1. Field order and types are chosen so that every member lands on its specified
// offset with no padding, which is what makes the struct cast legal.
// ---------------------------------------------------------------------------------------------

struct Header {
    uint8_t magic;       // 0
    uint8_t ver_flags;   // 1
    uint16_t src;        // 2
    uint16_t dst;        // 4
    uint8_t opcode;      // 6
    uint8_t lclass_pri;  // 7
    uint16_t seq;        // 8
    uint16_t msg_id;     // 10
    uint16_t frag_off;   // 12
    uint16_t total_len;  // 14
};

static_assert(sizeof(Header) == kHeaderSize, "§5.1: the header is exactly 16 bytes");
static_assert(alignof(Header) == 2, "§5.1: 16-bit fields on even offsets, so alignment is 2");
static_assert(offsetof(Header, magic) == 0, "§5.1 offset");
static_assert(offsetof(Header, ver_flags) == 1, "§5.1 offset");
static_assert(offsetof(Header, src) == 2, "§5.1 offset");
static_assert(offsetof(Header, dst) == 4, "§5.1 offset");
static_assert(offsetof(Header, opcode) == 6, "§5.1 offset");
static_assert(offsetof(Header, lclass_pri) == 7, "§5.1 offset");
static_assert(offsetof(Header, seq) == 8, "§5.1 offset");
static_assert(offsetof(Header, msg_id) == 10, "§5.1 offset");
static_assert(offsetof(Header, frag_off) == 12, "§5.1 offset");
static_assert(offsetof(Header, total_len) == 14, "§5.1 offset");

// ---------------------------------------------------------------------------------------------
// Field accessors. Trivial, but they keep the shift-and-mask arithmetic in exactly one place.
// ---------------------------------------------------------------------------------------------

constexpr uint8_t ver_of(uint8_t ver_flags) { return static_cast<uint8_t>(ver_flags >> 4); }
constexpr uint8_t flags_of(uint8_t ver_flags) { return static_cast<uint8_t>(ver_flags & kFlagMask); }
constexpr uint8_t make_ver_flags(uint8_t version, uint8_t flags) {
    return static_cast<uint8_t>((version << 4) | (flags & kFlagMask));
}

constexpr uint8_t lclass_of(uint8_t lclass_pri) { return static_cast<uint8_t>(lclass_pri >> 5); }
constexpr uint8_t priority_of(uint8_t lclass_pri) { return static_cast<uint8_t>(lclass_pri & 0x1Fu); }
constexpr uint8_t make_lclass_pri(uint8_t lclass, uint8_t priority) {
    return static_cast<uint8_t>((lclass << 5) | (priority & 0x1Fu));
}

// ---------------------------------------------------------------------------------------------
// Errors. One error space for encode and decode: a frame is either exactly what §5 describes or
// it is rejected with a reason. Values are stable — the serial JSON and the ERR opcode carry them.
// ---------------------------------------------------------------------------------------------

enum class FrameError : uint8_t {
    Ok = 0,
    TooShort = 1,        // fewer than 16 bytes on the wire
    BadMagic = 2,        // byte 0 is not 0xD9
    BadVersion = 3,      // ver_flags[7:4] is not 1
    ReservedFlagSet = 4, // ver_flags[0] is set; §5.1 says it must be 0
    BadClass = 5,        // lclass_pri[7:5] is 5..7; §4 defines only L0..L4
    MissingAuthTag = 6,  // AUTH is set but the 8 tag bytes are not present
    LengthMismatch = 7,  // payload length disagrees with total_len / frag_off
    FragNotAllowed = 8,  // FRAG set on an opcode §5.4 forbids fragmenting
    PayloadTooLong = 9,  // payload exceeds the transport profile's cap (§5.3)
    BufferTooSmall = 10, // encode: destination buffer cannot hold the frame
    BadArgument = 11,    // encode: class > L4, priority > 31, or a null payload with length
};

const char* frame_error_str(FrameError e);

// ---------------------------------------------------------------------------------------------
// A parsed frame. Zero-copy: payload and auth_tag point into the caller's buffer, which must
// outlive the Frame. On a node that buffer is an RX ring slot (§6); nothing is duplicated.
// ---------------------------------------------------------------------------------------------

struct Frame {
    Header hdr{};
    const uint8_t* payload = nullptr;
    uint16_t payload_len = 0;
    const uint8_t* auth_tag = nullptr;  // null unless AUTH is set; never verified before M5

    bool is_frag() const { return (flags_of(hdr.ver_flags) & kFlagFrag) != 0; }
    bool wants_ack() const { return (flags_of(hdr.ver_flags) & kFlagAckReq) != 0; }
    bool has_auth() const { return (flags_of(hdr.ver_flags) & kFlagAuth) != 0; }
    uint8_t lclass() const { return lclass_of(hdr.lclass_pri); }
    uint8_t priority() const { return priority_of(hdr.lclass_pri); }
};

// ---------------------------------------------------------------------------------------------
// What to encode. Deliberately a plain struct: no builder, no chaining. §6 does not pay for it.
// ---------------------------------------------------------------------------------------------

struct EncodeSpec {
    uint16_t src = kNodeUnprovisioned;
    uint16_t dst = kNodeBroadcast;
    uint8_t opcode = 0;
    uint8_t lclass = kClassL3;
    uint8_t priority = 0;
    uint16_t seq = 0;
    uint16_t msg_id = 0;
    bool ack_req = false;
    bool auth = false;  // reserve and zero the tag; M5 fills it in
    bool frag = false;
    uint16_t frag_off = 0;
    uint16_t total_len = 0;  // ignored when frag is false: set from payload_len
};

// Total wire size of a frame with this spec and payload length.
constexpr size_t encoded_size(const EncodeSpec& spec, uint16_t payload_len) {
    return kHeaderSize + payload_len + (spec.auth ? kAuthTagSize : 0);
}

// ---------------------------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------------------------

// Decode and validate `len` bytes. `max_payload` is the transport profile's cap (§5.3) — pass
// the value pinned for the peer the bytes arrived from, so a v1 peer's 226 B ceiling is enforced
// on the way in as well as on the way out.
//
// Every rejection is a positive check against §5, not a heuristic. In particular a v2 frame
// truncated to 250 B by a v1.0 receiver fails LengthMismatch rather than being parsed: that is
// the concrete reason §5.4's total_len rule exists.
FrameError parse(const uint8_t* buf, size_t len, Frame& out, uint16_t max_payload = kMaxPayloadV2);

// Encode into `out`. Writes `written` bytes on success. When `spec.auth` is set the 8 tag bytes
// are appended and zeroed — reserved from day one, enforced at M5 (§14).
FrameError encode(const EncodeSpec& spec, const uint8_t* payload, uint16_t payload_len,
                  uint8_t* out, size_t cap, size_t& written);

// True for the opcodes §5.4 forbids fragmenting: liveness and safety must never depend on
// reassembly. Declared here because the codec enforces it — one parser, one place.
bool opcode_forbids_frag(uint8_t opcode);

}  // namespace pot
