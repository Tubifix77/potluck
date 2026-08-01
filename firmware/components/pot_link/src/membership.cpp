// Membership and heartbeat state machine — see membership.hpp and ARCHITECTURE.md §8.2.

#include "pot/membership.hpp"

#include <cstring>

namespace pot {

const char* membership_change_str(MembershipChange c) {
    switch (c) {
        case MembershipChange::None: return "none";
        case MembershipChange::Discovered: return "discovered";
        case MembershipChange::Alive: return "alive";
        case MembershipChange::Dead: return "dead";
        case MembershipChange::Revived: return "revived";
        case MembershipChange::Rebooted: return "rebooted";
        case MembershipChange::Left: return "left";
        case MembershipChange::StaleEpoch: return "stale_epoch";
    }
    return "unknown";
}

// -------------------------------------------------------------------------------------------
// PeerTable
// -------------------------------------------------------------------------------------------

PeerTable::PeerTable() { reset(); }

void PeerTable::reset() {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        peers_[i].reset();
        histograms_[i].reset();
    }
}

PeerLink* PeerTable::find_by_mac(const uint8_t mac[kMacLen]) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].state != PeerState::Free && std::memcmp(peers_[i].mac, mac, kMacLen) == 0) {
            return &peers_[i];
        }
    }
    return nullptr;
}

PeerLink* PeerTable::find_by_node_id(uint16_t node_id) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].state != PeerState::Free && peers_[i].node_id == node_id) {
            return &peers_[i];
        }
    }
    return nullptr;
}

PeerLink* PeerTable::add(const uint8_t mac[kMacLen], uint16_t node_id, uint32_t now_ms,
                         uint32_t hb_period_ms, uint8_t miss_limit) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].state == PeerState::Free) {
            PeerLink& p = peers_[i];
            p.reset();
            histograms_[i].reset();
            std::memcpy(p.mac, mac, kMacLen);
            p.node_id = node_id;
            p.state = PeerState::Known;
            p.last_rx_ms = now_ms;
            p.hb_period_ms = hb_period_ms;
            p.miss_limit = miss_limit;
            p.msg_id_next = 1;  // 0 means "no correlation" in the payloads, so ids start at 1
            return &p;
        }
    }
    return nullptr;
}

size_t PeerTable::index_of(const PeerLink* p) const {
    if (p < &peers_[0] || p > &peers_[kMaxPeers - 1]) {
        return kMaxPeers;
    }
    return static_cast<size_t>(p - &peers_[0]);
}

size_t PeerTable::count_in_state(PeerState s) const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].state == s) {
            ++n;
        }
    }
    return n;
}

// -------------------------------------------------------------------------------------------
// The state machine
// -------------------------------------------------------------------------------------------

namespace {

// Whole heartbeat periods of silence, saturated at 255 so the uint8_t counter cannot wrap around
// to zero and quietly resurrect a peer that has been gone for a day.
uint8_t elapsed_periods(const PeerLink& p, uint32_t now_ms) {
    if (p.hb_period_ms == 0) {
        return 0;
    }
    const uint32_t silent_ms = now_ms - p.last_rx_ms;  // unsigned: wrap-safe for any real interval
    const uint32_t periods = silent_ms / p.hb_period_ms;
    return periods > 255u ? 255u : static_cast<uint8_t>(periods);
}

}  // namespace

bool peer_is_overdue(const PeerLink& p, uint32_t now_ms) {
    return elapsed_periods(p, now_ms) >= 1;
}

MembershipChange peer_on_tick(PeerLink& p, uint32_t now_ms) {
    if (p.state != PeerState::Alive && p.state != PeerState::Known) {
        // Dead peers stay dead until they send something; Left peers stay left until they say
        // HELLO again; Free slots have nothing to time out.
        return MembershipChange::None;
    }

    p.misses = elapsed_periods(p, now_ms);

    if (p.misses >= p.miss_limit) {
        p.state = PeerState::Dead;
        return MembershipChange::Dead;
    }
    return MembershipChange::None;
}

MembershipChange peer_on_frame(PeerLink& p, uint32_t now_ms, uint32_t boot_epoch) {
    const PeerState was = p.state;

    // Epoch first. A peer that rebooted has lost its sequence numbers, its statistics and its view
    // of us, so this has to be distinguishable from a link that merely recovered.
    //
    // Only an *increase* counts. An epoch going backwards is a frame from an incarnation already
    // superseded: ESP-NOW retries for up to ~104 ms (§3), so a frame sent just before a peer
    // rebooted can legitimately arrive after one sent just after. Treating any difference as a
    // reboot made a single reboot register two or three times, each one dragging p.boot_epoch
    // backwards and clearing the sequence expectations again — which would have suppressed genuine
    // loss detection for as long as the flapping continued.
    //
    // Monotonicity is also what §7.7 and §8.3 already assume: consumers fence on the highest
    // (epoch, assignment) per actor, and safety receivers "reject epochs older than the last heard
    // from that source". This makes the membership layer agree with them.
    bool rebooted = false;
    if (boot_epoch != 0) {
        if (p.boot_epoch != 0 && boot_epoch < p.boot_epoch) {
            // A ghost of a previous incarnation. Change nothing — not the epoch, not the sequence
            // expectations, and not liveness: the peer's *current* incarnation is what proves it is
            // alive, and it is beaconing on its own account.
            return MembershipChange::StaleEpoch;
        }
        if (p.boot_epoch != 0 && boot_epoch > p.boot_epoch) {
            rebooted = true;
        }
        p.boot_epoch = boot_epoch;
    }

    p.last_rx_ms = now_ms;
    p.misses = 0;
    p.state = PeerState::Alive;

    if (rebooted) {
        // The peer's seq and hb_seq restart from its new incarnation, so our expectations about
        // them are stale. Clearing them here is what stops the next frame being counted as tens of
        // thousands of losses — the counters themselves are kept, because the frames really were
        // exchanged and a 24-hour soak should not lose its history to a reboot.
        p.seq_rx_valid = false;
        p.seq_rx_last = 0;
        p.hb_seq_last = 0;
        p.probe_msg_id = 0;
        p.owed_msg_id = 0;
        return MembershipChange::Rebooted;
    }
    if (was == PeerState::Dead) {
        return MembershipChange::Revived;
    }
    if (was == PeerState::Known || was == PeerState::Left) {
        return MembershipChange::Alive;
    }
    return MembershipChange::None;
}

MembershipChange peer_on_bye(PeerLink& p, uint32_t now_ms) {
    p.last_rx_ms = now_ms;
    p.misses = 0;
    p.probe_msg_id = 0;
    p.owed_msg_id = 0;
    if (p.state == PeerState::Left) {
        return MembershipChange::None;
    }
    p.state = PeerState::Left;
    return MembershipChange::Left;
}

}  // namespace pot
