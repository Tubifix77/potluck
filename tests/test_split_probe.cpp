// A station split from part of its cell by a mesh's two channels (pot/split_probe.hpp), found 2026-10-18.

#include "pot/split_probe.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {
constexpr uint16_t kCh1And11 = (1u << 1) | (1u << 11);

SplitProbe::Inputs at(uint32_t t, uint8_t ch, uint8_t missing, uint8_t alive, bool assoc = true, bool outranked = false,
                      uint16_t seen = kCh1And11) {
    return SplitProbe::Inputs{t, assoc, ch, seen, missing, alive, outranked};
}
}  // namespace

TEST(split_probe, the_owners_split_c_finds_b_on_the_other_channel_and_stays) {
    // C on 1 with A, B (the extender, lower id) on 11: C misses B.
    SplitProbe p;
    CHECK(p.tick(at(1000, 1, 1, 1)) == SplitProbe::Act::None);
    CHECK(p.tick(at(1000 + SplitProbe::kSplitMs - 1, 1, 1, 1)) == SplitProbe::Act::None);  // not yet a minute
    CHECK(p.tick(at(1000 + SplitProbe::kSplitMs, 1, 1, 1)) == SplitProbe::Act::Probe);
    CHECK_EQ(p.target(), static_cast<uint8_t>(11));
    uint32_t t = 1000 + SplitProbe::kSplitMs;
    CHECK(p.tick(at(t + 3000, 11, 2, 0)) == SplitProbe::Act::None);  // landed: A left behind, B not yet heard
    // B heard, and it outranks C: the channel the cell follows anyway. Judged after the dwell.
    CHECK(p.tick(at(t + 3000 + SplitProbe::kDwellMs - 1, 11, 1, 1, true, true)) == SplitProbe::Act::None);
    CHECK(p.tick(at(t + 3000 + SplitProbe::kDwellMs, 11, 1, 1, true, true)) == SplitProbe::Act::None);
    CHECK(!p.probing());
    CHECK_EQ(p.counters().stayed, 1u);
    // A sweeps and finds them: nobody missing, nothing more to do.
    CHECK(p.tick(at(t + 200000, 11, 0, 2)) == SplitProbe::Act::None);
    CHECK_EQ(p.counters().probes, 1u);
}

TEST(split_probe, nobody_there_goes_back_and_waits_longer_each_time) {
    SplitProbe p;
    uint32_t t = 5000;
    p.tick(at(t, 1, 1, 1));
    t += SplitProbe::kSplitMs;
    CHECK(p.tick(at(t, 1, 1, 1)) == SplitProbe::Act::Probe);
    CHECK(p.tick(at(t + 2000, 11, 2, 0)) == SplitProbe::Act::None);
    CHECK(p.tick(at(t + 2000 + SplitProbe::kDwellMs, 11, 2, 0)) == SplitProbe::Act::Return);
    CHECK_EQ(p.target(), static_cast<uint8_t>(1));
    t += 2000 + SplitProbe::kDwellMs;
    // Back on 1: still split, but not before the backoff.
    p.tick(at(t + 1000, 1, 1, 1));
    CHECK(p.tick(at(t + SplitProbe::kFirstBackoffMs - 1, 1, 1, 1)) == SplitProbe::Act::None);
    CHECK(p.tick(at(t + SplitProbe::kFirstBackoffMs + SplitProbe::kSplitMs, 1, 1, 1)) == SplitProbe::Act::Probe);
    const uint32_t t2 = t + SplitProbe::kFirstBackoffMs + SplitProbe::kSplitMs;
    CHECK(p.tick(at(t2 + SplitProbe::kLandMs, 1, 1, 1)) == SplitProbe::Act::Return);  // never associated there: back
    // The backoff doubled.
    p.tick(at(t2 + SplitProbe::kLandMs + 1000, 1, 1, 1));
    CHECK(p.tick(at(t2 + SplitProbe::kLandMs + 2 * SplitProbe::kFirstBackoffMs - 1, 1, 1, 1)) == SplitProbe::Act::None);
    CHECK_EQ(p.counters().returned, 2u);
}

TEST(split_probe, a_network_on_one_channel_or_no_association_or_nobody_missing_never_probes) {
    SplitProbe p;
    for (uint32_t t = 0; t < 10 * SplitProbe::kSplitMs; t += 1000) {
        CHECK(p.tick(at(t, 1, 1, 1, true, false, 1u << 1)) == SplitProbe::Act::None);  // heard on 1 only
    }
    SplitProbe q;
    for (uint32_t t = 0; t < 10 * SplitProbe::kSplitMs; t += 1000) {
        CHECK(q.tick(at(t, 1, 1, 1, false)) == SplitProbe::Act::None);  // hunting for its router: not split
        CHECK(q.tick(at(t, 1, 0, 2)) == SplitProbe::Act::None);         // the whole cell here
    }
    CHECK_EQ(p.counters().probes + q.counters().probes, 0u);
}

TEST(split_probe, an_extender_waits_three_times_as_long) {
    SplitProbe p;
    p.set_split_ms(3 * SplitProbe::kSplitMs);
    p.tick(at(1, 11, 2, 0));
    CHECK(p.tick(at(1 + SplitProbe::kSplitMs, 11, 2, 0)) == SplitProbe::Act::None);
    CHECK(p.tick(at(1 + 3 * SplitProbe::kSplitMs, 11, 2, 0)) == SplitProbe::Act::Probe);
    CHECK_EQ(p.target(), static_cast<uint8_t>(1));
}

TEST(split_probe, three_channels_are_tried_in_turn) {
    SplitProbe p;
    const uint16_t seen = (1u << 1) | (1u << 6) | (1u << 11);
    uint32_t t = 1;
    p.tick(at(t, 1, 1, 1, true, false, seen));
    t += SplitProbe::kSplitMs;
    CHECK(p.tick(at(t, 1, 1, 1, true, false, seen)) == SplitProbe::Act::Probe);
    CHECK_EQ(p.target(), static_cast<uint8_t>(6));
    t += SplitProbe::kLandMs;
    CHECK(p.tick(at(t, 1, 1, 1, true, false, seen)) == SplitProbe::Act::Return);
    t += SplitProbe::kFirstBackoffMs + SplitProbe::kSplitMs;
    p.tick(at(t - SplitProbe::kSplitMs, 1, 1, 1, true, false, seen));
    CHECK(p.tick(at(t, 1, 1, 1, true, false, seen)) == SplitProbe::Act::Probe);
    CHECK_EQ(p.target(), static_cast<uint8_t>(11));
}
