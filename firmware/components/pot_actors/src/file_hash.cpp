// M25: the bulk channel's test app. The behaviour is specified in file_hash.hpp, the wire in bulk.hpp.

#include "pot/file_hash.hpp"

#include <cstdio>
#include <new>

namespace pot {

namespace {
NsDecl file_decl(uint32_t hash, uint16_t node, ValueType type, Access access) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = node;
    d.type = type;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = access;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = access == Access::ReadWrite ? 0 : 10000;  // the request does not go stale; the rest is
    d.staleness_policy = StalenessPolicy::Informative;                // published at least every 2 s (tick)
    return d;
}
}  // namespace

bool FileHashActor::start(uint32_t now_ms) {
    const uint16_t self = node_.config().node_id;
    if (node_.ns().declare(file_decl(cfg_.go_hash, self, ValueType::U32, Access::ReadWrite)) != NsError::Ok) return false;
    const uint32_t u32s[3] = {cfg_.bytes_hash, cfg_.state_hash, cfg_.ms_hash};
    for (uint32_t h : u32s) {
        if (node_.ns().declare(file_decl(h, self, ValueType::U32, Access::Read)) != NsError::Ok) return false;
    }
    if (node_.ns().declare(file_decl(cfg_.digest_hash, self, ValueType::U64, Access::Read)) != NsError::Ok) return false;
    const NsEntry* go = node_.ns().find(cfg_.go_hash);
    if (go != nullptr && !go->has_data()) node_.ns().publish(cfg_.go_hash, Value::of_u32(0), now_ms);
    if (go != nullptr) go_count_ = go->update_count;  // a write before this start is not a request
    now_ = now_ms;
    publish(now_ms);
    return true;
}

void FileHashActor::publish(uint32_t now_ms) {
    node_.ns().publish(cfg_.bytes_hash, Value::of_u32(got_), now_ms);
    node_.ns().publish(cfg_.state_hash, Value::of_u32(state_), now_ms);
    node_.ns().publish(cfg_.digest_hash, Value::of_u64(digest_), now_ms);
    node_.ns().publish(cfg_.ms_hash, Value::of_u32(elapsed_ms_), now_ms);
    published_at_ = now_ms;
}

void FileHashActor::set_state(uint32_t s, uint32_t now_ms) {
    state_ = s;
    publish(now_ms);
}

void FileHashActor::tick(uint32_t now) {
    now_ = now;
    if (state_ == kFileReading && asking_ == 0 && now - last_chunk_ms_ >= kFileQuietMs) set_state(kFileFailed, now);
    const NsEntry* go = node_.ns().find(cfg_.go_hash);
    if (go != nullptr && go->update_count != go_count_) {
        go_count_ = go->update_count;
        if (state_ != kFileReading) {
            const PeerLink* p = node_.peers().find_by_node_id(cfg_.provider);
            uint8_t args[5 + kFileNameMax];
            args[0] = kBulkOpen;
            args[1] = static_cast<uint8_t>(cfg_.in_hash);
            args[2] = static_cast<uint8_t>(cfg_.in_hash >> 8);
            args[3] = static_cast<uint8_t>(cfg_.in_hash >> 16);
            args[4] = static_cast<uint8_t>(cfg_.in_hash >> 24);
            for (uint8_t i = 0; i < cfg_.name_len; ++i) args[5 + i] = cfg_.name[i];
            got_ = 0;
            digest_ = 0;
            elapsed_ms_ = 0;
            xfer_ = 0;
            started_ms_ = last_chunk_ms_ = now;
            asking_ = (p != nullptr && p->state == PeerState::Alive)
                          ? node_.request_call(cfg_.provider, cfg_.svc_hash, args, static_cast<uint16_t>(5 + cfg_.name_len))
                          : 0;
            crypto_sha512_init(&sha_);
            set_state(asking_ != 0 ? kFileReading : kFileFailed, now);
        }
    }
    if (now - published_at_ >= 2000) publish(now);  // fresh between changes, so copies elsewhere are read again
}

bool FileHashActor::on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                                   const Value& v) {
    if (path_hash != cfg_.svc_hash || from_node != cfg_.provider || msg_id == 0 || msg_id != asking_) return false;
    asking_ = 0;
    uint32_t size = kBulkNoFile;
    if (o != Node::CallOutcome::Ok || !v.as_u32(size) || size == kBulkNoFile || size > kBulkSizeMax) {
        set_state(kFileFailed, now_);
        return true;
    }
    size_ = size;
    last_chunk_ms_ = now_;  // the quiet clock starts from the answer
    if (size_ == 0) {
        uint8_t h[64];
        crypto_sha512_final(&sha_, h);
        digest_ = 0;
        for (int i = 0; i < 8; ++i) digest_ = (digest_ << 8) | h[i];
        set_state(kFileDone, now_);
    }
    return true;
}

int FileHashActor::on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args,
                           uint16_t len) {
    if (path_hash != cfg_.in_hash) return kCallNotMine;
    BulkChunk c{};
    if (!parse_bulk_chunk(args, len, c)) return kCallRefused;
    ++chunks_;
    if (state_ == kFileReading && asking_ == 0) {
        if (got_ == 0 && c.offset == 0) xfer_ = c.xfer;  // the transfer this read takes
        if (c.xfer == xfer_ && c.offset == got_ && got_ + c.len <= size_) {
            crypto_sha512_update(&sha_, c.data, c.len);
            got_ += c.len;
            last_chunk_ms_ = now_;
            if (got_ == size_) {
                uint8_t h[64];
                crypto_sha512_final(&sha_, h);
                digest_ = 0;
                for (int i = 0; i < 8; ++i) digest_ = (digest_ << 8) | h[i];
                elapsed_ms_ = now_ - started_ms_;
                set_state(kFileDone, now_);
            } else if (now_ - published_at_ >= 250) {
                publish(now_);
            }
        } else {
            ++out_of_order_;
        }
    }
    node_.reply_call(from_node, msg_id, path_hash, Value::of_u32(got_));  // what this read expects next
    return kCallAccepted;
}

size_t FileHashActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"file_hash\",\"node\":%u,\"state\":%u,\"size\":%u,\"bytes\":%u,\"chunks\":%u,"
                                "\"out_of_order\":%u,\"ms\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(state_),
                                static_cast<unsigned>(size_), static_cast<unsigned>(got_),
                                static_cast<unsigned>(chunks_), static_cast<unsigned>(out_of_order_),
                                static_cast<unsigned>(elapsed_ms_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- registration ---------------------------------------------------------------------------------------

bool file_hash_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    FileHashConfig c{};
    if (!file_hash_config(d, c)) {
        *why = "file_hash config invalid";
        return false;
    }
    return true;
}

size_t file_hash_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    FileHashConfig c{};
    if (peer != 0 || cap < 5 || node != d.node_id || !file_hash_config(d, c)) return 0;
    out[0] = file_decl(c.bytes_hash, node, ValueType::U32, Access::Read);
    out[1] = file_decl(c.state_hash, node, ValueType::U32, Access::Read);
    out[2] = file_decl(c.digest_hash, node, ValueType::U64, Access::Read);
    out[3] = file_decl(c.ms_hash, node, ValueType::U32, Access::Read);
    out[4] = file_decl(c.go_hash, node, ValueType::U32, Access::ReadWrite);
    return 5;
}

namespace {
Actor* file_hash_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(FileHashActor) <= kActorSlotBytes, "FileHashActor outgrew its actor slot");
    FileHashConfig c{};
    if (env.node == nullptr || !file_hash_config(d, c)) return nullptr;
    return new (mem) FileHashActor(*env.node, c);
}
}  // namespace

const ActorKind kFileHashKind = {ActorType::FileHash, "file_hash", &file_hash_check, &file_hash_create, nullptr,
                                 &file_hash_outputs};

}  // namespace pot
