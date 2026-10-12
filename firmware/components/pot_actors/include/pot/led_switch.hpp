// M13: a board's status LED as a writable resource -- the bench's stand-in for a lightbulb (ARCHITECTURE section 13,
// M13 item 4). Pinned to one board, it declares node-<id>/led/on (a bool, writable) and shows the colour its
// config names on the status LED while the value is true. The value is written through the namespace's WRITE --
// from inside the cell, or from outside by the presenting role when the package's outside section allows it.
//
// The LED rule is M8.2's PS-5 (BoardServices::led_app): an application's colour shows only while every link is
// healthy -- blue, yellow and red always win -- so no app, and no system outside the cell, hides a dead link.
//
// When its writer goes away (the presenter dies, Home Assistant stops) it holds the last value: an LED that
// stays on is not a hazard, and the package says so (`on_writer_loss: hold`, the only policy this actor offers;
// anything that moves the world declares its own, section 8.3).
//
//   config: out_hash u32 (the hash of potluck://<system>/node-<id>/led/on), r u8, g u8, b u8, blink_ds u8
//           (full blink period in tenths of a second; 0 = steady)

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint8_t kLedSwitchCfgLen = 8;

struct LedSwitchConfig {
    uint32_t out_hash;
    uint8_t r, g, b, blink_ds;
};
bool led_switch_config(const ActorDecl& a, LedSwitchConfig& out);
NsDecl led_switch_decl(uint16_t node, const LedSwitchConfig& c);

class LedSwitchActor : public Actor {
  public:
    LedSwitchActor(Node& node, const LedSwitchConfig& cfg, const BoardServices* board)
        : node_(node), cfg_(cfg), board_(board) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;
    ~LedSwitchActor() override;

    bool on() const { return on_; }
    uint32_t changes() const { return changes_; }

  private:
    Node& node_;
    LedSwitchConfig cfg_;
    const BoardServices* board_;
    bool on_ = false;
    uint32_t changes_ = 0;
    uint32_t last_count_ = 0;
    uint32_t next_ms_ = 0;
};

extern const ActorKind kLedSwitchKind;

}  // namespace pot
