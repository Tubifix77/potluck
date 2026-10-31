// A board that powers on with a radio that sends but is hardly heard: when to restart it.
//
// Found on the owner's cell (B, 2026-10-09 and again 2026-10-31): after a power-on -- never after a software reset --
// B's ESP-NOW sends were accepted by the driver but 859 of 908 went unacknowledged, and it heard C at -68 dBm against
// -13 dBm before; it never rejoined the cell. A restart from USB cleared it every time. tx_dead (m0_main.cpp) restarts
// a board only when every send is refused, so it never fired. The cause is open; the remedy is the one that worked.
//
// So: for the first kWatchMs after a power-on, the delivery reports over each kWindowMs are counted, and a window with
// at least kMinAttempts reports of which fewer than kMinOkPct percent were acknowledged restarts the board -- once, since
// the restart is a software reset and the watch runs after a power-on only. A board powered on out of everyone's range
// costs one restart. Pure logic: the caller sums its radio peers' send callbacks and restarts.

#pragma once

#include <cstdint>

namespace pot {

class RadioWatch {
  public:
    static constexpr uint32_t kWatchMs = 300000;  // the first five minutes after a power-on
    static constexpr uint32_t kWindowMs = 30000;
    static constexpr uint32_t kMinAttempts = 60;  // ~2 a second to the cell's peers: heartbeats alone give more
    static constexpr uint32_t kMinOkPct = 25;     // the stalled B: 5 %; a board at the edge of range loses far less

    explicit RadioWatch(bool power_on) : armed_(power_on) {}

    // Once a second or so: the sums over the radio peers of their send callbacks, acknowledged and not.
    // True: restart now.
    bool tick(uint32_t now_ms, uint32_t cb_ok, uint32_t cb_fail) {
        if (!armed_) return false;
        if (now_ms >= kWatchMs) {
            armed_ = false;
            return false;
        }
        if (!started_) {
            started_ = true;
            window_at_ = now_ms;
            ok0_ = cb_ok;
            fail0_ = cb_fail;
            return false;
        }
        if (now_ms - window_at_ < kWindowMs) return false;
        const uint32_t ok = cb_ok - ok0_, fail = cb_fail - fail0_;
        window_at_ = now_ms;
        ok0_ = cb_ok;
        fail0_ = cb_fail;
        last_ok_ = ok;
        last_fail_ = fail;
        const uint32_t attempts = ok + fail;
        if (attempts < kMinAttempts || static_cast<uint64_t>(ok) * 100u >= static_cast<uint64_t>(attempts) * kMinOkPct) {
            return false;
        }
        armed_ = false;
        return true;
    }

    uint32_t last_ok() const { return last_ok_; }
    uint32_t last_fail() const { return last_fail_; }

  private:
    bool armed_;
    bool started_ = false;
    uint32_t window_at_ = 0;
    uint32_t ok0_ = 0, fail0_ = 0;
    uint32_t last_ok_ = 0, last_fail_ = 0;
};

}  // namespace pot
