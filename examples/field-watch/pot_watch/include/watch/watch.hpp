// pot_watch: the field watch -- an alarm when two of three independent senses agree that someone is there.
//
// ext:watch/two_of_three (type 0xA0, portable): every period it reads three inputs anywhere in the cell -- a
// presence value (passive-sensor's radio presence, say), a camera's motion (one or two paths; the larger counts) and a
// sound level (dBFS) -- and holds each one "on" for `hold_ms` after it was last over its threshold, so senses that
// fire a few seconds apart still agree. It publishes, under the `outputs` prefix (potluck://<system>/act/<name>/):
//
//   out      bool  the alarm: at least two of the three on
//   agree    u32   which are on: 1 presence, 2 motion, 4 sound
//   missing  u32   which inputs are not GOOD right now (STALE, NO_DATA, not yet declared), the same bits; a missing
//                  input counts as off, so a dead tablet cannot raise the alarm and cannot hold it up either
//
// While the alarm is on, with `led` set, the status LED of the board running it blinks cyan fast (Potluck draws an
// app's colour blue or cyan only: red is the board's own "a link is dead"). The LED follows the watch when it moves:
// the board it left drops its claim at once, the new one lights its own. What it raises is a notice, never anyone's
// only alarm (ARCHITECTURE.md section 12).
//
// Config: Potluck's portable header (out_hash = act/<name>/out, period_ms, the eligible nodes), then in
// potluck-modules.json's order: outputs u32 (the prefix's hash state), presence u32, presence_min f32, motion u32,
// motion_b u32 (the same as motion for one camera), motion_min f32, sound u32, sound_min f32, hold_ms u16, led u8.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {
namespace watch {

constexpr ActorType kWatchType = static_cast<ActorType>(0xA0);
constexpr size_t kOwnCfgLen = 35;  // 8 x 4 + 2 + 1: with a three-board header, 51 -- a board with PSRAM keeps 160
constexpr uint32_t kPresence = 1, kMotion = 2, kSound = 4;
constexpr uint8_t kLedSlot = 2;              // led_claim's slot: 1 is the LED switch's
constexpr uint16_t kLedBlinkMs = 300;
constexpr uint32_t kRepublishMs = 2000;     // fresh between changes, so a copy on another board is read again

struct Config {
    TickerConfig place{};  // the portable header: out_hash (the alarm), period_ms, eligible nodes
    uint32_t agree_hash = 0, missing_hash = 0;
    uint32_t presence = 0, motion = 0, motion_b = 0, sound = 0;
    float presence_min = 0, motion_min = 0, sound_min = 0;
    uint16_t hold_ms = 0;
    bool led = false;
};

// False if the declaration is not this actor's or its bytes are out of range.
bool parse(const ActorDecl& d, Config& out);

class WatchActor : public Actor {
  public:
    WatchActor(Node& node, const BoardServices* board, const Config& cfg) : node_(node), board_(board), cfg_(cfg) {}
    ~WatchActor() override;
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    uint32_t agree() const { return agree_; }
    uint32_t missing() const { return missing_; }
    bool alarm() const { return alarm_; }
    uint32_t alarms() const { return alarms_; }

  private:
    // The input's value as a number, refreshed from its owner as any reader does. False if it is not GOOD.
    bool sample(uint32_t hash, float& out);
    void led(bool on);

    Node& node_;
    const BoardServices* board_;
    Config cfg_;
    uint32_t next_ms_ = 0;
    uint32_t published_at_ = 0;
    bool published_ = false;
    uint32_t last_on_[3] = {};  // when each input was last over its threshold
    bool seen_[3] = {};
    uint32_t agree_ = 0, missing_ = 0;
    bool alarm_ = false;
    bool led_on_ = false;
    uint32_t alarms_ = 0;   // times the alarm went on
};

// The registration row: check, create, the portable placement hook, the outputs hook.
ActorKind watch_row();

}  // namespace watch

// The hook Potluck's firmware build calls for this component (generated from the directory's name).
const ActorKind* app_rows_pot_watch(size_t* n);

}  // namespace pot
