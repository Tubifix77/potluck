// A host service, called from a node -- ARCHITECTURE.md section 7.5, milestone M8.
//
// "An MCU actor calls it exactly as it calls any other remote actor -- CALL to a path -- and must
// handle ERR_UNAVAILABLE because the service is on a machine that might be off." This is that actor,
// built in (ADR-003 Tier 0): every period it CALLs one service on one provider -- a host's
// potluck-agent -- and publishes the answer to a resource this node owns, so everything else reads the
// service's result through the namespace like any other value.
//
// Host loss (section 8.3) is a non-event by construction:
//   * a CALL is never sent to a provider membership does not count as alive; nothing waits on it;
//   * a CALL in flight when the provider dies is written off by the node (CallOutcome::Unavailable);
//   * a CALL to a provider that is alive but silent is cancelled after a deadline, so a hung service
//     cannot hold the client's one slot for ever;
//   * meanwhile the output keeps its last value with its true age, and past its staleness bound every
//     read says STALE -- the honest form of "hold";
//   * the first answer after the provider returns is published at once.
// What it never does: present an old answer as fresh, block, or retry faster than its period.
//
// Portable, like the node and the reconciler: no ESP-IDF, no heap, called under the node's lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

class SvcClient : public Actor {
  public:
    SvcClient(Node& node, const SvcClientConfig& cfg);

    // Declare the output resource (owned here, untyped: it carries whatever the service answers).
    bool start(uint32_t now_ms) override;

    // Call after every Node::tick, under the same lock.
    void tick(uint32_t now_ms) override;

    // The node's call-result hook hands every result here. True if it was this client's.
    // M8.1: the actor API's names for on_result and for the {"t":"svc"} stats line.
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome outcome,
                        const Value& v) override {
        return on_result(from_node, msg_id, path_hash, outcome, v);
    }
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    bool on_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome outcome,
                   const Value& v);

    enum class State : uint8_t {
        Waiting = 0,   // no answer yet since boot
        Serving = 1,   // the last call was answered
        Degraded = 2,  // the provider is gone, or did not answer: the output is ageing
    };

    struct Stats {
        uint32_t calls;          // CALLs sent
        uint32_t answered;       // answered with a value, and published
        uint32_t refused;        // answered with an error (the provider does not serve this path)
        uint32_t lost;           // written off: the provider died with the call in flight
        uint32_t timed_out;      // cancelled: the provider is alive and did not answer in time
        uint32_t not_sent;       // a period passed with the provider not alive: nothing sent
        uint32_t degradations;   // transitions into Degraded
        uint32_t recoveries;     // transitions out of Degraded
        uint32_t last_answer_ms; // our clock; 0 = never
    };
    const Stats& stats() const { return stats_; }
    State state() const { return state_; }
    const SvcClientConfig& config() const { return cfg_; }

  private:
    void degrade();

    Node& node_;
    SvcClientConfig cfg_;
    State state_ = State::Waiting;
    Stats stats_{};
    uint32_t next_call_ms_ = 0;
    uint16_t in_flight_ = 0;      // msg_id of the call outstanding; 0 = none
    uint32_t in_flight_since_ = 0;
    uint16_t cancelling_ = 0;     // the call being cancelled for a timeout, while cancel_call reports it
    uint32_t now_ = 0;            // the last tick's time: results arrive between ticks
    bool started_ = false;
};

// M8.1: svc_client's row for a registration table.
extern const ActorKind kSvcClientKind;

}  // namespace pot
