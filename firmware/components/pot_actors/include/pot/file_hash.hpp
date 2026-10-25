// M25: the bulk channel's test app (pot/bulk.hpp). Pinned to one board; when act/<name>/go is written it asks the PC for
// its file and hashes what arrives with SHA-512 (the hash the firmware already carries for its images). Resources:
//
//   act/<name>/go      a u32 written to start a read (every write is one request; one read at a time)
//   act/<name>/bytes   a u32: bytes received in the current read
//   act/<name>/state   a u32: 0 idle, 1 reading, 2 done, 3 failed (no such file, the PC gone, or 10 s without a chunk)
//   act/<name>/digest  a u64: the SHA-512's first eight bytes, big-endian, when done (0 before)
//   act/<name>/ms      a u32: from the request to the last byte
//
// Portable: no ESP-IDF, no heap, called under the node's lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "monocypher-ed25519.h"
#include "pot/actor.hpp"
#include "pot/bulk.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint32_t kFileIdle = 0, kFileReading = 1, kFileDone = 2, kFileFailed = 3;
constexpr uint32_t kFileQuietMs = 10000;  // a read with no chunk for this long has failed

class FileHashActor : public Actor {
  public:
    FileHashActor(Node& node, const FileHashConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v) override;
    int on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args, uint16_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    uint32_t state() const { return state_; }
    uint32_t received() const { return got_; }
    uint64_t digest() const { return digest_; }

  private:
    void set_state(uint32_t s, uint32_t now_ms);
    void publish(uint32_t now_ms);
    Node& node_;
    FileHashConfig cfg_;
    crypto_sha512_ctx sha_{};
    uint32_t state_ = kFileIdle;
    uint32_t size_ = 0, got_ = 0;
    uint64_t digest_ = 0;
    uint32_t started_ms_ = 0, elapsed_ms_ = 0, last_chunk_ms_ = 0, published_at_ = 0, now_ = 0;
    uint32_t go_count_ = 0;
    uint16_t asking_ = 0;  // the open request's msg_id
    uint16_t xfer_ = 0;    // the transfer being read: taken from its first chunk
    uint32_t chunks_ = 0, out_of_order_ = 0;
};

bool file_hash_check(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t file_hash_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kFileHashKind;

}  // namespace pot
