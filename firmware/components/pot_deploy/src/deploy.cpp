#include "pot/deploy.hpp"

#include <cstring>

namespace pot {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

// The common CRC-32 (reflected 0xEDB88320, init and final xor all-ones): the same value as
// Python's zlib.crc32, so the host computes it without a second implementation to keep in step.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t c = ~seed;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

bool parse_image(const uint8_t* data, size_t len, DeployImage& out, const char** why) {
    const char* dummy = nullptr;
    const char*& w = (why != nullptr) ? *why : dummy;
    if (data == nullptr || len < kImageHeaderLen) {
        w = "shorter than the image header";
        return false;
    }
    if (len > kMaxImageLen) {
        w = "larger than kMaxImageLen";
        return false;
    }
    if (rd32(data) != kImageMagic) {
        w = "bad magic";
        return false;
    }
    if (data[4] != kImageVersion) {
        w = "unknown image version";
        return false;
    }
    const uint8_t count = data[5];
    if (count > kMaxActors) {
        w = "more actors than kMaxActors";
        return false;
    }
    const uint32_t body_len = rd32(data + 20);
    if (body_len != len - kImageHeaderLen) {
        w = "body_len disagrees with the image length";
        return false;
    }
    out.rollback_counter = rd32(data + 8);
    std::memcpy(out.package_digest, data + 12, sizeof(out.package_digest));
    out.actor_count = count;

    size_t at = kImageHeaderLen;
    for (uint8_t i = 0; i < count; ++i) {
        if (len - at < 4) {
            w = "actor declaration truncated";
            return false;
        }
        ActorDecl& a = out.actors[i];
        a.node_id = rd16(data + at);
        a.type = static_cast<ActorType>(data[at + 2]);
        a.cfg_len = data[at + 3];
        at += 4;
        if (len - at < a.cfg_len) {
            w = "actor config truncated";
            return false;
        }
        a.cfg = data + at;
        at += a.cfg_len;
        switch (a.type) {
            case ActorType::Led:
                if (a.cfg_len != kLedCfgLen) {
                    w = "led config has the wrong length";
                    return false;
                }
                break;
            case ActorType::Fault:
                if (a.cfg_len != kFaultCfgLen) {
                    w = "fault config has the wrong length";
                    return false;
                }
                break;
            default:
                w = "unknown actor type";
                return false;
        }
    }
    if (at != len) {
        w = "trailing bytes after the last actor";
        return false;
    }
    return true;
}

bool led_config(const ActorDecl& a, LedConfig& out) {
    if (a.type != ActorType::Led || a.cfg_len != kLedCfgLen) {
        return false;
    }
    out.r = a.cfg[0];
    out.g = a.cfg[1];
    out.b = a.cfg[2];
    out.blink = a.cfg[3];
    out.period_ms = rd16(a.cfg + 4);
    if (out.blink > 1 || out.period_ms < 200 || out.period_ms > 10000) {
        return false;
    }
    return true;
}

bool fault_config(const ActorDecl& a, FaultConfig& out) {
    if (a.type != ActorType::Fault || a.cfg_len != kFaultCfgLen) {
        return false;
    }
    out.panic_after_ms = rd32(a.cfg);
    return out.panic_after_ms > 0;
}

// ---------------------------------------------------------------------------------------------

const char* boot_outcome_str(BootOutcome o) {
    switch (o) {
        case BootOutcome::NoDeployment: return "no_deployment";
        case BootOutcome::Confirmed: return "confirmed";
        case BootOutcome::Trial: return "trial";
        case BootOutcome::Reverted: return "reverted";
    }
    return "?";
}

BootOutcome decide_boot(DeployState& s, uint8_t max_trial_boots) {
    if (s.active == kSlotNone) {
        return BootOutcome::NoDeployment;
    }
    if (s.pending == 0) {
        return BootOutcome::Confirmed;
    }
    if (s.trial_boots >= max_trial_boots) {
        revert_now(s);
        return BootOutcome::Reverted;
    }
    ++s.trial_boots;
    return BootOutcome::Trial;
}

void revert_now(DeployState& s) {
    s.active = s.previous;
    s.previous = kSlotNone;
    s.pending = 0;
    s.trial_boots = 0;
}

void confirm(DeployState& s) {
    s.pending = 0;
    s.trial_boots = 0;
}

// ---------------------------------------------------------------------------------------------

const char* deploy_status_str(DeployStatus s) {
    switch (s) {
        case DeployStatus::Ok: return "OK";
        case DeployStatus::TooLarge: return "TOO_LARGE";
        case DeployStatus::Downgrade: return "DOWNGRADE";
        case DeployStatus::NotStarted: return "NOT_STARTED";
        case DeployStatus::BadOffset: return "BAD_OFFSET";
        case DeployStatus::CrcMismatch: return "CRC_MISMATCH";
        case DeployStatus::BadImage: return "BAD_IMAGE";
        case DeployStatus::TrialInProgress: return "TRIAL_IN_PROGRESS";
        case DeployStatus::StoreFailed: return "STORE_FAILED";
        case DeployStatus::Malformed: return "MALFORMED";
        case DeployStatus::Busy: return "BUSY";
        case DeployStatus::Unsigned: return "UNSIGNED";
        case DeployStatus::BadSignature: return "BAD_SIGNATURE";
    }
    return "?";
}

bool load_deploy_begin(const uint8_t* p, size_t len, DeployBegin& out) {
    if (len < kDeployBeginLen) {
        return false;
    }
    out.image_len = rd32(p);
    out.image_crc = rd32(p + 4);
    out.rollback_counter = rd32(p + 8);
    std::memcpy(out.package_digest, p + 12, sizeof(out.package_digest));
    out.flags = p[20];
    return true;
}

bool load_deploy_chunk(const uint8_t* p, size_t len, DeployChunk& out) {
    if (len < kDeployChunkHeaderLen) {
        return false;
    }
    out.offset = rd32(p);
    out.len = rd16(p + 4);
    if (len - kDeployChunkHeaderLen < out.len || out.len > kDeployChunkMax) {
        return false;
    }
    out.data = p + kDeployChunkHeaderLen;
    return true;
}

bool load_deploy_commit(const uint8_t* p, size_t len, uint32_t& crc) {
    if (len < kDeployCommitLen) {
        return false;
    }
    crc = rd32(p);
    return true;
}

size_t store_deploy_begin(const DeployBegin& b, uint8_t* out, size_t cap) {
    if (cap < kDeployBeginLen) {
        return 0;
    }
    std::memset(out, 0, kDeployBeginLen);
    wr32(out, b.image_len);
    wr32(out + 4, b.image_crc);
    wr32(out + 8, b.rollback_counter);
    std::memcpy(out + 12, b.package_digest, sizeof(b.package_digest));
    out[20] = b.flags;
    return kDeployBeginLen;
}

size_t store_deploy_chunk(uint32_t offset, const uint8_t* data, uint16_t len, uint8_t* out, size_t cap) {
    if (len > kDeployChunkMax || cap < kDeployChunkHeaderLen + len) {
        return 0;
    }
    wr32(out, offset);
    wr16(out + 4, len);
    std::memcpy(out + kDeployChunkHeaderLen, data, len);
    return kDeployChunkHeaderLen + len;
}

size_t store_deploy_commit(uint32_t crc, uint8_t* out, size_t cap) {
    if (cap < kDeployCommitLen) {
        return 0;
    }
    wr32(out, crc);
    return kDeployCommitLen;
}

size_t store_deploy_reply(const DeployReply& r, uint8_t* out, size_t cap) {
    if (cap < kDeployReplyLen) {
        return 0;
    }
    std::memset(out, 0, kDeployReplyLen);
    wr16(out, static_cast<uint16_t>(r.status));
    out[2] = r.slot;
    out[3] = r.pending;
    wr32(out + 4, r.received);
    wr32(out + 8, r.counter);
    out[12] = r.peers_ok;
    out[13] = r.peers_failed;
    return kDeployReplyLen;
}

bool load_deploy_reply(const uint8_t* p, size_t len, DeployReply& out) {
    if (len < kDeployReplyLen) {
        return false;
    }
    out.status = static_cast<DeployStatus>(rd16(p));
    out.slot = p[2];
    out.pending = p[3];
    out.received = rd32(p + 4);
    out.counter = rd32(p + 8);
    out.peers_ok = p[12];
    out.peers_failed = p[13];
    return true;
}

// ---------------------------------------------------------------------------------------------

DeployReply DeployReceiver::reply(DeployStatus s) const {
    DeployReply r{};
    r.status = s;
    r.slot = target_;
    r.pending = committed_ ? 1 : 0;
    r.received = static_cast<uint32_t>(received_);
    r.counter = begin_.rollback_counter;
    return r;
}

DeployReply DeployReceiver::begin(const DeployBegin& b) {
    started_ = false;
    committed_ = false;
    received_ = 0;
    begin_ = b;
    DeployState s{};
    store_.load_state(s);
    if (s.pending != 0) {
        // Writing the inactive slot now would overwrite the trial's only fallback.
        return reply(DeployStatus::TrialInProgress);
    }
    if (b.rollback_counter < s.counter) {
        return reply(DeployStatus::Downgrade);
    }
    if (b.image_len == 0 || b.image_len > cap_ || b.image_len > kMaxImageLen) {
        return reply(DeployStatus::TooLarge);
    }
    target_ = (s.active == kSlotA) ? kSlotB : kSlotA;
    started_ = true;
    return reply(DeployStatus::Ok);
}

DeployReply DeployReceiver::chunk(const DeployChunk& c) {
    if (!started_) {
        return reply(DeployStatus::NotStarted);
    }
    if (c.offset != received_) {
        return reply(DeployStatus::BadOffset);
    }
    if (received_ + c.len > begin_.image_len) {
        return reply(DeployStatus::TooLarge);
    }
    std::memcpy(buf_ + received_, c.data, c.len);
    received_ += c.len;
    return reply(DeployStatus::Ok);
}

DeployReply DeployReceiver::commit(uint32_t crc) {
    if (!started_) {
        return reply(DeployStatus::NotStarted);
    }
    if (received_ != begin_.image_len) {
        return reply(DeployStatus::BadOffset);
    }
    if (crc != begin_.image_crc || crc32(buf_, received_) != crc) {
        return reply(DeployStatus::CrcMismatch);
    }
    // The image's own header says where it ends; anything after it is the M5 signature trailer.
    if (received_ < kImageHeaderLen) {
        return reply(DeployStatus::BadImage);
    }
    const uint32_t body = static_cast<uint32_t>(buf_[20]) | (static_cast<uint32_t>(buf_[21]) << 8) |
                          (static_cast<uint32_t>(buf_[22]) << 16) | (static_cast<uint32_t>(buf_[23]) << 24);
    if (body > received_ - kImageHeaderLen) {
        return reply(DeployStatus::BadImage);
    }
    const size_t image_len = kImageHeaderLen + body;
    if (verifier_ != nullptr) {
        const DeployStatus v = verifier_(verifier_ctx_, buf_, image_len, buf_ + image_len, received_ - image_len);
        if (v != DeployStatus::Ok) {
            return reply(v);
        }
    }
    DeployImage img{};
    if (!parse_image(buf_, image_len, img, nullptr) || img.rollback_counter != begin_.rollback_counter) {
        return reply(DeployStatus::BadImage);
    }
    DeployState s{};
    store_.load_state(s);
    if (s.pending != 0) {
        return reply(DeployStatus::TrialInProgress);
    }
    if (!store_.write_slot(target_, buf_, image_len)) {
        return reply(DeployStatus::StoreFailed);
    }
    s.previous = s.active;  // confirmed, since a pending one was refused above
    s.active = target_;
    s.pending = 1;
    s.trial_boots = 0;
    if (begin_.rollback_counter > s.counter) {
        s.counter = begin_.rollback_counter;
    }
    if (!store_.save_state(s)) {
        return reply(DeployStatus::StoreFailed);
    }
    started_ = false;
    committed_ = true;
    return reply(DeployStatus::Ok);
}

void DeployReceiver::abort() {
    started_ = false;
    committed_ = false;
    received_ = 0;
}

}  // namespace pot
