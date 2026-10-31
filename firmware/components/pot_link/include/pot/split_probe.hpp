// A board whose own Wi-Fi station pins its radio, split from part of its cell: when to look on another channel.
//
// Found on the owner's cell 2026-10-18: a house mesh serves one network on channels 1 and 11. The extender (B) rebooted
// and joined the access point on 11; the board presenting over its own station (C) stayed on 1. ESP-NOW needs every
// board on one channel, a station's radio is its access point's, and the channel authority (M13: the lowest-id station)
// only works between stations that hear each other -- so the cell stayed split, both halves alive. Plain members sweep
// for a lost peer; a station cannot, without dropping its association.
//
// So a station probes: when a member its package names (a board an actor is pinned to) has been missing for kSplitMs,
// and its network was heard on another channel, it joins the access point there and waits kDwellMs. It stays if it
// found more of the cell there than it left, or a station that outranks it (whose channel the cell follows anyway);
// otherwise it goes back, and probes again after a backoff that doubles to kMaxBackoffMs -- a presenting board drops
// its broker session for a few seconds each time, so not often. Pure logic: the caller does the Wi-Fi and the counting.
//
// Only the side missing most of the named members probes (2026-10-31, the owner's cell): with B unplugged, C -- still
// with A -- probed for B every few minutes and, each time, left A, the PC and the tablet for half a minute; every board
// read C dead and back. A board powered off and a split look alike from one side, but a split always leaves one side
// with fewer of the cell, and that side finding the rest heals it.

#pragma once

#include <cstdint>

namespace pot {

class SplitProbe {
  public:
    static constexpr uint32_t kSplitMs = 60000;       // a member missing this long: probe
    static constexpr uint32_t kDwellMs = 30000;       // on the probed channel, associated, before judging it
    static constexpr uint32_t kLandMs = 45000;        // to associate there at all, else back
    static constexpr uint32_t kFirstBackoffMs = 120000;
    static constexpr uint32_t kMaxBackoffMs = 600000;

    enum class Act : uint8_t { None, Probe, Return };

    struct Inputs {
        uint32_t now_ms;
        bool associated;         // the station holds an association
        uint8_t channel;         // its radio's channel now (1..14)
        uint16_t seen;           // bit c: the network was heard on channel c in the station's scans
        uint8_t missing;         // members the package names, not this board, not alive now
        uint8_t members;         // members the package names, not this board
        uint8_t alive;           // radio peers alive now
        bool outranked_here;     // a lower-id station is alive on this channel (Node::outranking_channel() == channel)
    };

    // How long a member must be missing before a probe; kSplitMs unless set. An extender waits longer: its channel is
    // the router's, which the cell follows, so the others normally come to it first.
    void set_split_ms(uint32_t ms) { split_ms_ = ms; }

    // One look, a few times a second or once a second. Probe/Return: join an access point on target() now.
    Act tick(const Inputs& in);
    uint8_t target() const { return target_; }
    bool probing() const { return state_ == State::Probing; }

    struct Counters {
        uint32_t probes;
        uint32_t stayed;   // the cell (or more of it) was there
        uint32_t returned;
    };
    const Counters& counters() const { return c_; }

  private:
    enum class State : uint8_t { Idle, Probing };
    static uint8_t next_channel(uint16_t seen, uint8_t from, uint8_t current);

    State state_ = State::Idle;
    uint32_t split_ms_ = kSplitMs;
    uint32_t split_since_ = 0;   // 0: not split
    uint32_t next_allowed_ = 0;  // no probe before this
    uint32_t backoff_ms_ = kFirstBackoffMs;
    uint32_t started_ = 0;
    uint32_t landed_ = 0;        // 0: not yet associated on the target
    uint8_t origin_ = 0;
    uint8_t target_ = 0;
    uint8_t last_probe_ = 0;     // the channel probed last, so a third channel is tried next
    uint8_t alive_before_ = 0;
    uint8_t missing_before_ = 0;
    Counters c_{};
};

inline uint8_t SplitProbe::next_channel(uint16_t seen, uint8_t from, uint8_t current) {
    for (uint8_t step = 1; step <= 14; ++step) {
        const uint8_t c = static_cast<uint8_t>((from - 1 + step) % 14 + 1);
        if (c != current && (seen & (1u << c)) != 0) return c;
    }
    return 0;
}

inline SplitProbe::Act SplitProbe::tick(const Inputs& in) {
    if (state_ == State::Probing) {
        if (in.associated && in.channel == target_) {
            if (landed_ == 0) landed_ = in.now_ms == 0 ? 1 : in.now_ms;
            if (in.now_ms - landed_ < kDwellMs) return Act::None;
            if (in.outranked_here || in.alive > alive_before_ || in.missing < missing_before_) {
                ++c_.stayed;  // the cell, or the part of it that decides the channel, is here
                state_ = State::Idle;
                split_since_ = 0;
                backoff_ms_ = kFirstBackoffMs;
                return Act::None;
            }
        } else if (in.now_ms - started_ < kLandMs) {
            return Act::None;  // still associating there
        }
        ++c_.returned;
        state_ = State::Idle;
        split_since_ = 0;
        next_allowed_ = in.now_ms + backoff_ms_ == 0 ? 1 : in.now_ms + backoff_ms_;
        backoff_ms_ = backoff_ms_ * 2 > kMaxBackoffMs ? kMaxBackoffMs : backoff_ms_ * 2;
        target_ = origin_;
        return Act::Return;
    }
    if (!in.associated || in.missing == 0 || 2u * in.missing <= in.members) {
        split_since_ = 0;  // nobody missing, or most of the cell here: a board gone, not a split to look for
        if (in.missing == 0) backoff_ms_ = kFirstBackoffMs;
        return Act::None;
    }
    if (split_since_ == 0) split_since_ = in.now_ms == 0 ? 1 : in.now_ms;
    if (in.now_ms - split_since_ < split_ms_) return Act::None;
    if (next_allowed_ != 0 && static_cast<int32_t>(in.now_ms - next_allowed_) < 0) return Act::None;
    const uint8_t to = next_channel(in.seen, last_probe_ != 0 ? last_probe_ : in.channel, in.channel);
    if (to == 0) return Act::None;  // the network is on this channel only: the split is not the mesh's doing
    ++c_.probes;
    state_ = State::Probing;
    origin_ = in.channel;
    target_ = to;
    last_probe_ = to;
    started_ = in.now_ms;
    landed_ = 0;
    alive_before_ = in.alive;
    missing_before_ = in.missing;
    return Act::Probe;
}

}  // namespace pot
