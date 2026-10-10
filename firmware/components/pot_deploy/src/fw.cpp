// M11: firmware over the cell, the portable half. See pot/fw.hpp.

#include "pot/fw.hpp"

#include <cstring>

namespace pot {

namespace {

void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

const char* fw_status_str(FwStatus s) {
    switch (s) {
        case FwStatus::Ok: return "ok";
        case FwStatus::TooLarge: return "too_large";
        case FwStatus::Downgrade: return "downgrade";
        case FwStatus::NotStarted: return "not_started";
        case FwStatus::BadOffset: return "bad_offset";
        case FwStatus::Malformed: return "malformed";
        case FwStatus::Busy: return "busy";
        case FwStatus::BadSignature: return "bad_signature";
        case FwStatus::StoreFailed: return "store_failed";
        case FwStatus::BadImage: return "bad_image";
        case FwStatus::Unreachable: return "unreachable";
        case FwStatus::Incomplete: return "incomplete";
    }
    return "?";
}

size_t store_fw_begin(const FwBegin& b, uint8_t* out, size_t cap) {
    if (cap < kFwBeginLen || b.trailer == nullptr) return 0;
    wr16(out, b.target);
    wr32(out + 2, b.image_len);
    wr32(out + 6, b.counter);
    std::memcpy(out + 10, b.trailer, kFwTrailerLen);
    return kFwBeginLen;
}

size_t store_fw_chunk(const FwChunk& c, uint8_t* out, size_t cap) {
    if (c.len > kFwChunkMax || cap < kFwChunkHeaderLen + c.len) return 0;
    wr16(out, c.target);
    wr32(out + 2, c.offset);
    wr16(out + 6, c.len);
    if (c.len > 0) std::memcpy(out + kFwChunkHeaderLen, c.data, c.len);
    return kFwChunkHeaderLen + c.len;
}

size_t store_fw_target(uint16_t target, uint8_t* out, size_t cap) {
    if (cap < kFwTargetLen) return 0;
    wr16(out, target);
    return kFwTargetLen;
}

size_t store_fw_reply(const FwReply& r, uint8_t* out, size_t cap) {
    if (cap < kFwReplyLen) return 0;
    wr16(out, static_cast<uint16_t>(r.status));
    wr16(out + 2, r.node);
    wr32(out + 4, r.received);
    wr32(out + 8, r.running);
    wr32(out + 12, r.floor);
    out[16] = static_cast<uint8_t>(r.state);
    std::memcpy(out + 17, r.version, kFwVersionLen);
    return kFwReplyLen;
}

bool load_fw_begin(const uint8_t* p, size_t len, FwBegin& out) {
    if (p == nullptr || len != kFwBeginLen) return false;
    out.target = rd16(p);
    out.image_len = rd32(p + 2);
    out.counter = rd32(p + 6);
    out.trailer = p + 10;
    return true;
}

bool load_fw_chunk(const uint8_t* p, size_t len, FwChunk& out) {
    if (p == nullptr || len < kFwChunkHeaderLen) return false;
    out.target = rd16(p);
    out.offset = rd32(p + 2);
    out.len = rd16(p + 6);
    if (out.len > kFwChunkMax || len != kFwChunkHeaderLen + out.len) return false;
    out.data = p + kFwChunkHeaderLen;
    return true;
}

bool load_fw_target(const uint8_t* p, size_t len, uint16_t& target) {
    if (p == nullptr || len != kFwTargetLen) return false;
    target = rd16(p);
    return true;
}

bool load_fw_reply(const uint8_t* p, size_t len, FwReply& out) {
    if (p == nullptr || len != kFwReplyLen) return false;
    out.status = static_cast<FwStatus>(rd16(p));
    out.node = rd16(p + 2);
    out.received = rd32(p + 4);
    out.running = rd32(p + 8);
    out.floor = rd32(p + 12);
    out.state = static_cast<FwState>(p[16]);
    std::memcpy(out.version, p + 17, kFwVersionLen);
    out.version[kFwVersionLen - 1] = 0;
    return true;
}

FwStatus FwReceiver::begin(const FwBegin& b) {
    if (busy_) return FwStatus::Busy;
    if (b.counter <= floor_) return FwStatus::Downgrade;
    if (b.image_len == 0 || b.image_len > sink_.capacity()) return FwStatus::TooLarge;
    if (active_) sink_.abort();  // a new BEGIN replaces an unfinished transfer
    active_ = false;
    if (!sink_.begin(b.image_len)) return FwStatus::StoreFailed;
    active_ = true;
    image_len_ = b.image_len;
    counter_ = b.counter;
    received_ = 0;
    std::memcpy(trailer_, b.trailer, kFwTrailerLen);
    crypto_sha512_init(&sha_);
    return FwStatus::Ok;
}

FwStatus FwReceiver::chunk(const FwChunk& c) {
    if (!active_) return FwStatus::NotStarted;
    if (c.offset == received_ - c.len && c.len > 0 && received_ >= c.len) {
        return FwStatus::Ok;  // a retransmission of the chunk just taken: its REPLY was lost
    }
    if (c.offset != received_) return FwStatus::BadOffset;
    if (static_cast<uint64_t>(received_) + c.len > image_len_) return FwStatus::TooLarge;
    if (!sink_.write(c.data, c.len)) {
        abort();
        return FwStatus::StoreFailed;
    }
    crypto_sha512_update(&sha_, c.data, c.len);
    received_ += c.len;
    return FwStatus::Ok;
}

FwStatus FwReceiver::commit() {
    if (!active_) return FwStatus::NotStarted;
    if (received_ != image_len_) return FwStatus::Incomplete;
    uint8_t digest[64];
    crypto_sha512_final(&sha_, digest);
    active_ = false;
    if (verify_ == nullptr || !verify_(vctx_, counter_, image_len_, digest, trailer_)) {
        sink_.abort();
        return FwStatus::BadSignature;
    }
    if (!sink_.finish()) {
        sink_.abort();
        return FwStatus::BadImage;
    }
    if (!sink_.activate()) return FwStatus::StoreFailed;
    return FwStatus::Ok;
}

void FwReceiver::abort() {
    if (active_) sink_.abort();
    active_ = false;
    received_ = 0;
}

}  // namespace pot
