// M28: a board asks the PC to play a short pattern of notes -- a host's named service (section 7.5, svc/sound), called
// once per request, as speak asks for speech. Pinned to one board, it declares two resources of its own:
//
//   act/<name>/play     an i32 written from inside or outside the cell: 0 stops, 1..8 plays one of the hub's patterns
//   act/<name>/result   a u32: 0 not asked yet, 1 playing (the PC took it), 2 refused (no such pattern, or the service
//                       does not serve the path), 3 gone (the provider is not alive, died with the call in flight, or
//                       was silent past 5 s)
//
// It may also follow another resource (config `source`, e.g. an alarm guest's output): when its value changes, 0
// stops and any other value plays the actor's `pattern` (or, with pattern 0, the source's own value). The source is
// read about once a second and refreshed from its owner, as any reader does (DEVELOPING-APPS section 4). A request
// while a call is out waits for the answer, and only the newest waits. The result is republished every 2 s so a copy
// on another board is read again (speak's lesson). Host loss (section 8.3) is a published 3, never a wait. What it
// plays is a notice, never anyone's only smoke, fire or gas alarm (section 12).
//
// Portable, like speak: no ESP-IDF, no heap, called under the node's lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint32_t kToneNotYet = 0, kTonePlaying = 1, kToneRefused = 2, kToneGone = 3;
constexpr uint32_t kToneDeadlineMs = 5000;  // the hub answers at once and plays on a thread of its own
constexpr uint32_t kToneSourcePeriodMs = 1000;

class ToneActor : public Actor {
  public:
    ToneActor(Node& node, const ToneConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    struct Stats {
        uint32_t asked;     // requests: writes of act/<name>/play and changes of the source
        uint32_t calls;     // CALLs sent
        uint32_t playing, refused, gone;
        uint32_t replaced;  // a waiting request a newer one replaced
    };
    const Stats& stats() const { return stats_; }
    uint32_t result() const { return result_; }

  private:
    void ask(int32_t pattern, uint32_t now_ms);
    void finish(uint32_t result, uint32_t now_ms);
    void follow(uint32_t now_ms);
    Node& node_;
    ToneConfig cfg_;
    Stats stats_{};
    uint32_t result_ = kToneNotYet;
    uint32_t play_count_ = 0;
    uint16_t in_flight_ = 0;
    uint16_t cancelling_ = 0;
    uint32_t in_flight_since_ = 0;
    bool has_waiting_ = false;
    int32_t waiting_ = 0;
    bool has_source_ = false;
    int32_t source_ = 0;
    uint32_t next_source_ms_ = 0;
    uint32_t published_at_ = 0;
    uint32_t now_ = 0;
};

bool tone_check(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t tone_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kToneKind;

}  // namespace pot
