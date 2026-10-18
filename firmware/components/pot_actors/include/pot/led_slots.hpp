// The status LED's application states, one slot per app (BoardServices::led_app is slot 0, led_claim names one).
//
// Found 2026-10-18 on the owner's cell: B ran two actors that show a state on the LED -- the LED switch (blue while on,
// refreshed every 2 s) and passive-sensor's presence indicator (off while the room is empty, every window). With one
// shared state the last caller won, so the LED alternated blue and green every few seconds. Now each app has its own
// slot: an "off" clears only its own, and the LED shows the most recent "on" that has not lapsed. Pure logic.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

class LedSlots {
  public:
    static constexpr size_t kSlots = 4;
    static constexpr uint32_t kLapseMs = 5000;  // a state not called again for this long is gone

    struct State {
        bool on;
        uint8_t r, g, b;
        uint16_t period_ms;
        uint32_t set_ms;    // when it was last called: the newest "on" wins
        uint32_t until_ms;
    };

    void set(size_t slot, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms, uint32_t now_ms) {
        if (slot >= kSlots) return;
        slots_[slot] = State{on, r, g, b, period_ms, now_ms, now_ms + kLapseMs};
    }

    // The state to show now, or null for none: among the slots that are on and not lapsed, the one set most recently.
    const State* current(uint32_t now_ms) const {
        const State* best = nullptr;
        for (const State& s : slots_) {
            if (!s.on || static_cast<int32_t>(s.until_ms - now_ms) <= 0) continue;
            if (best == nullptr || static_cast<int32_t>(s.set_ms - best->set_ms) > 0) best = &s;
        }
        return best;
    }

  private:
    State slots_[kSlots]{};
};

}  // namespace pot
