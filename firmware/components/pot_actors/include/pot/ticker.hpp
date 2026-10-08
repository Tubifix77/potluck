// M6's ticker, as an actor (M8.1). The smallest actor that serves reads: every period_ms it
// publishes a u32 to its output -- this node's id in the high 16 bits, a count in the low 16 that
// starts at 1 in every activation (section 7.7: "re-activation restarts from the actor's declared
// initial state"), so a reader sees which instance answered and that it is a fresh one.
//
// It is portable: the reconciler (pot/reconcile.hpp) decides where it runs, creates an instance on
// each activation, ticks it only while this node is the output's fenced owner, and destroys it when
// it is fenced or hands over. The output resource is the reconciler's to declare, since ownership
// of it is what the claims decide.

#pragma once

#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

class TickerActor : public Actor {
  public:
    TickerActor(Node& node, const TickerConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override {
        next_ms_ = now_ms;
        return true;
    }
    void tick(uint32_t now_ms) override;
    uint32_t published() const { return count_; }

  private:
    Node& node_;
    TickerConfig cfg_;
    uint32_t next_ms_ = 0;
    uint32_t count_ = 0;
};

// The registration table's row: portable, with the placement hook.
extern const ActorKind kTickerKind;

}  // namespace pot
