// Membership and the heartbeat state machine — ARCHITECTURE.md §8.2, §7.3.
//
// §8.2's wireless row is the whole specification this implements: period 100 ms, 6 misses,
// declared dead after 600 ms. The rationale matters as much as the numbers — 600 ms is
// "≈ 6× the ~104 ms worst-case in-protocol retry window, and strictly above L3's 500 ms deadline
// so liveness and class cannot flap at the same boundary" — so the constants below are named for
// the specification rather than inlined, and a session that wants to shorten them has to argue
// with §8.2 rather than with a magic number.
//
// The clock is injected. Every entry point takes `now_ms`, nothing here calls a timer, and the
// tests drive it by handing it fabricated time — which is the only way to test a 600 ms death
// declaration and a revival without waiting 600 ms or flaking when the host is busy.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/link_stats.hpp"

namespace pot {

// §8.2, wireless (ESP-NOW) row.
constexpr uint32_t kHbPeriodWirelessMs = 100;
constexpr uint8_t kHbMissLimitWireless = 6;
constexpr uint32_t kHbDeadAfterWirelessMs = kHbPeriodWirelessMs * kHbMissLimitWireless;
static_assert(kHbDeadAfterWirelessMs == 600, "§8.2: wireless death declaration at 600 ms");
static_assert(kHbDeadAfterWirelessMs > 500,
              "§8.2: strictly above L3's 500 ms deadline, so liveness and class cannot flap at "
              "the same boundary");

// §8.2, wired (UART / CAN) row. Not used at M0 — ESP-NOW only, per ADR-001 and M0-BRIEF.md — but
// the state machine is parameterised by period and limit, so the wired timers are a configuration
// rather than a fork. Kept here so M4's TWAI work does not re-derive them.
constexpr uint32_t kHbPeriodWiredMs = 20;
constexpr uint8_t kHbMissLimitWired = 3;

// §8.2, host link row.
constexpr uint32_t kHbPeriodHostMs = 250;
constexpr uint8_t kHbMissLimitHost = 4;

// What a tick or an inbound frame changed. The caller turns these into events and JSON; the state
// machine itself neither logs nor allocates.
enum class MembershipChange : uint8_t {
    None = 0,
    Discovered,  // a slot was taken for a peer we had not seen
    Alive,       // first heartbeat, or a Known peer became Alive
    Dead,        // §8.2 miss limit reached
    Revived,     // a Dead peer sent again, same incarnation
    Rebooted,    // a peer sent again with a different boot_epoch
    Left,        // BYE
    StaleEpoch,  // a frame from an incarnation already superseded; ignored, but counted
};

const char* membership_change_str(MembershipChange c);

// A fixed-size peer table. No heap: §6 budgets the table statically and ESP-NOW caps peers at 20
// anyway (§3), so a growable container would buy nothing and cost the budget.
class PeerTable {
  public:
    PeerTable();

    void reset();

    // Find by MAC, or by node id. Returns nullptr when absent — every caller handles that, because
    // an unknown peer is a normal thing to receive from, not an error.
    PeerLink* find_by_mac(const uint8_t mac[kMacLen]);
    PeerLink* find_by_node_id(uint16_t node_id);

    // Claim a free slot for `mac`. Returns nullptr when the table is full — §3's 20-peer ceiling
    // is a hardware limit, so this is a condition to report, not to grow through.
    PeerLink* add(const uint8_t mac[kMacLen], uint16_t node_id, uint32_t now_ms,
                  uint32_t hb_period_ms, uint8_t miss_limit);

    PeerLink& slot(size_t i) { return peers_[i]; }
    const PeerLink& slot(size_t i) const { return peers_[i]; }
    RttHistogram& histogram(size_t i) { return histograms_[i]; }
    const RttHistogram& histogram(size_t i) const { return histograms_[i]; }
    static constexpr size_t capacity() { return kMaxPeers; }

    // Index of `p` within the table, or kMaxPeers if it is not ours.
    size_t index_of(const PeerLink* p) const;

    size_t count_in_state(PeerState s) const;

  private:
    PeerLink peers_[kMaxPeers];
    RttHistogram histograms_[kMaxPeers];
};

// ---------------------------------------------------------------------------------------------
// The state machine. Two entry points: time passes, or a frame arrives.
// ---------------------------------------------------------------------------------------------

// Time passed. Recomputes `misses` from the elapsed time rather than counting ticks, so a late or
// coalesced tick cannot under-count and a burst of ticks cannot over-count: the death declaration
// depends on how long the peer has actually been silent, not on how often this was called.
//
// Returns MembershipChange::Dead the one time the peer crosses its miss limit, None thereafter.
MembershipChange peer_on_tick(PeerLink& p, uint32_t now_ms);

// A well-formed frame arrived from this peer. `boot_epoch` comes from the payload; pass 0 when the
// opcode does not carry one, and the epoch check is skipped.
//
// Ordering matters here: the epoch is checked before the liveness state is updated, so a peer that
// rebooted inside its own death window reports Rebooted rather than a plain Revived. The
// difference is not cosmetic — Revived means the link recovered, Rebooted means the node lost its
// state, and §7.7's fencing will need to tell them apart.
MembershipChange peer_on_frame(PeerLink& p, uint32_t now_ms, uint32_t boot_epoch);

// BYE received — §5.2's intentional departure. A peer that leaves on purpose is not a failure and
// must not be counted as one, which is why Left is a state and not just Dead with a flag.
MembershipChange peer_on_bye(PeerLink& p, uint32_t now_ms);

// True once the peer has been silent for at least one whole period, i.e. a heartbeat is overdue.
bool peer_is_overdue(const PeerLink& p, uint32_t now_ms);

}  // namespace pot
