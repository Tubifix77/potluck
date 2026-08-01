// Heartbeat state machine tests — ARCHITECTURE.md §8.2.
//
// The clock is injected, so 600 ms of silence costs nothing to test and the result does not depend
// on how busy the host is. Every case below is written in terms of §8.2's numbers rather than
// their arithmetic results, so that changing the specification makes the tests fail loudly rather
// than quietly agreeing with whatever the code now does.

#include "pot/membership.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

const uint8_t kMacA[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x01};
const uint8_t kMacB[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x02};

// A peer already alive at t=0, on the §8.2 wireless timers.
PeerLink wireless_peer(uint32_t now_ms = 0) {
    PeerLink p;
    p.reset();
    p.state = PeerState::Alive;
    p.last_rx_ms = now_ms;
    p.hb_period_ms = kHbPeriodWirelessMs;
    p.miss_limit = kHbMissLimitWireless;
    p.node_id = 2;
    return p;
}

}  // namespace

TEST(heartbeat, the_specification_numbers) {
    // §8.2, wireless row: 100 ms period, 6 misses, dead after 600 ms.
    CHECK_EQ(kHbPeriodWirelessMs, 100u);
    CHECK_EQ(static_cast<uint32_t>(kHbMissLimitWireless), 6u);
    CHECK_EQ(kHbDeadAfterWirelessMs, 600u);

    // §8.2's stated rationale, as assertions rather than prose: 600 ms is above L3's 500 ms
    // deadline so liveness and class cannot flap together, and it is roughly 6× the ~104 ms
    // in-protocol retry window (31 retransmissions × 3350 µs).
    CHECK(kHbDeadAfterWirelessMs > 500u);
    const uint32_t retry_window_ms = (31u * 3350u) / 1000u;  // §3: ~103 ms
    CHECK(kHbDeadAfterWirelessMs >= 5 * retry_window_ms);

    // And §8.2's other two rows, which M4 and M8 will need.
    CHECK_EQ(kHbPeriodWiredMs * kHbMissLimitWired, 60u);
    CHECK_EQ(kHbPeriodHostMs * kHbMissLimitHost, 1000u);
}

TEST(heartbeat, miss_counting_is_one_per_elapsed_period) {
    PeerLink p = wireless_peer(1000);

    // Inside the first period: nothing is missed yet.
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 1000)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(static_cast<uint32_t>(p.misses), 0u);
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 1099)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(static_cast<uint32_t>(p.misses), 0u);

    // Each further whole period adds exactly one miss, and none of them declare death.
    for (uint8_t n = 1; n < kHbMissLimitWireless; ++n) {
        const uint32_t t = 1000 + n * kHbPeriodWirelessMs;
        CHECK_EQ(static_cast<int>(peer_on_tick(p, t)), static_cast<int>(MembershipChange::None));
        CHECK_EQ(static_cast<uint32_t>(p.misses), static_cast<uint32_t>(n));
        CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));
    }
}

TEST(heartbeat, death_is_declared_at_exactly_600ms) {
    PeerLink p = wireless_peer(0);

    // 599 ms of silence: still alive. This is the assertion that matters — §8.2 chose 600 ms
    // deliberately, and a node that gives up at 599 would produce the false disconnects §8.3 is
    // written to prevent.
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 599)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));

    CHECK_EQ(static_cast<int>(peer_on_tick(p, 600)), static_cast<int>(MembershipChange::Dead));
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Dead));
    CHECK_EQ(static_cast<uint32_t>(p.misses), 6u);
}

TEST(heartbeat, death_is_declared_once_not_every_tick) {
    // A death declaration is an event (§4 rule 4: loud). Re-emitting it on every subsequent tick
    // would turn one failure into a flood, and the event ring holds 32 entries.
    PeerLink p = wireless_peer(0);
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 600)), static_cast<int>(MembershipChange::Dead));
    for (uint32_t t = 700; t <= 5000; t += 100) {
        CHECK_EQ(static_cast<int>(peer_on_tick(p, t)), static_cast<int>(MembershipChange::None));
    }
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Dead));
}

TEST(heartbeat, a_late_tick_cannot_undercount_misses) {
    // The state machine derives misses from elapsed time, not from how many times it was called.
    // A node whose tick task was starved for a second must still declare the peer dead — counting
    // ticks instead of time is how a busy node fails to notice a dead peer.
    PeerLink p = wireless_peer(0);
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 2000)), static_cast<int>(MembershipChange::Dead));
    CHECK_EQ(static_cast<uint32_t>(p.misses), 20u);
}

TEST(heartbeat, a_burst_of_ticks_cannot_overcount_misses) {
    // The mirror image: being called a hundred times inside one period must not age the peer.
    PeerLink p = wireless_peer(0);
    for (int i = 0; i < 100; ++i) {
        CHECK_EQ(static_cast<int>(peer_on_tick(p, 50)), static_cast<int>(MembershipChange::None));
    }
    CHECK_EQ(static_cast<uint32_t>(p.misses), 0u);
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));
}

TEST(heartbeat, revival_after_death) {
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 7;

    CHECK_EQ(static_cast<int>(peer_on_tick(p, 600)), static_cast<int>(MembershipChange::Dead));

    // Same incarnation returning: the link recovered.
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 1000, 7)),
             static_cast<int>(MembershipChange::Revived));
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));
    CHECK_EQ(static_cast<uint32_t>(p.misses), 0u);

    // And the death clock restarts from the revival, not from the original silence.
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 1599)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 1600)), static_cast<int>(MembershipChange::Dead));
}

TEST(heartbeat, reboot_is_distinguished_from_revival) {
    // A different boot_epoch means the peer restarted and lost its state. §7.7 will fence on
    // epochs, so conflating this with a recovered link would be a real bug later — and even at M0
    // it decides whether the peer's sequence numbers can still be trusted.
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 7;
    p.seq_rx_valid = true;
    p.seq_rx_last = 30000;
    p.hb_seq_last = 500;

    CHECK_EQ(static_cast<int>(peer_on_tick(p, 600)), static_cast<int>(MembershipChange::Dead));
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 1000, 8)),
             static_cast<int>(MembershipChange::Rebooted));
    CHECK_EQ(p.boot_epoch, 8u);

    // Sequence expectations are cleared, or the peer's restart from seq 0 would be charged as
    // 35,000 lost frames.
    CHECK(!p.seq_rx_valid);
    CHECK_EQ(p.seq_rx_last, static_cast<uint16_t>(0));
    CHECK_EQ(p.hb_seq_last, 0u);
}

TEST(heartbeat, reboot_without_death_is_still_a_reboot) {
    // A node can reboot and be back inside 600 ms. Nothing was declared dead, but the peer still
    // lost its state, and reporting "none" here would leave stale sequence expectations in place.
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 7;
    p.seq_rx_valid = true;
    p.seq_rx_last = 1234;

    CHECK_EQ(static_cast<int>(peer_on_frame(p, 300, 9)),
             static_cast<int>(MembershipChange::Rebooted));
    CHECK(!p.seq_rx_valid);
}

TEST(heartbeat, an_epoch_going_backwards_is_a_ghost_not_a_reboot) {
    // ESP-NOW retries for up to ~104 ms (§3), so a frame sent just before a peer rebooted can
    // arrive just after one sent afterwards. Treating any epoch *difference* as a reboot made one
    // power-cycle register two or three times, each dragging boot_epoch backwards and clearing the
    // sequence expectations again — which suppresses genuine loss detection for as long as it
    // flaps. Found by the simulator's reboot scenario: 24 reboots, 324 observations.
    //
    // Monotonicity is also what §7.7's fencing and §8.3's replay rejection already assume.
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 7;
    p.seq_rx_valid = true;
    p.seq_rx_last = 4321;
    p.hb_seq_last = 99;

    // The peer reboots: epoch advances, expectations are cleared. That is a reboot.
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 100, 8)),
             static_cast<int>(MembershipChange::Rebooted));
    CHECK_EQ(p.boot_epoch, 8u);
    CHECK(!p.seq_rx_valid);

    // Now a straggler from the previous incarnation lands.
    p.seq_rx_valid = true;
    p.seq_rx_last = 11;
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 150, 7)),
             static_cast<int>(MembershipChange::StaleEpoch));

    // Nothing moved: not the epoch, not the sequence expectations, not liveness.
    CHECK_EQ(p.boot_epoch, 8u);
    CHECK(p.seq_rx_valid);
    CHECK_EQ(p.seq_rx_last, static_cast<uint16_t>(11));
    CHECK_EQ(p.last_rx_ms, 100u);  // the ghost does not refresh liveness

    // And the live incarnation still reports normally.
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 200, 8)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(p.last_rx_ms, 200u);
}

TEST(heartbeat, repeated_stale_frames_never_accumulate_state) {
    // A burst of stragglers must be idempotent — the failure mode being guarded against is a peer
    // that flaps between two epochs and clears its sequence baseline on every frame.
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 5;
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 10, 9)),
             static_cast<int>(MembershipChange::Rebooted));
    p.seq_rx_valid = true;
    p.seq_rx_last = 500;

    for (uint32_t old = 1; old <= 8; ++old) {
        CHECK_EQ(static_cast<int>(peer_on_frame(p, 20 + old, old)),
                 static_cast<int>(MembershipChange::StaleEpoch));
    }
    CHECK_EQ(p.boot_epoch, 9u);
    CHECK(p.seq_rx_valid);
    CHECK_EQ(p.seq_rx_last, static_cast<uint16_t>(500));
    CHECK_EQ(p.last_rx_ms, 10u);
}

TEST(heartbeat, first_epoch_seen_is_not_a_reboot) {
    // A peer whose epoch we did not know yet has not rebooted; it has just introduced itself.
    PeerLink p;
    p.reset();
    p.state = PeerState::Known;
    p.hb_period_ms = kHbPeriodWirelessMs;
    p.miss_limit = kHbMissLimitWireless;
    CHECK_EQ(p.boot_epoch, 0u);

    CHECK_EQ(static_cast<int>(peer_on_frame(p, 10, 42)), static_cast<int>(MembershipChange::Alive));
    CHECK_EQ(p.boot_epoch, 42u);
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));
}

TEST(heartbeat, frames_without_an_epoch_do_not_disturb_it) {
    // Not every opcode carries a boot_epoch; those callers pass 0, which must mean "no
    // information" rather than "epoch zero".
    PeerLink p = wireless_peer(0);
    p.boot_epoch = 5;
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 100, 0)), static_cast<int>(MembershipChange::None));
    CHECK_EQ(p.boot_epoch, 5u);
}

TEST(heartbeat, bye_is_not_a_failure) {
    // §5.2: BYE is an intentional departure. Counting it as a death would make a planned shutdown
    // indistinguishable from a link failure in the statistics.
    PeerLink p = wireless_peer(0);
    CHECK_EQ(static_cast<int>(peer_on_bye(p, 100)), static_cast<int>(MembershipChange::Left));
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Left));

    // A departed peer is not then declared dead when its heartbeats stop, because of course they
    // stopped — it said so.
    for (uint32_t t = 200; t < 5000; t += 100) {
        CHECK_EQ(static_cast<int>(peer_on_tick(p, t)), static_cast<int>(MembershipChange::None));
    }
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Left));

    // A second BYE is not a second departure.
    CHECK_EQ(static_cast<int>(peer_on_bye(p, 5100)), static_cast<int>(MembershipChange::None));

    // But it can come back.
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 6000, 0)), static_cast<int>(MembershipChange::Alive));
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Alive));
}

TEST(heartbeat, known_peer_becomes_alive_on_first_frame) {
    PeerLink p;
    p.reset();
    p.state = PeerState::Known;
    p.hb_period_ms = kHbPeriodWirelessMs;
    p.miss_limit = kHbMissLimitWireless;
    p.last_rx_ms = 0;

    // A Known peer that never speaks is still declared dead: it was announced and did not follow
    // through, which is exactly the case §7.3's membership needs to notice.
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 600)), static_cast<int>(MembershipChange::Dead));
}

TEST(heartbeat, overdue_predicate) {
    PeerLink p = wireless_peer(1000);
    CHECK(!peer_is_overdue(p, 1000));
    CHECK(!peer_is_overdue(p, 1099));
    CHECK(peer_is_overdue(p, 1100));
    CHECK(peer_is_overdue(p, 9999));
}

TEST(heartbeat, ms_counter_wrap_is_survivable) {
    // Uptime is a uint32_t of milliseconds and wraps after 49.7 days. A 24-hour soak will not hit
    // it, but a deployed node will, and unsigned subtraction is only correct if nothing compares
    // the timestamps directly. Straddle the wrap and check the peer neither dies early nor
    // becomes immortal.
    PeerLink p = wireless_peer(0xFFFFFF00u);

    CHECK_EQ(static_cast<int>(peer_on_tick(p, 0xFFFFFF00u + 500)),
             static_cast<int>(MembershipChange::None));  // 500 ms later, past the wrap
    CHECK_EQ(static_cast<uint32_t>(p.misses), 5u);

    CHECK_EQ(static_cast<int>(peer_on_tick(p, 0xFFFFFF00u + 600)),
             static_cast<int>(MembershipChange::Dead));

    // And a frame arriving after the wrap revives it correctly.
    CHECK_EQ(static_cast<int>(peer_on_frame(p, 0x00000100u, 0)),
             static_cast<int>(MembershipChange::Revived));
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 0x00000100u + 599)),
             static_cast<int>(MembershipChange::None));
    CHECK_EQ(static_cast<int>(peer_on_tick(p, 0x00000100u + 600)),
             static_cast<int>(MembershipChange::Dead));
}

TEST(heartbeat, misses_saturate_rather_than_wrap) {
    // misses is a uint8_t. A peer silent for a day is 864,000 periods; if that wrapped modulo 256
    // it would periodically read as zero and the peer would look healthy.
    PeerLink p = wireless_peer(0);
    peer_on_tick(p, 100u * 1000u);  // 1000 periods
    CHECK_EQ(static_cast<uint32_t>(p.misses), 255u);
    CHECK_EQ(static_cast<int>(p.state), static_cast<int>(PeerState::Dead));
}

// ------------------------------------------------------------------------------------------
// PeerTable
// ------------------------------------------------------------------------------------------

TEST(peer_table, add_find_and_capacity) {
    PeerTable t;
    CHECK_EQ(t.capacity(), static_cast<size_t>(20));  // §3: the ESP-NOW peer ceiling
    CHECK(t.find_by_mac(kMacA) == nullptr);

    PeerLink* a = t.add(kMacA, 10, 0, kHbPeriodWirelessMs, kHbMissLimitWireless);
    CHECK(a != nullptr);
    CHECK_EQ(static_cast<int>(a->state), static_cast<int>(PeerState::Known));
    CHECK_EQ(a->node_id, static_cast<uint16_t>(10));
    CHECK_EQ(a->msg_id_next, static_cast<uint16_t>(1));  // 0 means "no correlation"

    CHECK(t.find_by_mac(kMacA) == a);
    CHECK(t.find_by_node_id(10) == a);
    CHECK(t.find_by_mac(kMacB) == nullptr);
    CHECK_EQ(t.index_of(a), static_cast<size_t>(0));

    PeerLink* b = t.add(kMacB, 11, 0, kHbPeriodWirelessMs, kHbMissLimitWireless);
    CHECK(b != nullptr);
    CHECK_EQ(t.index_of(b), static_cast<size_t>(1));
    CHECK_EQ(t.count_in_state(PeerState::Known), static_cast<size_t>(2));
}

TEST(peer_table, fills_up_and_says_so) {
    // §3 caps ESP-NOW at 20 peers, so a full table is a hardware limit to report, not a container
    // to grow. Returning nullptr rather than overwriting somebody is the whole point.
    PeerTable t;
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
    for (int i = 0; i < 20; ++i) {
        mac[5] = static_cast<uint8_t>(i);
        CHECK(t.add(mac, static_cast<uint16_t>(i), 0, kHbPeriodWirelessMs,
                    kHbMissLimitWireless) != nullptr);
    }
    mac[5] = 99;
    CHECK(t.add(mac, 99, 0, kHbPeriodWirelessMs, kHbMissLimitWireless) == nullptr);
    CHECK_EQ(t.count_in_state(PeerState::Known), static_cast<size_t>(20));
}

TEST(peer_table, a_new_peer_starts_with_clean_statistics) {
    // Slots are reused across the life of a node. A peer inheriting the previous occupant's
    // counters would corrupt the PDR figure M0 exists to produce.
    PeerTable t;
    PeerLink* a = t.add(kMacA, 10, 0, kHbPeriodWirelessMs, kHbMissLimitWireless);
    a->rx_frames = 12345;
    a->tx_cb_fail = 99;
    t.histogram(0).add(5000);
    CHECK_EQ(t.histogram(0).count(), 1u);

    a->reset();
    PeerLink* b = t.add(kMacB, 11, 0, kHbPeriodWirelessMs, kHbMissLimitWireless);
    CHECK_EQ(t.index_of(b), static_cast<size_t>(0));  // reused the freed slot
    CHECK_EQ(b->rx_frames, 0u);
    CHECK_EQ(b->tx_cb_fail, 0u);
    CHECK_EQ(t.histogram(0).count(), 0u);
}
