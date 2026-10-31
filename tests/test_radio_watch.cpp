// A board that powers on with a radio that sends but is hardly heard (pot/radio_watch.hpp), found on B 2026-10-31.

#include "pot/radio_watch.hpp"
#include "test_harness.hpp"

using namespace pot;

TEST(radio_watch, the_stalled_b_restarts_after_one_window) {
    // B after the power-on: ~6 sends a second, about 5 % acknowledged.
    RadioWatch w(true);
    uint32_t ok = 0, fail = 0;
    bool restart = false;
    uint32_t at = 0;
    for (uint32_t t = 1000; t <= 120000 && !restart; t += 1000) {
        ok += (t % 20000 == 0) ? 6 : 0;
        fail += 6;
        restart = w.tick(t, ok, fail);
        at = t;
    }
    CHECK(restart);
    CHECK(at <= 1000 + RadioWatch::kWindowMs);
    CHECK(w.last_fail() > 150u);
    CHECK(!w.tick(at + 1000, ok, fail + 100));  // once: the restart is a software reset, and it is spent
}

TEST(radio_watch, a_healthy_or_quiet_or_soft_reset_board_never_restarts) {
    RadioWatch good(true), quiet(true), soft(false);
    uint32_t ok = 0, fail = 0;
    for (uint32_t t = 1000; t < 2 * RadioWatch::kWatchMs; t += 1000) {
        ok += 9;
        fail += 1;  // 90 %: a noisy house, not a stalled radio
        CHECK(!good.tick(t, ok, fail));
        CHECK(!quiet.tick(t, 0, t / 1000 % 2));  // alone: hardly anything sent
        CHECK(!soft.tick(t, 0, t));              // every send failing, but not after a power-on: tx_dead's
    }
}

TEST(radio_watch, it_watches_only_the_first_five_minutes) {
    RadioWatch w(true);
    uint32_t ok = 0, fail = 0;
    for (uint32_t t = 1000; t < RadioWatch::kWatchMs; t += 1000) {
        ok += 9;
        CHECK(!w.tick(t, ok, fail));
    }
    for (uint32_t t = RadioWatch::kWatchMs; t < 2 * RadioWatch::kWatchMs; t += 1000) {
        fail += 10;  // a stall that starts later is not the power-on one
        CHECK(!w.tick(t, ok, fail));
    }
}
