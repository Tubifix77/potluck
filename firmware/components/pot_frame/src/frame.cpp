// Potluck Frame v1 codec implementation — ARCHITECTURE.md §5.1, §5.2, §5.4.

#include "pot/frame.hpp"

#include <cstring>

#include "pot/opcodes.hpp"

namespace pot {

const char* frame_error_str(FrameError e) {
    switch (e) {
        case FrameError::Ok: return "ok";
        case FrameError::TooShort: return "too_short";
        case FrameError::BadMagic: return "bad_magic";
        case FrameError::BadVersion: return "bad_version";
        case FrameError::ReservedFlagSet: return "reserved_flag_set";
        case FrameError::BadClass: return "bad_class";
        case FrameError::MissingAuthTag: return "missing_auth_tag";
        case FrameError::LengthMismatch: return "length_mismatch";
        case FrameError::FragNotAllowed: return "frag_not_allowed";
        case FrameError::PayloadTooLong: return "payload_too_long";
        case FrameError::BufferTooSmall: return "buffer_too_small";
        case FrameError::BadArgument: return "bad_argument";
    }
    return "unknown";
}

const char* opcode_str(uint8_t opcode) {
    switch (opcode) {
        case kOpHello: return "HELLO";
        case kOpHelloAck: return "HELLO_ACK";
        case kOpHeartbeat: return "HEARTBEAT";
        case kOpBye: return "BYE";
        case kOpSafeState: return "SAFE_STATE";
        case kOpErr: return "ERR";
        default: return "?";
    }
}

bool opcode_forbids_frag(uint8_t opcode) {
    // §5.4: "No fragmentation for SAFE_STATE, HEARTBEAT, or HELLO." Liveness and safety must
    // never depend on reassembly. HELLO_ACK is deliberately absent — the specification lists
    // three opcodes and this function implements exactly those three.
    return opcode == kOpSafeState || opcode == kOpHeartbeat || opcode == kOpHello;
}

FrameError parse(const uint8_t* buf, size_t len, Frame& out, uint16_t max_payload) {
    if (buf == nullptr || len < kHeaderSize) {
        return FrameError::TooShort;
    }

    // Copy rather than cast: an ESP-NOW receive buffer carries no alignment promise, and Header
    // needs 2-byte alignment. The layout static_asserts in frame.hpp are what make this copy
    // equivalent to the struct cast §5.1 describes; the compiler emits the same loads either way.
    Header hdr{};
    std::memcpy(&hdr, buf, kHeaderSize);

    if (hdr.magic != kMagic) {
        return FrameError::BadMagic;
    }
    if (ver_of(hdr.ver_flags) != kVersion) {
        return FrameError::BadVersion;
    }

    const uint8_t flags = flags_of(hdr.ver_flags);
    if ((flags & kFlagReserved) != 0) {
        // §5.1 says bit 0 must be 0. Rejecting it now is what lets a future version claim it.
        return FrameError::ReservedFlagSet;
    }
    if (lclass_of(hdr.lclass_pri) > kClassMax) {
        // §4 defines L0..L4; 5..7 are not classes, so a frame carrying one is not a Potluck Frame.
        return FrameError::BadClass;
    }

    const bool has_auth = (flags & kFlagAuth) != 0;
    const size_t overhead = kHeaderSize + (has_auth ? kAuthTagSize : 0);
    if (len < overhead) {
        return FrameError::MissingAuthTag;
    }

    const size_t payload_len = len - overhead;
    if (payload_len > max_payload) {
        return FrameError::PayloadTooLong;
    }

    const bool is_frag = (flags & kFlagFrag) != 0;
    if (is_frag && opcode_forbids_frag(hdr.opcode)) {
        return FrameError::FragNotAllowed;
    }

    // Length consistency. This is the check that catches the §5.3 truncation hazard: a v1.0
    // receiver handed a longer v2.0 frame may deliver only its first 250 bytes, and a silently
    // shortened frame parsed as if whole is worse than a dropped one.
    if (is_frag) {
        if (static_cast<size_t>(hdr.frag_off) + payload_len > hdr.total_len) {
            return FrameError::LengthMismatch;
        }
        if (payload_len == 0) {
            // A fragment carrying nothing advances no reassembly and cannot be distinguished
            // from a probe; §5.4's reassembly cap makes accepting them a free denial of service.
            return FrameError::LengthMismatch;
        }
    } else {
        if (hdr.frag_off != 0 || payload_len != hdr.total_len) {
            return FrameError::LengthMismatch;
        }
    }

    out.hdr = hdr;
    out.payload_len = static_cast<uint16_t>(payload_len);
    out.payload = payload_len > 0 ? buf + kHeaderSize : nullptr;
    out.auth_tag = has_auth ? buf + kHeaderSize + payload_len : nullptr;
    return FrameError::Ok;
}

FrameError encode(const EncodeSpec& spec, const uint8_t* payload, uint16_t payload_len,
                  uint8_t* out, size_t cap, size_t& written) {
    written = 0;

    if (out == nullptr) {
        return FrameError::BadArgument;
    }
    if (payload == nullptr && payload_len != 0) {
        return FrameError::BadArgument;
    }
    if (spec.lclass > kClassMax || spec.priority > kPriorityMax) {
        return FrameError::BadArgument;
    }
    if (spec.frag && opcode_forbids_frag(spec.opcode)) {
        // Refuse at the sender too. A rule enforced only on receive is a rule that ships broken
        // firmware to every peer that has not been updated yet.
        return FrameError::FragNotAllowed;
    }

    const size_t total = encoded_size(spec, payload_len);
    if (total > cap) {
        return FrameError::BufferTooSmall;
    }

    // total_len is the whole message across fragments; for an unfragmented frame that is just the
    // payload. Deriving it rather than trusting the caller removes the commonest way to emit a
    // frame that its own parser would reject.
    uint16_t total_len;
    if (spec.frag) {
        total_len = spec.total_len;
        if (static_cast<size_t>(spec.frag_off) + payload_len > total_len || payload_len == 0) {
            return FrameError::LengthMismatch;
        }
    } else {
        total_len = payload_len;
    }

    uint8_t flags = 0;
    if (spec.frag) flags |= kFlagFrag;
    if (spec.ack_req) flags |= kFlagAckReq;
    if (spec.auth) flags |= kFlagAuth;

    Header hdr{};
    hdr.magic = kMagic;
    hdr.ver_flags = make_ver_flags(kVersion, flags);
    hdr.src = spec.src;
    hdr.dst = spec.dst;
    hdr.opcode = spec.opcode;
    hdr.lclass_pri = make_lclass_pri(spec.lclass, spec.priority);
    hdr.seq = spec.seq;
    hdr.msg_id = spec.msg_id;
    hdr.frag_off = spec.frag ? spec.frag_off : 0;
    hdr.total_len = total_len;

    std::memcpy(out, &hdr, kHeaderSize);
    if (payload_len > 0) {
        std::memcpy(out + kHeaderSize, payload, payload_len);
    }
    if (spec.auth) {
        // Reserved from day one, enforced at M5 (§14). Zeroing rather than leaving the bytes
        // undefined is the difference between a reserved field and an information leak.
        std::memset(out + kHeaderSize + payload_len, 0, kAuthTagSize);
    }

    written = total;
    return FrameError::Ok;
}

}  // namespace pot
