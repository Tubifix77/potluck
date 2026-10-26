// M23: a board asks the PC to say something -- a host's named service (section 7.5), called once per request rather
// than polled (svc_client polls). Pinned to one board, it declares two resources of its own:
//
//   act/<name>/say      a u32 written from inside or outside the cell; every write is one request (its value is not
//                       read), so asking twice says the phrase twice
//   act/<name>/result   a u32: 0 not asked yet, 1 done, 2 refused (the service said no, or does not serve the path),
//                       3 gone (the provider is not alive, died with the call in flight, or was silent past 30 s)
//
// The phrase is the actor's own (its config, 1..120 bytes, sent as the CALL's arguments): what a board says is the
// package's choice, never text arriving from outside. Host loss (section 8.3) is a published 3, never a wait.
//
// Portable, like svc_client: no ESP-IDF, no heap, called under the node's lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint32_t kSpeakNotYet = 0, kSpeakDone = 1, kSpeakRefused = 2, kSpeakGone = 3;
constexpr uint32_t kSpeakDeadlineMs = 30000;  // speaking a 120-byte phrase takes ~10 s; a PC silent past this is gone

class SpeakActor : public Actor {
  public:
    SpeakActor(Node& node, const SpeakConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    struct Stats {
        uint32_t asked;    // writes of act/<name>/say
        uint32_t calls;    // CALLs sent
        uint32_t done, refused, gone;
        uint32_t busy;     // a request while a call was out: counted, not queued
    };
    const Stats& stats() const { return stats_; }
    uint32_t result() const { return result_; }

  private:
    void finish(uint32_t result, uint32_t now_ms);
    Node& node_;
    SpeakConfig cfg_;
    Stats stats_{};
    uint32_t result_ = kSpeakNotYet;
    uint32_t ctl_count_ = 0;
    uint16_t in_flight_ = 0;
    uint16_t cancelling_ = 0;
    uint32_t in_flight_since_ = 0;
    uint32_t now_ = 0;
    uint32_t published_at_ = 0;
};

bool speak_check(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t speak_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kSpeakKind;

}  // namespace pot
