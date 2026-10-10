// The Potluck node policy — see node.hpp.

#include "pot/node.hpp"
#include "pot/sys_resources.hpp"

#include <cstring>

#include "pot/opcodes.hpp"

namespace pot {

const uint8_t kBroadcastMacAddr[kMacLen] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

const char* beacon_mode_str(BeaconMode m) {
    switch (m) {
        case BeaconMode::UnicastFullMesh: return "unicast_full_mesh";
        case BeaconMode::BroadcastBeacon: return "broadcast_beacon";
    }
    return "unknown";
}

namespace {

// M0 traffic is L3 (§4: one wireless hop, <500 ms). Membership runs at priority 1 so anything a
// later milestone puts at 0 loses to liveness and everything above wins.
constexpr uint8_t kMembershipPriority = 1;

}  // namespace

Node::Node(const NodeConfig& cfg, const NodeHal& hal) : cfg_(cfg), hal_(hal) {
    peers_.reset();
    counters_.reset();
    events_.reset();
    std::memset(tx_, 0, sizeof(tx_));
}

// -------------------------------------------------------------------------------------------
// Plumbing
// -------------------------------------------------------------------------------------------

void Node::emit(EventKind kind, const PeerLink* p, uint32_t a, uint32_t b) {
    Event e{};
    e.at_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    e.kind = kind;
    e.node_id = (p != nullptr) ? p->node_id : 0;
    e.peer_slot = (p != nullptr) ? static_cast<uint8_t>(peers_.index_of(p)) : 0xFF;
    e.detail_a = a;
    e.detail_b = b;
    events_.push(e);
    if (hal_.on_event != nullptr) {
        hal_.on_event(hal_.ctx, e);
    }
}

void Node::record_event(EventKind kind, uint16_t node_id, uint32_t a, uint32_t b) {
    Event e{};
    e.at_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    e.kind = kind;
    e.node_id = node_id;
    e.peer_slot = 0xFF;
    e.detail_a = a;
    e.detail_b = b;
    events_.push(e);
    if (hal_.on_event != nullptr) {
        hal_.on_event(hal_.ctx, e);
    }
}

void Node::note(MembershipChange c, PeerLink* p) {
    switch (c) {
        case MembershipChange::Dead:
            ++counters_.deaths_declared;
            emit(EventKind::PeerDead, p, p->misses,
                 (hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0) - p->last_rx_ms);
            // Work units dispatched to it are owed answers that are never coming. Writing them
            // off here rather than on a timer is deliberate: the death window (section 8.2) is
            // the system's one definition of "gone", and a second, longer deadline invented for
            // work units would be a number with no measurement behind it -- and would make a
            // coordinator wait past the moment it already knew.
            abandon_calls_to(p->node_id);
            break;
        case MembershipChange::Revived:
            ++counters_.revivals;
            emit(EventKind::PeerRevived, p);
            break;
        case MembershipChange::Rebooted:
            ++counters_.reboots_seen;
            emit(EventKind::PeerRebooted, p, p->boot_epoch);
            // A rebooted authority's channel is unknown until its first HELLO says: its first beacon
            // arrives before that HELLO, and a sweep that heard it would stop wherever its station's
            // scan happened to be (node.hpp, authority_declared_).
            if (p->node_id == authority_id_) authority_declared_ = 0;
            // A rebooted worker is a different incarnation: it has no memory of the unit it
            // accepted, so that unit is lost even though the peer is answering again.
            abandon_calls_to(p->node_id);
            break;
        case MembershipChange::Left:
            emit(EventKind::PeerLeft, p);
            abandon_calls_to(p->node_id);
            break;
        case MembershipChange::StaleEpoch: ++counters_.rx_stale_epoch; break;
        case MembershipChange::Alive: emit(EventKind::PeerAlive, p); break;
        default: break;
    }
}

void Node::pin_version(PeerLink& p, uint8_t peer_version) {
    // §5.3: both ends must be v2 before the 1446-byte profile is used. Unknown means unknown, and
    // the v1 floor is the safe answer — a v1 receiver silently truncates a longer frame.
    const EspNowVersion was = p.version;
    if (peer_version >= 2 && cfg_.espnow_version >= 2) {
        p.version = EspNowVersion::V2;
    } else if (peer_version >= 1) {
        p.version = EspNowVersion::V1;
    }
    if (p.version != was) {
        emit(EventKind::VersionPinned, &p, static_cast<uint32_t>(p.version), p.max_payload());
    }
}

// -------------------------------------------------------------------------------------------
// Sending
// -------------------------------------------------------------------------------------------

bool Node::send_frame(PeerLink* p, const uint8_t mac[kMacLen], uint8_t opcode, const void* payload,
                      uint16_t payload_len, bool ack_req, uint16_t msg_id, uint16_t dst_override,
                      bool broadcast, bool single_frame, uint8_t lclass, uint8_t priority) {
    EncodeSpec spec;
    spec.src = cfg_.node_id;
    spec.dst = dst_override;
    spec.opcode = opcode;
    spec.lclass = lclass;
    spec.priority = priority;
    // §5.1: seq is per (src,dst). Broadcast is a distinct destination and keeps its own counter.
    spec.seq = single_frame ? 0 : broadcast ? seq_tx_bcast_++ : (p != nullptr ? p->seq_tx++ : 0);
    spec.msg_id = msg_id;
    spec.ack_req = ack_req;
    // M5 step 4: tag every unicast frame to a peer whose key we hold (frame_auth.hpp). §14 reserved
    // the eight bytes in §5.3's MTU arithmetic from day one, so no payload cap moves.
    PeerAuth* tag_with = nullptr;
    if (trust_ != nullptr && trust_->enrolled && p != nullptr && !broadcast && !single_frame) {
        const size_t i = peers_.index_of(p);
        // The cable's own far end needs no tag (section 9.3). A member reached THROUGH the cable -- M10's
        // host talking to the cell via its board's relay -- does: the tag is between the two ends, and
        // the far member checks it; on the bench every claim the PC sent B and C was "untagged".
        const bool cable_end = is_trusted_link(mac) && !(i < kMaxPeers && relayed_[i]);
        if (i < kMaxPeers && auth_[i].verified && !cable_end) tag_with = &auth_[i];
    }
    spec.auth = (tag_with != nullptr);

    const uint16_t cap = (p != nullptr) ? p->max_payload() : kMaxPayloadV1;
    if (payload_len > cap) {
        return false;
    }

    size_t written = 0;
    if (encode(spec, static_cast<const uint8_t*>(payload), payload_len, tx_, sizeof(tx_), written) !=
        FrameError::Ok) {
        return false;
    }

    if (tag_with != nullptr) {
        const size_t body = written - kAuthTagSize;
        frame_tag(tag_with->key, tag_with->tx_hi, tx_, body, tx_ + body);
        if (spec.seq == 0xFFFF) ++tag_with->tx_hi;  // the next frame is seq 0 of the next subspace
    }

    ++counters_.tx_total;
    tally_.bytes += static_cast<uint32_t>(written);
    if (p != nullptr && !broadcast) {
        ++p->tx_frames;
    }

    const int32_t err = hal_.send ? hal_.send(hal_.ctx, mac, tx_, written) : -1;
    if (err != 0) {
        ++counters_.tx_enqueue_err;
        if (p != nullptr && !broadcast) {
            ++p->tx_enqueue_err;
        }
        emit(EventKind::TxError, p, static_cast<uint32_t>(err));
        return false;
    }
    return true;
}

void Node::heavy(void (*fn)(void*), void* arg) {
    if (hal_.run_heavy != nullptr) {
        hal_.run_heavy(hal_.ctx, fn, arg);
    } else {
        fn(arg);
    }
}

namespace {

struct VerifyJob {
    const Identity* id;
    const uint8_t* mac;
    const uint8_t* payload;
    size_t len;
    uint16_t sender;
    uint16_t my_id;
    uint32_t my_epoch;
    uint32_t peer_epoch;
    HelloAuth result;
    bool key_ok;
    uint8_t key[kSessionKeyLen];
};

void verify_job(void* arg) {
    VerifyJob& j = *static_cast<VerifyJob*>(arg);
    j.result = hello_verify(j.id->ca_pub, j.mac, j.payload, j.len, j.sender);
    j.key_ok = false;
    if (j.result.error == HelloAuthError::Ok) {
        j.key_ok = session_key(*j.id, j.my_id, j.my_epoch, j.result.peer_pub, j.sender, j.peer_epoch, j.key);
    }
}

struct SignJob {
    const Identity* id;
    const uint8_t* mac;
    const uint8_t* base;
    uint8_t* out;
    bool ok;
};

void sign_job(void* arg) {
    SignJob& j = *static_cast<SignJob*>(arg);
    j.ok = hello_sign(*j.id, j.mac, j.base, j.out);
}

}  // namespace

namespace {
struct ChSignJob {
    const Identity* id;
    uint16_t node_id;
    const uint8_t* base;
    uint8_t* sig;
    bool ok;
};
void ch_sign_job(void* a) {
    ChSignJob& j = *static_cast<ChSignJob*>(a);
    j.ok = channel_sign(*j.id, j.node_id, j.base, j.sig);
}
struct ChVerifyJob {
    const uint8_t* pub;
    uint16_t node_id;
    const uint8_t* base;
    const uint8_t* sig;
    bool ok;
};
void ch_verify_job(void* a) {
    ChVerifyJob& j = *static_cast<ChVerifyJob*>(a);
    j.ok = channel_verify(j.pub, j.node_id, j.base, j.sig);
}
}  // namespace

void Node::retune(uint8_t ch) {
    if (ch != channel_) signed_hello_ok_[0] = signed_hello_ok_[1] = false;  // HELLO declares the channel
    channel_ = ch;
    if (hal_.set_channel != nullptr) hal_.set_channel(hal_.ctx, ch);
}

void Node::set_channel_now(uint8_t ch) {
    if (ch < cfg_.channel_lo || ch > cfg_.channel_hi) return;
    retune(ch);
    emit(EventKind::ChannelChanged, nullptr, ch, 3);
    last_alive_ms_ = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;  // give the move a fair chance first
    if (!departed_) send_hello(true);
}

bool Node::move_cell(uint8_t ch, uint16_t delay_ms) {
    if (ch < cfg_.channel_lo || ch > cfg_.channel_hi || delay_ms > 2000) return false;
    ChannelPayload c{};
    c.channel = ch;
    c.reason = 1;
    c.delay_ms = delay_ms;
    c.epoch = cfg_.boot_epoch;
    c.move_id = ++move_tx_;
    uint8_t buf[kChannelSignedLen];
    std::memcpy(buf, &c, kChannelBaseLen);
    uint16_t len = static_cast<uint16_t>(kChannelBaseLen);
    if (trust_ != nullptr && trust_->enrolled) {
        ChSignJob job{trust_, cfg_.node_id, buf, buf + kChannelBaseLen, false};
        heavy(&ch_sign_job, &job);
        if (!job.ok) return false;
        len = static_cast<uint16_t>(kChannelSignedLen);
    }
    // Said three times before anyone moves: a broadcast has no MAC-layer retries.
    for (int i = 0; i < 3; ++i) {
        send_frame(nullptr, kBroadcastMacAddr, kOpChannel, buf, len, false, 0, kNodeBroadcast, true);
    }
    ++ch_counters_.moves_sent;
    pending_channel_ = ch;
    pending_channel_at_ms_ = (hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0) + delay_ms;
    if (pending_channel_at_ms_ == 0) pending_channel_at_ms_ = 1;
    return true;
}

void Node::handle_channel(PeerLink* p, const Frame& f) {
    if (p == nullptr || f.payload_len < kChannelBaseLen) {
        ++ch_counters_.moves_refused;
        return;
    }
    ChannelPayload c{};
    std::memcpy(&c, f.payload, kChannelBaseLen);
    const size_t idx = peers_.index_of(p);
    if (c.channel < cfg_.channel_lo || c.channel > cfg_.channel_hi || c.delay_ms > 2000 || idx >= kMaxPeers ||
        cfg_.channel_fixed) {
        ++ch_counters_.moves_refused;
        return;
    }
    if (trust_required()) {
        // A forged move would scatter the cell, so it is signed, and fenced by the sender's current
        // verified epoch: a recorded move from an earlier incarnation is refused without a verify.
        const PeerAuth* pa = peer_auth(p);
        if (pa != nullptr && pa->verified && c.epoch == pa->epoch && c.move_id == move_rx_last_[idx] &&
            move_rx_last_[idx] != 0) {
            return;  // the same move again (it is said three times): already scheduled
        }
        if (f.payload_len != kChannelSignedLen || pa == nullptr || !pa->verified || c.epoch != pa->epoch ||
            c.move_id <= move_rx_last_[idx]) {
            ++ch_counters_.moves_refused;
            return;
        }
        ChVerifyJob job{pa->pub, f.hdr.src, f.payload, f.payload + kChannelBaseLen, false};
        heavy(&ch_verify_job, &job);
        if (!job.ok) {
            ++ch_counters_.moves_refused;
            return;
        }
    } else if (c.move_id <= move_rx_last_[idx] && c.epoch == p->boot_epoch) {
        return;  // a repeat of a move already scheduled (it is said three times)
    }
    move_rx_last_[idx] = c.move_id;
    ++ch_counters_.moves_followed;
    pending_channel_ = c.channel;
    pending_channel_at_ms_ = (hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0) + c.delay_ms;
    if (pending_channel_at_ms_ == 0) pending_channel_at_ms_ = 1;
}

void Node::tick_channel(uint32_t now) {
    if (pending_channel_at_ms_ != 0 && static_cast<int32_t>(now - pending_channel_at_ms_) >= 0) {
        pending_channel_at_ms_ = 0;
        if (pending_channel_ != channel_) {
            retune(pending_channel_);
            emit(EventKind::ChannelChanged, nullptr, pending_channel_, 1);
        }
        last_alive_ms_ = now;
        if (!departed_) send_hello(true);
    }
    if (cfg_.channel_fixed || hal_.set_channel == nullptr || departed_) return;
    const size_t alive = peers_.count_in_state(PeerState::Alive);

    // M6.1: a sweep for the channel authority (see node.hpp). It ends when the authority is heard
    // again -- on whatever channel the sweep, or its HELLO's declared channel, brought us to -- or after
    // one pass over the band, when we go home: it really died, and the others are there.
    if (sweeping_) {
        const PeerLink* a = peers_.find_by_node_id(authority_id_);
        if (a != nullptr && a->state == PeerState::Alive && authority_declared_ != 0) {
            sweeping_ = false;
            ++ch_counters_.authority_found;
            settle_until_ms_ = now + 2 * cfg_.hello_interval_ms + 500;
            last_alive_ms_ = now;
            emit(EventKind::ChannelChanged, nullptr, channel_, 6);
            if (!departed_) send_hello(true);
            return;
        }
        if (static_cast<int32_t>(now - scan_next_ms_) >= 0) {
            if (sweep_left_ == 0) {
                sweeping_ = false;
                retune(sweep_home_);
                last_alive_ms_ = now;
                emit(EventKind::ChannelChanged, nullptr, sweep_home_, 7);
                if (!departed_) send_hello(true);
                // Not found anywhere *now*. It may be rebooting: keep looking, gently (node.hpp).
                searching_ = true;
                visiting_ = false;
                search_home_ = sweep_home_;
                search_cursor_ = sweep_home_;
                search_in_pass_ = 0;
                search_interval_ms_ = kSearchFirstMs;
                search_next_ms_ = now + search_interval_ms_;
                return;
            }
            const uint8_t next = (channel_ >= cfg_.channel_hi || channel_ < cfg_.channel_lo)
                                     ? cfg_.channel_lo
                                     : static_cast<uint8_t>(channel_ + 1);
            --sweep_left_;
            retune(next);
            ++ch_counters_.scan_hops;
            scan_next_ms_ = now + cfg_.scan_dwell_ms;
            send_hello(true);
        }
        return;
    }
    if (searching_) {
        const PeerLink* a = peers_.find_by_node_id(authority_id_);
        if (a != nullptr && a->state == PeerState::Alive && authority_declared_ != 0) {
            // Heard again. On a visit: it is here, so stay, and the members at home find us by CR-1's
            // scan once they have lost everyone. At home: it came back where we are.
            searching_ = false;
            if (visiting_) {
                visiting_ = false;
                ++ch_counters_.authority_found;
                settle_until_ms_ = now + 2 * cfg_.hello_interval_ms + 500;
                last_alive_ms_ = now;
                emit(EventKind::ChannelChanged, nullptr, channel_, 6);
                if (!departed_) send_hello(true);
            }
            return;
        }
        if (visiting_) {
            if (static_cast<int32_t>(now - visit_end_ms_) >= 0) {
                visiting_ = false;
                retune(search_home_);
            }
            return;
        }
        if (alive == 0 || scanning_) {
            searching_ = false;  // everyone is gone: CR-1's scan is the search now
        } else if (static_cast<int32_t>(now - search_next_ms_) >= 0) {
            if (channel_ != search_home_) search_home_ = channel_;  // the cell moved meanwhile
            uint8_t next = search_cursor_;
            for (int k = 0; k <= cfg_.channel_hi - cfg_.channel_lo + 1; ++k) {
                next = (next >= cfg_.channel_hi || next < cfg_.channel_lo) ? cfg_.channel_lo
                                                                           : static_cast<uint8_t>(next + 1);
                if (next != search_home_) break;
            }
            search_cursor_ = next;
            retune(next);
            visiting_ = true;
            visit_end_ms_ = now + cfg_.scan_dwell_ms;
            ++ch_counters_.search_visits;
            send_hello(true);
            if (++search_in_pass_ >= cfg_.channel_hi - cfg_.channel_lo) {
                search_in_pass_ = 0;
                search_interval_ms_ = search_interval_ms_ * 2 > kSearchMaxMs ? kSearchMaxMs : search_interval_ms_ * 2;
            }
            search_next_ms_ = now + search_interval_ms_;
            return;
        }
    }
    if (authority_id_ != 0 && alive > 0 && !scanning_ && !searching_) {
        const PeerLink* a = peers_.find_by_node_id(authority_id_);
        if (a == nullptr || a->state == PeerState::Dead) {
            sweeping_ = true;
            sweep_home_ = channel_;
            // Every other channel once, home last: if it is not anywhere, it is not anywhere.
            sweep_left_ = static_cast<uint8_t>(cfg_.channel_hi - cfg_.channel_lo);
            scan_next_ms_ = now;
            ++ch_counters_.authority_sweeps;
            return;
        }
    }

    if (alive > 0) {
        if (scanning_) {
            scanning_ = false;
            ++ch_counters_.found_by_scan;
            ch_counters_.last_lost_to_found_ms = now - scan_started_ms_ + cfg_.scan_after_ms;
            // Long enough for a stable peer's next HELLO to tell us where it really is.
            settle_until_ms_ = now + 2 * cfg_.hello_interval_ms + 500;
            if (!departed_) send_hello(true);
            emit(EventKind::ChannelChanged, nullptr, channel_, 2);
        }
        last_alive_ms_ = now;
        return;
    }
    if (!scanning_) {
        if (now - last_alive_ms_ < cfg_.scan_after_ms) return;
        scanning_ = true;
        scan_started_ms_ = now;
        scan_next_ms_ = now;
        ++ch_counters_.scans_started;
    }
    if (static_cast<int32_t>(now - scan_next_ms_) >= 0) {
        const uint8_t next = (channel_ >= cfg_.channel_hi || channel_ < cfg_.channel_lo)
                                 ? cfg_.channel_lo
                                 : static_cast<uint8_t>(channel_ + 1);
        retune(next);
        ++ch_counters_.scan_hops;
        scan_next_ms_ = now + cfg_.scan_dwell_ms;
        send_hello(true);  // so a scanner can be found as well as find
    }
}

PeerLink* Node::direct_peer(const uint8_t mac[kMacLen]) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& q = peers_.slot(i);
        if (q.state != PeerState::Free && !relayed_[i] && std::memcmp(q.mac, mac, kMacLen) == 0) return &q;
    }
    return nullptr;
}

PeerLink* Node::relayed_peer(uint16_t node_id) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& q = peers_.slot(i);
        if (q.state != PeerState::Free && relayed_[i] && q.node_id == node_id) return &q;
    }
    return nullptr;
}

PeerLink* Node::any_peer(uint16_t node_id) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& q = peers_.slot(i);
        if (q.state != PeerState::Free && q.node_id == node_id) return &q;
    }
    return nullptr;
}

bool Node::peer_relayed(const PeerLink* p) const {
    if (p == nullptr) return false;
    const size_t i = peers_.index_of(p);
    return i < kMaxPeers && relayed_[i];
}

void Node::set_relay(bool on) {
    if (relay_ == on) return;
    relay_ = on;
    announce_change();  // the RELAY bit is in HELLO
}

void Node::relay_hello(const Frame& f, const uint8_t orig_mac[kMacLen]) {
    // The original HELLO, header fields unchanged, with the sender's MAC appended (ADR-009).
    uint8_t payload[kHelloSignedLen + kRelayMacTrailer];
    if (f.payload_len > kHelloSignedLen) return;
    std::memcpy(payload, f.payload, f.payload_len);
    std::memcpy(payload + f.payload_len, orig_mac, kMacLen);
    EncodeSpec spec;
    spec.src = f.hdr.src;
    spec.dst = f.hdr.dst;
    spec.opcode = f.hdr.opcode;
    spec.seq = f.hdr.seq;
    spec.msg_id = f.hdr.msg_id;
    size_t n = 0;
    if (encode(spec, payload, static_cast<uint16_t>(f.payload_len + kRelayMacTrailer), tx_, sizeof(tx_), n) !=
        FrameError::Ok) {
        return;
    }
    if (hal_.send != nullptr && hal_.send(hal_.ctx, kBroadcastMacAddr, tx_, n) == 0) ++relay_counters_.hello_forwarded;
}

void Node::announce_change() {
    // A HELLO that says something new: the cached signature covers the old content.
    signed_hello_ok_[0] = signed_hello_ok_[1] = false;
    announce_left_ = 3;
    next_announce_ms_ = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
}

bool Node::set_heartbeat_window(uint32_t period_ms, uint8_t miss_limit) {
    if (period_ms < 10 || period_ms > 2550 || period_ms % 10 != 0 || miss_limit == 0) {
        return false;
    }
    const uint32_t old_window = cfg_.hb_period_ms * cfg_.hb_miss_limit;
    const bool looser = period_ms * miss_limit > old_window || period_ms > cfg_.hb_period_ms;
    announced_period_ms_ = period_ms;
    announced_miss_ = miss_limit;
    if (looser) {
        // Keep beaconing at the old rate while the news spreads: the old death window, plus two
        // ordinary HELLO intervals, since every periodic HELLO carries the new window too. A peer
        // is caught out only if it misses all three announcements AND two periodic HELLOs in a row.
        const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
        window_apply_at_ms_ = now + old_window + 2 * cfg_.hello_interval_ms;
        if (window_apply_at_ms_ == 0) window_apply_at_ms_ = 1;
    } else {
        window_apply_at_ms_ = 0;
        cfg_.hb_period_ms = period_ms;
        cfg_.hb_miss_limit = miss_limit;
    }
    announce_change();
    return true;
}

void Node::set_busy(bool b) {
    const uint32_t next = b ? (caps_ | kHelloCapBusy) : (caps_ & ~kHelloCapBusy);
    if (next == caps_) return;
    caps_ = next;
    announce_change();
}

uint32_t Node::peer_caps(const PeerLink* p) const {
    if (p == nullptr) return 0;
    const size_t i = peers_.index_of(p);
    return (i < kMaxPeers) ? peer_caps_[i] : 0;
}

bool Node::is_trusted_link(const uint8_t mac[kMacLen]) const {
    return cfg_.has_trusted_mac && std::memcmp(mac, cfg_.trusted_mac, kMacLen) == 0;
}

void Node::set_trust(const Identity* id, bool require) {
    trust_ = id;
    require_auth_ = require;
    signed_hello_ok_[0] = signed_hello_ok_[1] = false;
    for (PeerAuth& a : auth_) a = PeerAuth{};
}

const Node::PeerAuth* Node::peer_auth(const PeerLink* p) const {
    if (p == nullptr) return nullptr;
    const size_t i = peers_.index_of(p);
    return (i < kMaxPeers) ? &auth_[i] : nullptr;
}

void Node::refuse(uint16_t claimed_id, HelloAuthError e, CertError ce) {
    ++auth_counters_.refused;
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    for (const Refusal& r : refusals_) {
        if (r.at_ms != 0 && r.node_id == claimed_id && r.reason == static_cast<uint8_t>(e) && now - r.at_ms < 10000) {
            return;  // counted; logged within the last 10 s already
        }
    }
    Refusal& slot = refusals_[refusal_cursor_];
    refusal_cursor_ = (refusal_cursor_ + 1) % (sizeof(refusals_) / sizeof(refusals_[0]));
    slot.node_id = claimed_id;
    slot.reason = static_cast<uint8_t>(e);
    slot.at_ms = (now == 0) ? 1 : now;
    Event ev{};
    ev.at_ms = now;
    ev.kind = EventKind::PeerRefused;
    ev.node_id = claimed_id;
    ev.peer_slot = 0xFF;
    ev.detail_a = static_cast<uint32_t>(e);
    ev.detail_b = static_cast<uint32_t>(ce);
    events_.push(ev);
    if (hal_.on_event != nullptr) {
        hal_.on_event(hal_.ctx, ev);
    }
}

Node::AuthOutcome Node::authenticate_hello(PeerLink* p, const uint8_t mac[kMacLen], const Frame& f,
                                           const HelloPayload& h, PeerAuth& out) {
    if (!trust_->enrolled) {
        if (!require_auth_) return AuthOutcome::Legacy;
        refuse(h.node_id, HelloAuthError::NotEnrolled, CertError::Ok);
        return AuthOutcome::Refused;
    }
    if (f.payload_len == kHelloBaseLen) {
        if (!require_auth_) return AuthOutcome::Legacy;
        refuse(h.node_id, HelloAuthError::Unsigned, CertError::Ok);
        return AuthOutcome::Refused;
    }
    const PeerAuth* known = peer_auth(p);
    uint8_t dg[kHelloDigestLen];
    hello_digest(mac, f.payload, f.payload_len, dg);
    uint32_t issued = 0;
    hello_cert_issued(f.payload, f.payload_len, issued);  // unverified: only decides whether to verify
    if (known != nullptr && known->verified) {
        if (std::memcmp(dg, known->digest, kHelloDigestLen) == 0) {
            ++auth_counters_.cache_hits;
            return AuthOutcome::Cached;
        }
        // The fence is (certificate issue time, boot epoch). An older incarnation's HELLO -- late, or
        // replayed after the peer rebooted -- never rolls the verified epoch, and the session key with
        // it, backwards; nor does a HELLO under an OLDER certificate, whatever its epoch (a recording
        // from before the peer was re-enrolled). A NEWER certificate is a new lineage: the board was
        // erased and enrolled again, and its epoch counter restarted with its flash (M0-LOG session 27).
        if (issued < known->issued || (issued == known->issued && h.boot_epoch < known->epoch)) {
            ++auth_counters_.stale_epoch;
            return AuthOutcome::Ignore;
        }
    }
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    if (verify_refill_ms_ == 0) verify_refill_ms_ = now;
    while (now - verify_refill_ms_ >= kVerifyRefillMs) {
        verify_refill_ms_ += kVerifyRefillMs;
        if (verify_tokens_ < kVerifyBurst) ++verify_tokens_;
    }
    if (verify_tokens_ == 0) {
        ++auth_counters_.rate_limited;
        return AuthOutcome::Ignore;
    }
    --verify_tokens_;
    const uint32_t t0 = hal_.now_us ? hal_.now_us(hal_.ctx) : 0;
    VerifyJob job{};
    job.id = trust_;
    job.mac = mac;
    job.payload = f.payload;
    job.len = f.payload_len;
    job.sender = f.hdr.src;
    job.my_id = cfg_.node_id;
    job.my_epoch = cfg_.boot_epoch;
    job.peer_epoch = h.boot_epoch;
    heavy(&verify_job, &job);
    const HelloAuth& r = job.result;
    if (r.error != HelloAuthError::Ok) {
        refuse(h.node_id, r.error, r.cert_error);
        return AuthOutcome::Refused;
    }
    out = PeerAuth{};
    if (!job.key_ok) {
        refuse(h.node_id, HelloAuthError::WeakKey, CertError::Ok);
        return AuthOutcome::Refused;
    }
    std::memcpy(out.key, job.key, kSessionKeyLen);
    std::memset(job.key, 0, kSessionKeyLen);
    out.verified = true;
    out.epoch = h.boot_epoch;
    out.issued = issued;  // now authentic: the certificate verified
    std::memcpy(out.pub, r.peer_pub, kEdPubLen);
    std::memcpy(out.digest, dg, kHelloDigestLen);
    ++auth_counters_.verified;
    const uint32_t took = (hal_.now_us ? hal_.now_us(hal_.ctx) : 0) - t0;
    if (took > auth_counters_.verify_us_max) auth_counters_.verify_us_max = took;
    return AuthOutcome::Fresh;
}

void Node::reject_frame(const PeerLink& p, uint32_t reason, uint32_t ext_seq) {
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    if (last_reject_event_ms_ != 0 && now - last_reject_event_ms_ < 1000) return;  // counted regardless
    last_reject_event_ms_ = (now == 0) ? 1 : now;
    emit(EventKind::FrameRejected, &p, reason, ext_seq);
}

bool Node::check_frame_auth(PeerLink& p, const uint8_t* data, const Frame& f) {
    const size_t i = peers_.index_of(&p);
    PeerAuth* pa = (i < kMaxPeers) ? &auth_[i] : nullptr;
    if (pa == nullptr || !pa->verified || !f.has_auth() || f.auth_tag == nullptr) {
        ++auth_counters_.untagged;
        reject_frame(p, 3, f.hdr.seq);
        return false;
    }
    // RFC 4303 section 3.4.3's order: the cheap replay check first, then integrity, and the window
    // moves only once the tag has verified.
    const uint16_t hi = replay_seq_hi(pa->rx, f.hdr.seq);
    const uint32_t ext = (static_cast<uint32_t>(hi) << 16) | f.hdr.seq;
    if (replay_check(pa->rx, ext) != ReplayVerdict::New) {
        ++auth_counters_.replayed;
        reject_frame(p, 1, ext);
        return false;
    }
    const uint32_t t0 = hal_.now_us ? hal_.now_us(hal_.ctx) : 0;
    const bool ok = frame_tag_ok(pa->key, hi, data, kHeaderSize + f.payload_len, f.auth_tag);
    const uint32_t took = (hal_.now_us ? hal_.now_us(hal_.ctx) : 0) - t0;
    if (took > auth_counters_.tag_us_max) auth_counters_.tag_us_max = took;
    if (!ok) {
        ++auth_counters_.bad_tag;
        reject_frame(p, 2, ext);
        return false;
    }
    replay_accept(pa->rx, ext);
    ++auth_counters_.tags_ok;
    return true;
}

void Node::send_hello(bool want_ack) {
    HelloPayload h{};
    h.boot_epoch = cfg_.boot_epoch;
    const uint32_t hnow = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    // 0: "not known", which nobody follows -- a station still hunting, or a node only visiting a channel
    // while it sweeps or searches for its authority (another sweeper once followed one there).
    const uint8_t declared = (cfg_.channel_known && !sweeping_ && !visiting_) ? channel_ : 0;
    h.caps = caps_ | (static_cast<uint32_t>(declared & 0xF) << kHelloCapChannelShift) |
             (cfg_.channel_fixed ? kHelloCapChannelFixed : 0u) | (settling(hnow) ? kHelloCapSettling : 0u) |
             (relay_ ? kHelloCapRelay : 0u);
    h.node_id = cfg_.node_id;
    h.espnow_version = cfg_.espnow_version;
    // M5.1: the window being announced, which leads cfg_ while a looser window is pending.
    const uint32_t period = announced_period_ms_ != 0 ? announced_period_ms_ : cfg_.hb_period_ms;
    const uint8_t misses = announced_miss_ != 0 ? announced_miss_ : cfg_.hb_miss_limit;
    h.hb_period_cs = static_cast<uint8_t>(period / 10);
    h.hb_miss_limit = misses;
    h.flags = want_ack ? kHelloFlagWantAck : 0;
    const void* payload = &h;
    uint16_t plen = static_cast<uint16_t>(sizeof(h));
    if (trust_ != nullptr && trust_->enrolled) {
        // M5: the first 8 bytes of our key, then the certificate and a signature appended (200 B).
        std::memcpy(h.pubkey_fp, trust_->pub, sizeof(h.pubkey_fp));
        const size_t k = want_ack ? 1 : 0;
        if (!signed_hello_ok_[k]) {
            uint8_t base[kHelloBaseLen];
            std::memcpy(base, &h, kHelloBaseLen);
            SignJob job{trust_, cfg_.mac, base, signed_hello_[k], false};
            heavy(&sign_job, &job);
            signed_hello_ok_[k] = job.ok;
        }
        if (signed_hello_ok_[k]) {
            payload = signed_hello_[k];
            plen = static_cast<uint16_t>(kHelloSignedLen);
        }
    }
    if (send_frame(nullptr, kBroadcastMacAddr, kOpHello, payload, plen, false, 0, kNodeBroadcast,
                   true)) {
        ++tally_.hellos;
    }
}

void Node::send_hello_ack(PeerLink& p, uint8_t decision, uint16_t ref_msg_id) {
    HelloAckPayload a{};
    a.boot_epoch = cfg_.boot_epoch;
    a.node_id = cfg_.node_id;
    a.espnow_version = cfg_.espnow_version;
    a.decision = decision;
    a.hb_period_cs = static_cast<uint8_t>((announced_period_ms_ != 0 ? announced_period_ms_ : cfg_.hb_period_ms) / 10);
    a.hb_miss_limit = announced_miss_ != 0 ? announced_miss_ : cfg_.hb_miss_limit;
    if (send_frame(&p, p.mac, kOpHelloAck, &a, sizeof(a), false, ref_msg_id, p.node_id, false)) {
        ++tally_.hello_acks;
    }
}

void Node::fill_heartbeat(HeartbeatPayload& hb, const PeerLink* p) const {
    hb.uptime_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    hb.boot_epoch = cfg_.boot_epoch;
    hb.espnow_version = cfg_.espnow_version;
    hb.free_dram_kib =
        static_cast<uint16_t>((hal_.free_dram ? hal_.free_dram(hal_.ctx) : 0) >> 10);
    if (p != nullptr) {
        hb.tx_frames = p->tx_frames;
        hb.tx_cb_ok = p->tx_cb_ok;
        hb.tx_cb_fail = p->tx_cb_fail;
        hb.rx_frames = p->rx_frames;
        hb.rx_lost_seqgap = p->rx_lost_seqgap;
        hb.rtt_min_us_d8 = (p->rtt_samples > 0) ? quantise_us_d8(p->rtt_min_us) : kRttUnknownD8;
        hb.rtt_max_us_d8 = (p->rtt_samples > 0) ? quantise_us_d8(p->rtt_max_us) : kRttUnknownD8;
    } else {
        // A broadcast beacon is addressed to everyone, so per-link counters would be meaningless
        // and are left zero rather than filled with one arbitrary peer's numbers. The receiver
        // reads its own view of the link from its own peer slot.
        hb.rtt_min_us_d8 = kRttUnknownD8;
        hb.rtt_max_us_d8 = kRttUnknownD8;
    }
}

void Node::send_beacon() {
    // §5.3.1 / M4: liveness only, 8 bytes, one CAN frame. The link statistics travel in probes.
    BeaconPayload b{};
    b.node_id = cfg_.node_id;
    b.hb_seq = static_cast<uint16_t>(++hb_seq_bcast_);
    b.boot_epoch = cfg_.boot_epoch;
    // No ACKREQ: a broadcast gets no MAC-layer ACK, and asking N peers to reply would reintroduce
    // exactly the O(N²) cost this mode exists to remove. Header seq and msg_id are 0, because the
    // CAN single-frame profile has no room for them and the frame must decode byte-identical there.
    if (send_frame(nullptr, kBroadcastMacAddr, kOpHeartbeat, &b, sizeof(b), false, 0,
                   kNodeBroadcast, true, /*single_frame=*/true)) {
        ++tally_.beacons;
    }
}

void Node::send_probe(PeerLink& p) {
    if (p.probe_msg_id != 0) {
        // The previous probe went unanswered. Count it before overwriting, or a link that never
        // replies would show a healthy-looking zero sample count and nothing else.
        ++p.rtt_timeouts;
        emit(EventKind::ProbeTimeout, &p, p.probe_msg_id);
    }

    HeartbeatPayload hb{};
    fill_heartbeat(hb, &p);
    hb.hb_seq = ++p.hb_seq_last_tx;
    hb.hb_flags = 0;

    uint16_t msg_id = p.msg_id_next++;
    if (msg_id == 0) {  // 0 means "no correlation"
        msg_id = p.msg_id_next++;
    }

    // Record the submit instant *before* sending: on a fast link the completion callback can fire
    // before send() returns, and a probe whose start time was written afterwards yields a negative
    // RTT.
    p.probe_msg_id = msg_id;
    p.probe_submit_us = hal_.now_us ? hal_.now_us(hal_.ctx) : 0;
    p.probe_sendcb_us = 0;

    if (send_frame(&p, p.mac, kOpHeartbeat, &hb, sizeof(hb), true, msg_id, p.node_id, false)) {
        ++tally_.probes;
    } else {
        p.probe_msg_id = 0;  // never went out, so nothing is outstanding
    }
}

void Node::send_reply(PeerLink& p, uint16_t ack_of_msg_id, uint32_t recv_us) {
    HeartbeatPayload hb{};
    fill_heartbeat(hb, &p);
    hb.hb_seq = p.hb_seq_last_tx;  // a reply is not a new beacon, so the counter does not advance
    hb.ack_of_msg_id = ack_of_msg_id;
    hb.hb_flags = kHbFlagIsReply;
    // Measured wholly on our clock and sent as a duration, never an instant: the two nodes' clocks
    // are not synchronised, and durations survive that where timestamps do not.
    hb.turnaround_us = (hal_.now_us ? hal_.now_us(hal_.ctx) : 0) - recv_us;

    // ACKREQ clear: a reply that asked to be replied to would ping-pong forever.
    if (send_frame(&p, p.mac, kOpHeartbeat, &hb, sizeof(hb), false, ack_of_msg_id, p.node_id,
                   false)) {
        ++tally_.replies;
    }
}

void Node::send_bye(PeerLink& p) {
    ByePayload b{};
    b.boot_epoch = cfg_.boot_epoch;
    b.node_id = cfg_.node_id;
    b.reason = kByeReasonShutdown;
    if (send_frame(&p, p.mac, kOpBye, &b, sizeof(b), false, 0, p.node_id, false)) {
        ++tally_.byes;
    }
}

void Node::send_err(PeerLink* p, const uint8_t mac[kMacLen], uint16_t code, uint16_t ref_msg_id) {
    // §5.2: an ERR is never silently dropped. Every code this node emits fits the fixed part, so
    // none of them fragment.
    ErrPayload e{};
    e.code = code;
    e.ref_msg_id = ref_msg_id;
    e.detail_len = 0;
    const uint16_t dst = (p != nullptr) ? p->node_id : kNodeBroadcast;
    if (send_frame(p, mac, kOpErr, &e, sizeof(e), false, ref_msg_id, dst, p == nullptr)) {
        ++tally_.errs;
    }
}

// -------------------------------------------------------------------------------------------
// Receiving
// -------------------------------------------------------------------------------------------

void Node::handle_hello(PeerLink* p, const uint8_t src_mac[kMacLen], const Frame& f, int8_t rssi) {
    HelloPayload h{};
    if (!load_hello(f.payload, f.payload_len, h)) {
        ++counters_.rx_short_payload;
        send_err(p, src_mac, kErrPayloadTooShort, f.hdr.msg_id);
        return;
    }

    PeerAuth fresh{};
    AuthOutcome outcome = AuthOutcome::Legacy;
    if (!rx_via_relay_ && p == nullptr) {
        // ADR-009: a member we reached through the relay is now heard directly -- move it to its own MAC.
        PeerLink* r = relayed_peer(h.node_id);
        if (r != nullptr && (hal_.add_peer == nullptr || hal_.add_peer(hal_.ctx, src_mac))) {
            std::memcpy(r->mac, src_mac, kMacLen);
            relayed_[peers_.index_of(r)] = false;
            ++relay_counters_.path_changes;
            p = r;
        }
    }
    if (relay_ && (h.caps & kHelloCapRelay) != 0 && h.node_id < cfg_.node_id) {
        relay_ = false;  // two relays in one cell: the lower node id keeps the role (ADR-009)
        announce_change();
    }

    if (trust_ != nullptr) {
        // The cable is trusted without a signature (section 9.3) -- but a host that signs anyway
        // (M10's pot_hostnode, enrolled like a board) is verified, so this board relays its HELLO to
        // the cell, which only takes verified members (ADR-009), and holds a pair key with it. A
        // signature that fails on the cable leaves it what it always was: trusted, unverified.
        const bool cable = is_trusted_link(src_mac);
        if (!cable || f.payload_len >= kHelloSignedLen) {
            outcome = authenticate_hello(p, rx_orig_mac_, f, h, fresh);
            if (outcome == AuthOutcome::Refused || outcome == AuthOutcome::Ignore) {
                if (!cable) return;  // a refused stranger never gets a peer slot; a known peer's state is untouched
                outcome = AuthOutcome::Legacy;  // the cable: as if it had not signed
            }
        }
    }

    if (p == nullptr) {
        const size_t in_use = kMaxPeers - peers_.count_in_state(PeerState::Free);
        if (in_use >= kMaxUnicastPeers) {
            // §3's twenty-peer ceiling minus the broadcast entry. A hardware limit to report, not
            // a container to grow.
            ++counters_.peer_table_full;
            return;
        }
        if (!rx_via_relay_ && hal_.add_peer != nullptr && !hal_.add_peer(hal_.ctx, src_mac)) {
            ++counters_.peer_table_full;  // a relayed member costs no radio peer slot: it is the relay's
            return;
        }
        p = peers_.add(src_mac, h.node_id, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, cfg_.hb_period_ms,
                       cfg_.hb_miss_limit);
        if (p == nullptr) {
            ++counters_.peer_table_full;
            return;
        }
        auth_[peers_.index_of(p)] = PeerAuth{};  // a reused slot must not inherit its last tenant's key
        relayed_[peers_.index_of(p)] = rx_via_relay_;
        emit(EventKind::PeerDiscovered, p, h.boot_epoch);
        // on_rx only accounts a frame against a peer it already knew, so the HELLO that creates the
        // peer has to be accounted here or it is missing from rx_frames forever. A broadcast HELLO
        // carries the broadcast stream's seq, which must not become the unicast stream's baseline
        // (section 5.1, and on_rx's broadcast branch): it charged a board that booted into a running
        // cell with the difference -- ~12,500 phantom losses per peer (M0-LOG session 30).
        if (f.hdr.dst == kNodeBroadcast) {
            ++p->rx_frames;
        } else {
            account_rx_seq(*p, f.hdr.seq);
        }
        p->last_rssi = rssi;
    }

    bool new_incarnation = false;  // first verified contact with this peer, or it rebooted or re-enrolled
    if (outcome == AuthOutcome::Fresh) {
        PeerAuth& slot = auth_[peers_.index_of(p)];
        new_incarnation = !slot.verified || slot.epoch != fresh.epoch || slot.issued != fresh.issued;
        if (slot.verified && fresh.issued > slot.issued) {
            // Re-enrolled (a newer certificate). Its epoch counter may have restarted below the one
            // membership remembers, which would otherwise take every frame of it for a ghost.
            peer_new_lineage(*p);
            ++counters_.reboots_seen;
            emit(EventKind::PeerRebooted, p, h.boot_epoch, 1);
        }
        if (slot.verified && std::memcmp(slot.key, fresh.key, kSessionKeyLen) == 0) {
            // Same two epochs, so the same key: a HELLO that differs only in what it announces must
            // not reset the replay window, or every recent frame would become replayable.
            fresh.tx_hi = slot.tx_hi;
            fresh.rx = slot.rx;
        }
        slot = fresh;
    }
    p->node_id = h.node_id;
    peer_caps_[peers_.index_of(p)] = h.caps;
    {
        // CR-1: a peer heard one channel off (adjacent-channel leakage at close range) is really on the
        // channel it declares. Who moves: a station's channel always wins; a node that has only just
        // found the cell by scanning defers to a stable peer; two settling nodes break the tie by node
        // id; and a stable node never follows a settling one, so a scan's guess cannot drag the cell.
        const uint8_t declared = static_cast<uint8_t>((h.caps & kHelloCapChannelMask) >> kHelloCapChannelShift);
        const bool peer_fixed = (h.caps & kHelloCapChannelFixed) != 0;
        if (peer_fixed) {
            authority_id_ = h.node_id;  // M6.1: this peer's channel is its router's; follow it if it moves
            authority_declared_ = declared;
        } else if (authority_id_ == h.node_id) {
            authority_id_ = 0;
        }
        const bool peer_settling = (h.caps & kHelloCapSettling) != 0;
        const uint32_t hnow = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
        const bool me_settling = settling(hnow);
        const bool follow = peer_fixed || (me_settling && (!peer_settling || h.node_id < cfg_.node_id));
        if (declared != 0 && declared != channel_ && !cfg_.channel_fixed && declared >= cfg_.channel_lo &&
            declared <= cfg_.channel_hi && follow && hal_.set_channel != nullptr) {
            retune(declared);
            scanning_ = false;
            emit(EventKind::ChannelChanged, p, declared, 5);
        }
    }
    p->hb_period_ms = (h.hb_period_cs != 0) ? h.hb_period_cs * 10u : cfg_.hb_period_ms;
    p->miss_limit = (h.hb_miss_limit != 0) ? h.hb_miss_limit : cfg_.hb_miss_limit;
    pin_version(*p, h.espnow_version);
    note(peer_on_frame(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, h.boot_epoch), p);

    if ((h.flags & kHelloFlagWantAck) != 0 && !departed_) {
        send_hello_ack(*p, kAdmitOk, f.hdr.msg_id);
        emit(EventKind::PeerAdmitted, p);
        if (new_incarnation) {
            // First contact with this incarnation of the asker. It may still hold the key of OUR
            // previous incarnation, and then drops every unicast frame we send it -- the ack above
            // included -- as a replay; only a signed broadcast HELLO can replace that key. Without
            // this it waited for our periodic one, which a peer visiting our channel for 300 ms rarely
            // catches: the extender that rebooted onto another channel was never readmitted (M0-LOG
            // session 31). Not on every fresh verification: a HELLO's signature covers its flags, so
            // that would echo between two settled peers. Want-ack, the variant already signed and
            // cached: no extra signing here, and the asker recognises it from its cache, so it answers
            // with an ack and nothing more -- no loop.
            send_hello(true);
        }
    }
}

void Node::handle_hello_ack(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    HelloAckPayload a{};
    if (!load_hello_ack(f.payload, f.payload_len, a)) {
        ++counters_.rx_short_payload;
        return;
    }
    if (trust_required() && !is_trusted_link(p->mac)) {
        // HELLO_ACK is not signed. From a peer we have verified, for the epoch we verified, it is
        // only a liveness hint; anything else would let an unsigned frame move an epoch.
        const PeerAuth* pa = peer_auth(p);
        if (pa == nullptr || !pa->verified || a.boot_epoch != pa->epoch) {
            return;
        }
    }
    p->node_id = a.node_id;
    p->hb_period_ms = (a.hb_period_cs != 0) ? a.hb_period_cs * 10u : cfg_.hb_period_ms;
    p->miss_limit = (a.hb_miss_limit != 0) ? a.hb_miss_limit : cfg_.hb_miss_limit;
    pin_version(*p, a.espnow_version);
    note(peer_on_frame(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, a.boot_epoch), p);
}

void Node::handle_heartbeat(PeerLink* p, const Frame& f, uint32_t recv_us, bool was_broadcast) {
    if (p == nullptr) {
        return;
    }
    // The 8-byte beacon: liveness and the loss count, nothing else.
    BeaconPayload b{};
    if (load_beacon(f.payload, f.payload_len, b)) {
        if (b.node_id != p->node_id) {
            ++counters_.rx_short_payload;  // a beacon naming someone other than its sender is malformed
            return;
        }
        if (trust_required()) {
            // Unauthenticated: it may keep a verified peer alive, never announce a reboot. A new epoch
            // is believed only from a signed HELLO, which also brings the new key.
            const PeerAuth* pa = peer_auth(p);
            if (pa != nullptr && pa->verified && b.boot_epoch != pa->epoch) {
                ++auth_counters_.beacon_epoch_ignored;
                return;
            }
        }
        note(peer_on_frame(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, b.boot_epoch), p);
        ++p->rx_bcast_frames;
        account_rx_beacon_seq(*p, b.hb_seq);
        return;
    }
    HeartbeatPayload hb{};
    if (!load_heartbeat(f.payload, f.payload_len, hb)) {
        ++counters_.rx_short_payload;
        return;
    }

    pin_version(*p, hb.espnow_version);
    note(peer_on_frame(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, hb.boot_epoch), p);

    if (was_broadcast) {
        // A beacon. Its hb_seq is the sender's broadcast counter, and gaps in it are the inbound
        // loss signal for this link in BroadcastBeacon mode — the header seq cannot serve, because
        // it belongs to a different (src,dst) stream than the unicast probes.
        ++p->rx_bcast_frames;
        account_rx_hb_seq(*p, hb.hb_seq);
        return;
    }

    if ((hb.hb_flags & kHbFlagIsReply) != 0) {
        if (p->probe_msg_id != 0 && hb.ack_of_msg_id == p->probe_msg_id) {
            const uint32_t rtt_us = recv_us - p->probe_submit_us;
            const size_t idx = peers_.index_of(p);
            account_rtt(*p, idx < kMaxPeers ? &peers_.histogram(idx) : nullptr, rtt_us);

            p->remote_turnaround_last_us = hb.turnaround_us;
            if (hb.turnaround_us > p->remote_turnaround_max_us) {
                p->remote_turnaround_max_us = hb.turnaround_us;
            }
            if (p->probe_sendcb_us != 0) {
                const uint32_t txq = p->probe_sendcb_us - p->probe_submit_us;
                p->txq_last_us = txq;
                if (txq > p->txq_max_us) {
                    p->txq_max_us = txq;
                }
            }
            p->probe_msg_id = 0;
        }
        // A reply that matches nothing is not an error: it answers a probe already given up on.
        return;
    }

    // A unicast probe. Answer immediately, on this call, so the turnaround we report is a small
    // real quantity rather than an artefact of waiting for our own next beacon.
    if (f.wants_ack() && !departed_) {
        send_reply(*p, f.hdr.msg_id, recv_us);
    }
}


// -------------------------------------------------------------------------------------------
// M1: the namespace (§7.2, §4 rule 2)
// -------------------------------------------------------------------------------------------

Node::PendingNs* Node::pending_find(uint16_t msg_id, uint16_t peer_node) {
    if (msg_id == 0) {
        return nullptr;
    }
    // Keyed on both. A msg_id is unique per *peer* (section 5.1 makes seq and msg_id per link, and
    // each PeerLink has its own msg_id_next), so two peers routinely have the same msg_id
    // outstanding at once -- their sixth request each. Matching on msg_id alone frees whichever
    // slot is found first, which is a reply credited to the wrong peer and a request left to sit
    // until it times out. It never showed while the table held four entries and a coordinator
    // talked to one owner; it appeared immediately with six workers.
    for (PendingNs& q : pending_) {
        if (q.msg_id == msg_id && q.peer_node == peer_node) {
            return &q;
        }
    }
    return nullptr;
}

Node::PendingNs* Node::pending_claim() {
    for (PendingNs& q : pending_) {
        if (q.msg_id == 0) {
            return &q;
        }
    }
    return nullptr;
}

void Node::pending_expire(uint32_t now_ms) {
    for (PendingNs& q : pending_) {
        // A work unit is not a read and does not get the read timeout. The whole premise of
        // section 7.8 is that a unit runs for far longer than a round trip, so a two-second
        // deadline would write off every job worth shipping. What ends a unit is the peer dying
        // (abandon_calls_to) or the coordinator giving up on its own terms (cancel_call) -- the
        // application knows how long its work takes, and the node does not.
        if (q.op == kOpCall) {
            continue;
        }
        if (q.msg_id != 0 && (now_ms - q.sent_ms) > cfg_.ns_request_timeout_ms) {
            // A request nobody answered. Freeing the slot matters more than the counter: four
            // stuck requests would block every subsequent read, which is the same wedging failure
            // §5.4's reassembly cap has, arrived at from a different direction.
            ++ns_counters_.read_timeouts;
            const uint16_t msg_id = q.msg_id;
            const uint16_t peer = q.peer_node;
            const uint8_t op = q.op;
            q.msg_id = 0;
            if (is_deploy_opcode(op) && deploy_result_ != nullptr) {
                // A deploy client is waiting on this step; tell it rather than let it hang.
                deploy_result_(deploy_result_ctx_, peer, msg_id, op, true, nullptr, 0);
            }
        }
    }
}

NsError Node::read(uint32_t path_hash, Reading& out, bool* is_local) {
    const NsEntry* e = ns_.find(path_hash);
    if (e == nullptr && adopt_ != nullptr && adopt_(adopt_ctx_, path_hash)) {
        e = ns_.find(path_hash);  // M8.2: an actor output on another node, declared on demand
    }
    if (e == nullptr) {
        if (is_local != nullptr) {
            *is_local = false;
        }
        return NsError::NotFound;
    }

    const bool local = e->is_local(cfg_.node_id);
    if (is_local != nullptr) {
        *is_local = local;
    }

    // §4 rule 2's Unavailable outranks any cached freshness, so liveness is resolved here rather
    // than inside the namespace: a value cached a millisecond ago from a node §8.2 has declared
    // dead is not evidence about the world now.
    bool owner_alive = true;
    if (!local) {
        const PeerLink* p = peers_.find_by_node_id(e->owner_node);
        owner_alive = (p != nullptr) && (p->state == PeerState::Alive);
    }
    return ns_.read(path_hash, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, owner_alive, out);
}

NsError Node::publish(uint32_t path_hash, const Value& v) {
    return ns_.publish(path_hash, v, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0);
}

NsError Node::publish_faulty(uint32_t path_hash) {
    return ns_.publish_faulty(path_hash, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0);
}

NsError Node::write_local(uint32_t path_hash, const Value& v) {
    return ns_.write_local(path_hash, v, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0);
}

uint16_t Node::request_read(uint16_t peer_node_id, uint32_t path_hash) {
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr || departed_) {
        return 0;
    }
    PendingNs* q = pending_claim();
    if (q == nullptr) {
        return 0;  // four already outstanding; the caller retries rather than us queueing forever
    }

    uint16_t msg_id = p->msg_id_next++;
    if (msg_id == 0) {
        msg_id = p->msg_id_next++;
    }

    // Register the request *before* sending. A transport can deliver the reply inside send() --
    // a loopback, a host bridge, or simply a peer on the same core -- and a pending slot filled
    // afterwards would be filled after the answer had already been rejected as unmatched. This is
    // the same ordering hazard as the probe timestamp in send_probe(), and it bites the same way.
    q->msg_id = msg_id;
    q->peer_node = peer_node_id;
    q->path_hash = path_hash;
    q->sent_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    q->op = kOpRead;

    ReadPayload r{};
    r.path_hash = path_hash;
    if (!send_frame(p, p->mac, kOpRead, &r, sizeof(r), true, msg_id, p->node_id, false)) {
        q->msg_id = 0;  // never went out; release the slot rather than wait out its timeout
        return 0;
    }
    ++ns_counters_.reads_requested;
    return msg_id;
}

uint16_t Node::request_write(uint16_t peer_node_id, uint32_t path_hash, const Value& v) {
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr || departed_) {
        return 0;
    }
    PendingNs* q = pending_claim();
    if (q == nullptr) {
        return 0;
    }

    uint16_t msg_id = p->msg_id_next++;
    if (msg_id == 0) {
        msg_id = p->msg_id_next++;
    }

    q->msg_id = msg_id;  // before sending; see request_read()
    q->peer_node = peer_node_id;
    q->path_hash = path_hash;
    q->sent_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    q->op = kOpWrite;

    WritePayload w{};
    w.path_hash = path_hash;
    value_to_wire(v, w.value_type, w.value_len, w.value_raw);
    if (!send_frame(p, p->mac, kOpWrite, &w, sizeof(w), true, msg_id, p->node_id, false)) {
        q->msg_id = 0;
        return 0;
    }
    return msg_id;
}

void Node::handle_read(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    ReadPayload req{};
    if (!load_read(f.payload, f.payload_len, req)) {
        ++counters_.rx_short_payload;
        send_err(p, p->mac, kErrPayloadTooShort, f.hdr.msg_id);
        return;
    }

    // A read for something we hold no entry for may still be one of a peer's built-in resources:
    // their paths are a contract derived from the node id (sys_resources.hpp), so the owner, type
    // and unit are known without asking. This is how a host attached to one board reads another
    // board's sys/* -- M1's acceptance on hardware -- without v2's frame forwarding (§4: v1 paths
    // are single-hop). We hold a replica, fetched by our own single-hop READ, and answer from it
    // with its true age. Declared on demand rather than at admission: six entries for each of 19
    // peers would take 114 of §6's 128 namespace slots from the application.
    if (ns_.find(req.path_hash) == nullptr) {
        adopt_peer_sys_resource(req.path_hash);
    }

    // Node::read, not ns_.read(..., true): the owner's liveness must decide, and a replica of a dead
    // node's resource is Unavailable however recently it was cached (§4 rule 2). Answering every
    // read as if the owner were alive was harmless only while every entry was local.
    Reading r;
    bool local = true;
    const NsError st = read(req.path_hash, r, &local);

    ReplyPayload rep{};
    reply_from_reading(rep, req.path_hash, r, st);
    if (send_frame(p, p->mac, kOpReply, &rep, sizeof(rep), false, f.hdr.msg_id, p->node_id, false)) {
        ++ns_counters_.reads_served;
    }

    // Refresh the replica for the next read. The answer above is what we knew -- NoData the first
    // time, or the cached value with its age -- and is never held back waiting on the owner, so a
    // dead owner cannot stall the requester. Skipped when the requester owns the resource itself.
    if (st == NsError::Ok && !local) {
        const NsEntry* e = ns_.find(req.path_hash);
        if (e != nullptr && e->owner_node != p->node_id && e->owner_node != 0) {
            request_read(e->owner_node, req.path_hash);
        }
    }
}

bool Node::adopt_peer_sys_resource(uint32_t path_hash) {
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& q = peers_.slot(i);
        if (q.state == PeerState::Free) {
            continue;
        }
        for (size_t k = 0; k < kSysResourceCount; ++k) {
            const SysResource which = static_cast<SysResource>(k);
            if (sys_resource_hash(q.node_id, which) == path_hash) {
                return ns_.declare(sys_resource_decl(q.node_id, which)) == NsError::Ok;
            }
        }
    }
    return false;
}

void Node::handle_write(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    WritePayload req{};
    if (!load_write(f.payload, f.payload_len, req)) {
        ++counters_.rx_short_payload;
        send_err(p, p->mac, kErrPayloadTooShort, f.hdr.msg_id);
        return;
    }

    const Value v = value_from_wire(req.value_type, req.value_len, req.value_raw);
    const NsError st = write_local(req.path_hash, v);
    if (st == NsError::Ok) {
        ++ns_counters_.writes_served;
    } else {
        ++ns_counters_.writes_rejected;
    }

    // Answer whatever happened, including the refusal. §5.2 says an error is never silently
    // dropped, and a write that vanishes is worse than one that fails loudly.
    Reading r;
    ns_.read(req.path_hash, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, true, r);
    ReplyPayload rep{};
    reply_from_reading(rep, req.path_hash, r, st);
    rep.reply_to = kOpWrite;
    send_frame(p, p->mac, kOpReply, &rep, sizeof(rep), false, f.hdr.msg_id, p->node_id, false);
}

void Node::handle_reply(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    // A REPLY's payload depends on what it answers, so the request is looked up before the
    // payload is parsed: a deploy status is not a namespace reading and would fail to load as one.
    {
        PendingNs* dq = pending_find(f.hdr.msg_id, p->node_id);
        if (dq != nullptr && is_deploy_opcode(dq->op)) {
            const uint8_t op = dq->op;
            dq->msg_id = 0;
            ++ns_counters_.replies_matched;
            if (deploy_result_ != nullptr) {
                deploy_result_(deploy_result_ctx_, p->node_id, f.hdr.msg_id, op, false, f.payload,
                               f.payload_len);
            }
            return;
        }
    }
    ReplyPayload rep{};
    if (!load_reply(f.payload, f.payload_len, rep)) {
        ++counters_.rx_short_payload;
        return;
    }

    PendingNs* q = pending_find(f.hdr.msg_id, p->node_id);
    if (q == nullptr) {
        // Answers a request already given up on. Not an error - the timeout fired first - but
        // counted, because a link where this is common has a timeout set too short.
        ++ns_counters_.replies_unmatched;
        return;
    }
    const uint8_t op = q->op;
    const uint32_t asked_for = q->path_hash;
    const uint16_t worker = q->peer_node;
    q->msg_id = 0;
    ++ns_counters_.replies_matched;

    if (op == kOpCall) {
        // A call's answer is a result, not a reading: it names no resource this node holds, so it
        // goes to the coordinator and nowhere near the namespace. Caching it would invent a
        // resource whose age nobody bounds.
        if (call_result_ != nullptr) {
            const bool ok = static_cast<NsError>(rep.status) == NsError::Ok &&
                            quality_has_value(static_cast<Quality>(rep.quality));
            const Value v = ok ? value_from_wire(rep.value_type, rep.value_len, rep.value_raw)
                               : Value{};
            call_result_(result_ctx_, worker, f.hdr.msg_id, asked_for,
                         ok ? CallOutcome::Ok : CallOutcome::Refused, v);
        }
        return;
    }

    if (static_cast<NsError>(rep.status) != NsError::Ok) {
        return;  // the owner refused; nothing to cache
    }
    const Quality rq = static_cast<Quality>(rep.quality);
    if (!quality_has_value(rq) && rq != Quality::Faulty) {
        // The owner had nothing to give - unavailable, or strict-and-stale. Caching a value it
        // declined to send is not an option, because it did not send one.
        return;
    }

    // M6 (section 7.7): accept a value only from the node that owns the resource *now*. A portable
    // actor that was re-placed while a read was in flight answers from the instance consumers have
    // since fenced out, and that answer must not land -- "fenced consumers never accept the losing
    // instance". For every resource that never moves, the owner is the node asked, and this is a no-op.
    {
        const NsEntry* e = ns_.find(rep.path_hash);
        if (e != nullptr && e->owner_node != p->node_id) {
            ++ns_counters_.replies_fenced;
            return;
        }
    }

    if (rq == Quality::Faulty) {
        // M8.1: the owner says its sensor is broken, and sends no number. Until the bench showed
        // otherwise (M0-LOG session 30) this reply was dropped with the valueless ones above, so the
        // replica kept the last good number and aged it into STALE: the old number this rule forbids.
        ns_.apply_remote_faulty(rep.path_hash, rep.timestamp_ms, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0,
                                rep.age_ms);
        return;
    }

    const Value v = value_from_wire(rep.value_type, rep.value_len, rep.value_raw);
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    // The owner's timestamp is carried through untouched; the age will be measured from arrival on
    // our clock, because the two clocks are unsynchronised.
    ns_.apply_remote(rep.path_hash, v, rep.timestamp_ms, now, rep.age_ms,
                     static_cast<Quality>(rep.quality) == Quality::Faulty);
}

// -------------------------------------------------------------------------------------------
// section 7.8: work units. CALL carries a unit out, REPLY carries the result back, CAST is the
// same thing with nobody waiting.
// -------------------------------------------------------------------------------------------

// The whole request lives in one v1-profile payload. Work units are tiny in, tiny out by design:
// the arguments are a seed and a count, not a data set, and the result is a Value. Sizing the
// encode buffer at the v1 cap keeps a 1446-byte v2 buffer off the caller's stack for a payload
// nothing intends to fill; a workload that genuinely needs more should put it in the namespace,
// which is the same answer the result side gives.
static constexpr uint16_t kCallArgLimit = kMaxCallArgsV1;

size_t Node::calls_outstanding() const {
    size_t n = 0;
    for (const PendingNs& q : pending_) {
        if (q.msg_id != 0 && q.op == kOpCall) {
            ++n;
        }
    }
    return n;
}

void Node::abandon_calls_to(uint16_t peer_node_id) {
    for (PendingNs& q : pending_) {
        if (q.msg_id == 0 || q.op != kOpCall || q.peer_node != peer_node_id) {
            continue;
        }
        const uint16_t msg_id = q.msg_id;
        const uint32_t path_hash = q.path_hash;
        q.msg_id = 0;  // free the slot first: the callback may dispatch the unit again from here
        ++ns_counters_.calls_lost;
        if (call_result_ != nullptr) {
            call_result_(result_ctx_, peer_node_id, msg_id, path_hash, CallOutcome::Unavailable,
                         Value{});
        }
    }
}

bool Node::cancel_call(uint16_t msg_id) {
    for (PendingNs& q : pending_) {
        if (q.msg_id != msg_id || q.op != kOpCall) {
            continue;
        }
        const uint16_t worker = q.peer_node;
        const uint32_t path_hash = q.path_hash;
        q.msg_id = 0;  // freed first: the callback may dispatch this unit again from inside it
        ++ns_counters_.calls_lost;
        if (call_result_ != nullptr) {
            call_result_(result_ctx_, worker, msg_id, path_hash, CallOutcome::Unavailable, Value{});
        }
        return true;
    }
    return false;  // already answered, already written off, or never ours
}

uint16_t Node::request_call(uint16_t peer_node_id, uint32_t path_hash, const uint8_t* args,
                            uint16_t arg_len) {
    if (arg_len > kCallArgLimit || (arg_len != 0 && args == nullptr)) {
        return 0;
    }
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr || departed_) {
        return 0;
    }
    // Only to a peer this node believes is alive. A unit sent to a Dead peer is a unit that
    // vanishes: the frame goes nowhere, the pending slot is held, and abandon_calls_to has already
    // fired for that death, so nobody is ever told. Refusing here hands the decision back to the
    // coordinator while it still has the work -- the same principle as section 4 rule 2's
    // Unavailable outranking any cached freshness.
    if (p->state != PeerState::Alive) {
        return 0;
    }
    PendingNs* q = pending_claim();
    if (q == nullptr) {
        return 0;  // every slot outstanding; the caller still owns the work and retries
    }

    uint16_t msg_id = p->msg_id_next++;
    if (msg_id == 0) {
        msg_id = p->msg_id_next++;
    }

    // Registered before sending, for the reason request_read() gives at length: a transport can
    // deliver the reply inside send().
    q->msg_id = msg_id;
    q->peer_node = peer_node_id;
    q->path_hash = path_hash;
    q->sent_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    q->op = kOpCall;

    uint8_t buf[sizeof(CallPayload) + kCallArgLimit];
    CallPayload hdr{};
    hdr.path_hash = path_hash;
    hdr.flags = 0;
    hdr.arg_len = arg_len;
    std::memcpy(buf, &hdr, sizeof(hdr));
    if (arg_len != 0) {
        std::memcpy(buf + sizeof(hdr), args, arg_len);
    }
    const uint16_t len = static_cast<uint16_t>(sizeof(hdr) + arg_len);

    if (!send_frame(p, p->mac, kOpCall, buf, len, true, msg_id, p->node_id, false)) {
        q->msg_id = 0;
        return 0;
    }
    ++ns_counters_.calls_requested;
    return msg_id;
}

bool Node::cast(uint16_t peer_node_id, uint32_t path_hash, const uint8_t* args, uint16_t arg_len) {
    if (arg_len > kCallArgLimit || (arg_len != 0 && args == nullptr)) {
        return false;
    }
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr || departed_ || p->state != PeerState::Alive) {
        return false;  // see request_call: a unit sent to a dead peer vanishes silently
    }
    uint16_t msg_id = p->msg_id_next++;
    if (msg_id == 0) {
        msg_id = p->msg_id_next++;
    }

    uint8_t buf[sizeof(CallPayload) + kCallArgLimit];
    CallPayload hdr{};
    hdr.path_hash = path_hash;
    hdr.flags = 0;
    hdr.arg_len = arg_len;
    std::memcpy(buf, &hdr, sizeof(hdr));
    if (arg_len != 0) {
        std::memcpy(buf + sizeof(hdr), args, arg_len);
    }
    const uint16_t len = static_cast<uint16_t>(sizeof(hdr) + arg_len);
    return send_frame(p, p->mac, kOpCast, buf, len, false, msg_id, p->node_id, false);
}

bool Node::reply_call(uint16_t peer_node_id, uint16_t msg_id, uint32_t path_hash, const Value& v) {
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr) {
        // The unit was finished and the coordinator is gone. Nothing to send it to, and nothing to
        // retry: the coordinator has already written this unit off (abandon_calls_to) and given the
        // work to somebody else.
        return false;
    }
    ReplyPayload rep{};
    rep.path_hash = path_hash;
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    rep.timestamp_ms = now;
    rep.age_ms = 0;  // computed, not stale: it was produced now
    rep.unit = static_cast<uint16_t>(Unit::None);
    rep.reply_to = kOpCall;
    rep.status = static_cast<uint8_t>(NsError::Ok);
    rep.quality = static_cast<uint8_t>(Quality::Good);
    rep.latency_class = 4;  // L4: it crossed the radio to get here, whatever produced it
    value_to_wire(v, rep.value_type, rep.value_len, rep.value_raw);
    return send_frame(p, p->mac, kOpReply, &rep, sizeof(rep), false, msg_id, p->node_id, false);
}

void Node::handle_call(PeerLink* p, const Frame& f, bool wants_reply) {
    if (p == nullptr) {
        return;
    }
    CallPayload req{};
    if (!load_call(f.payload, f.payload_len, req)) {
        // Either shorter than the header, or an arg_len the frame cannot back. Rejected rather
        // than clamped: a silently truncated argument list is the same class of fault as an
        // unmarked stale value.
        ++counters_.rx_short_payload;
        send_err(p, p->mac, kErrPayloadTooShort, f.hdr.msg_id);
        return;
    }

    const uint8_t* args = f.payload + sizeof(CallPayload);
    const bool accepted =
        call_handler_ != nullptr &&
        call_handler_(call_ctx_, p->node_id, f.hdr.msg_id, req.path_hash, args, req.arg_len);

    if (accepted) {
        ++ns_counters_.calls_served;
        return;  // the result is owed, and arrives later through reply_call()
    }
    ++ns_counters_.calls_refused;

    if (!wants_reply) {
        return;  // a CAST nobody is waiting for; the counter is the only record
    }
    // Refused now rather than left to a timeout. A coordinator that gets an answer can hand the
    // unit to another worker immediately; one that gets silence has to wait out a death window
    // for a worker that is alive and simply not doing the job.
    ReplyPayload rep{};
    rep.path_hash = req.path_hash;
    rep.reply_to = kOpCall;
    rep.status = static_cast<uint8_t>(NsError::NotFound);
    rep.quality = static_cast<uint8_t>(Quality::Unavailable);
    rep.value_type = static_cast<uint8_t>(ValueType::None);
    send_frame(p, p->mac, kOpReply, &rep, sizeof(rep), false, f.hdr.msg_id, p->node_id, false);
}

void Node::handle_bye(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    ByePayload b{};
    if (!load_bye(f.payload, f.payload_len, b)) {
        ++counters_.rx_short_payload;
        return;
    }
    note(peer_on_bye(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0), p);
}

void Node::handle_err(PeerLink* p, const Frame& f) {
    ErrPayload e{};
    if (!load_err(f.payload, f.payload_len, e)) {
        ++counters_.rx_short_payload;
        return;
    }
    // §5.2: never silently dropped. Nothing for M0 to do beyond making it visible.
    emit(EventKind::BadFrame, p, e.code, e.ref_msg_id);
}

void Node::on_rx(const uint8_t src_mac[kMacLen], const uint8_t* data, size_t len, uint32_t recv_us,
                 int8_t rssi) {
    ++counters_.rx_total;

    // ADR-009: the MAC is the hop the frame came from. Which member sent it is the header's src.
    PeerLink* p = direct_peer(src_mac);
    rx_via_relay_ = false;

    // Parse against the cap pinned for this peer (§5.3); an unknown peer gets the v1 floor, which
    // is the conservative answer when we do not yet know what it can send.
    const uint16_t cap = (p != nullptr) ? p->max_payload() : kMaxPayloadV1;
    Frame f;
    const FrameError e = parse(data, len, f, cap);
    if (e != FrameError::Ok) {
        ++counters_.rx_bad_frame;
        if (p != nullptr) {
            ++p->rx_dropped_bad;
        }
        emit(EventKind::BadFrame, p, static_cast<uint32_t>(e), static_cast<uint32_t>(len));
        return;
    }

    const bool was_broadcast = (f.hdr.dst == kNodeBroadcast);

    // ---- ADR-009, the relay ----
    if (f.hdr.src == cfg_.node_id) {
        ++relay_counters_.own_echo;  // our own frame, re-broadcast to us by the relay
        return;
    }
    PeerLink* const hop = p;
    if (hop != nullptr && f.hdr.src != hop->node_id) {
        // Forwarded by `hop`. Only a declared relay forwards, and a relay never takes a relayed frame.
        if (relay_ || (peer_caps_[peers_.index_of(hop)] & kHelloCapRelay) == 0) {
            ++relay_counters_.refused;
            return;
        }
        PeerLink* d = any_peer(f.hdr.src);
        if (d != nullptr && !relayed_[peers_.index_of(d)]) {
            if (d->state == PeerState::Alive) {
                ++relay_counters_.relay_dup;  // we hear this member directly: the direct path wins
                return;
            }
            // Its direct path has died; from here on it is reached through the relay.
            std::memcpy(d->mac, hop->mac, kMacLen);
            relayed_[peers_.index_of(d)] = true;
            ++relay_counters_.path_changes;
        }
        p = relayed_peer(f.hdr.src);
        rx_via_relay_ = true;
    } else if (hop == nullptr && f.hdr.opcode == kOpHello &&
               (f.payload_len == kHelloSignedLen + kRelayMacTrailer || f.payload_len == kHelloBaseLen + kRelayMacTrailer)) {
        ++relay_counters_.refused;  // a relayed HELLO from a relay we have not admitted yet
        return;
    }
    if (relay_ && hop != nullptr && !rx_via_relay_ && !was_broadcast && f.hdr.dst != cfg_.node_id) {
        // Unicast between two members through us: forward it unchanged. Not authenticated here -- the
        // tag is under the two ends' key, which the relay does not hold, and verifies end to end.
        PeerLink* to = any_peer(f.hdr.dst);
        if (to != nullptr && to != hop && !relayed_[peers_.index_of(to)] && hal_.send != nullptr &&
            hal_.send(hal_.ctx, to->mac, data, len) == 0) {
            ++relay_counters_.unicast_forwarded;
        } else {
            ++relay_counters_.not_forwarded;
        }
        return;
    }
    const bool rebroadcast = relay_ && hop != nullptr && !rx_via_relay_ && was_broadcast &&
                             (f.hdr.opcode == kOpHeartbeat || f.hdr.opcode == kOpSafeState ||
                              f.hdr.opcode == kOpChannel);

    if (!was_broadcast && f.hdr.dst != cfg_.node_id) {
        ++counters_.rx_wrong_dst;
        return;
    }

    if (trust_required() && !is_trusted_link(src_mac)) {
        if (was_broadcast) {
            // There is no pairwise key for a broadcast. HELLO is signed, SAFE_STATE is signed (step
            // 5), and the 8-byte beacon is a liveness hint that cannot move an epoch (see
            // handle_heartbeat). Nothing else may arrive unauthenticated.
            if (f.hdr.opcode != kOpHello && f.hdr.opcode != kOpHeartbeat && f.hdr.opcode != kOpSafeState &&
                f.hdr.opcode != kOpChannel) {
                ++auth_counters_.bcast_dropped;
                return;
            }
        } else if (p != nullptr && !check_frame_auth(*p, data, f)) {
            return;
        }
    }

    if (p == nullptr) {
        ++counters_.rx_unknown_peer;
        // Only HELLO admits a stranger -- or, on a bus with no HELLO (CAN, M4), a valid beacon.
        // Anything else from an unknown MAC is noise, or a node that believes it knows us; either
        // way it introduces itself first.
        if (f.hdr.opcode == kOpHeartbeat && cfg_.admit_on_beacon) {
            p = admit_from_beacon(src_mac, f, rssi);
            if (p == nullptr) {
                return;
            }
        } else if (f.hdr.opcode != kOpHello) {
            return;
        }
    } else {
        p->last_rssi = rssi;
        if (was_broadcast) {
            // Counted, but not through account_rx_seq: §5.1 makes seq per (src,dst), so the
            // broadcast stream is a different sequence from the unicast one and folding them
            // together would invent a gap at every alternation. handle_heartbeat() accounts the
            // broadcast stream from the payload's hb_seq instead.
            ++p->rx_frames;
        } else {
            account_rx_seq(*p, f.hdr.seq);
        }
        if (hal_.on_rx_sample != nullptr) {
            RxSample s{};
            s.recv_us = recv_us;
            s.node_id = f.hdr.src;
            s.rssi = rssi;
            s.relayed = rx_via_relay_;
            s.channel = channel_;
            s.off_home = (sweeping_ && channel_ != sweep_home_) || (visiting_ && channel_ != search_home_);
            if (was_broadcast && f.hdr.opcode == kOpHeartbeat) {
                s.kind = kRxKindBeacon;
                BeaconPayload b{};
                if (load_beacon(f.payload, f.payload_len, b)) s.hb_seq = b.hb_seq;
            } else {
                s.kind = was_broadcast ? kRxKindOtherBroadcast : kRxKindUnicast;
            }
            hal_.on_rx_sample(hal_.ctx, s);
        }
    }

    switch (f.hdr.opcode) {
        case kOpHello: {
            Frame fh = f;
            if (rx_via_relay_) {
                if (f.payload_len < kRelayMacTrailer) return;
                fh.payload_len = static_cast<uint16_t>(f.payload_len - kRelayMacTrailer);
                std::memcpy(rx_orig_mac_, f.payload + fh.payload_len, kMacLen);
            } else {
                std::memcpy(rx_orig_mac_, src_mac, kMacLen);
            }
            handle_hello(p, src_mac, fh, rssi);
            if (relay_ && !rx_via_relay_) {
                // Forward a member's HELLO only once we have admitted (and, if required, verified) it.
                PeerLink* q = direct_peer(src_mac);
                const PeerAuth* qa = peer_auth(q);
                if (q != nullptr && q->node_id == f.hdr.src && (!trust_required() || (qa != nullptr && qa->verified))) {
                    relay_hello(f, src_mac);
                }
            }
            break;
        }
        case kOpHelloAck: handle_hello_ack(p, f); break;
        case kOpHeartbeat: handle_heartbeat(p, f, recv_us, was_broadcast); break;
        case kOpBye: handle_bye(p, f); break;
        case kOpChannel: handle_channel(p, f); break;
        case kOpRead: handle_read(p, f); break;
        case kOpWrite: handle_write(p, f); break;
        case kOpReply: handle_reply(p, f); break;
        case kOpCall: handle_call(p, f, true); break;
        case kOpCast: handle_call(p, f, false); break;
        case kOpErr: handle_err(p, f); break;
        case kOpDeployBegin:
        case kOpDeployChunk:
        case kOpDeployCommit:
        case kOpDeployAbort: handle_deploy(p, f); break;
        case kOpSafeState: handle_safe_state(p, f); break;
        default:
            ++counters_.rx_unknown_opcode;
            send_err(p, src_mac, kErrUnknownOpcode, f.hdr.msg_id);
            break;
    }
    if (rebroadcast && hal_.send != nullptr && hal_.send(hal_.ctx, kBroadcastMacAddr, data, len) == 0) {
        ++relay_counters_.broadcast_forwarded;  // unchanged, after our own processing of it
    }
}

void Node::on_tx_done(const uint8_t dst_mac[kMacLen], bool ok, uint32_t done_us) {
    PeerLink* p = direct_peer(dst_mac);
    if (p == nullptr) {
        // A broadcast completion. A broadcast has no MAC-layer ACK and is not a link, so it has no
        // PDR — the inbound side of a beacon is measured by its receivers, from hb_seq gaps.
        return;
    }
    if (ok) {
        ++p->tx_cb_ok;
        // A completion stamped *before* the probe was submitted belongs to an earlier frame to the
        // same peer: done_us is taken in the radio's callback and reaches this task through a queue,
        // so an older completion can be dequeued after a newer probe was submitted. Crediting it
        // made sendcb - submit go negative and wrap; the 2026-10-02 soak recorded txq_max_us =
        // 2^32 - 86 on both B-C links. The callback reports a MAC and a status, not which frame,
        // so a completion stamped after submit is still trusted; that can only make txq read short.
        if (p->probe_msg_id != 0 && p->probe_sendcb_us == 0 &&
            static_cast<int32_t>(done_us - p->probe_submit_us) >= 0) {
            p->probe_sendcb_us = done_us;
        }
    } else {
        ++p->tx_cb_fail;
    }
}

// -------------------------------------------------------------------------------------------
// Scheduling
// -------------------------------------------------------------------------------------------

void Node::start() {
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    channel_ = cfg_.channel;
    last_alive_ms_ = now;  // a node that has just booted has not yet been alone for long
    next_beacon_ms_ = now + cfg_.hb_period_ms;
    next_probe_ms_ = now + cfg_.probe_interval_ms;
    next_hello_ms_ = now;
    started_ = true;
    send_hello(true);
}

void Node::do_beacon_round(uint32_t now_ms) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& p = peers_.slot(i);
        if (p.state == PeerState::Free) {
            continue;
        }
        note(peer_on_tick(p, now_ms), &p);
    }

    if (departed_) {
        return;
    }

    if (cfg_.beacon_mode == BeaconMode::BroadcastBeacon) {
        // One frame, everyone hears it. O(N) across the cell.
        send_beacon();
        return;
    }

    // UnicastFullMesh: a probe to every peer, every period. 2·N·(N−1) frames per period across the
    // cell — retained so the simulator can measure the saturation rather than take it on trust.
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& p = peers_.slot(i);
        if (p.state == PeerState::Alive || p.state == PeerState::Dead) {
            // Dead peers keep being probed: that is how they get declared revived rather than
            // waiting for them to notice us.
            send_probe(p);
        }
    }
}

void Node::do_probe_round(uint32_t now_ms) {
    if (departed_ || cfg_.beacon_mode != BeaconMode::BroadcastBeacon) {
        return;
    }

    // Retire any probe that has outlived its deadline, so a silent peer does not hold its slot
    // forever and under-report timeouts.
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& p = peers_.slot(i);
        if (p.probe_msg_id != 0 && p.probe_submit_us != 0) {
            const uint32_t age_ms = ((hal_.now_us ? hal_.now_us(hal_.ctx) : 0) - p.probe_submit_us) / 1000u;
            if (age_ms > cfg_.probe_timeout_ms) {
                ++p.rtt_timeouts;
                emit(EventKind::ProbeTimeout, &p, p.probe_msg_id);
                p.probe_msg_id = 0;
            }
        }
    }

    // One peer per interval, round robin. Cost is 2 frames per interval per node regardless of N,
    // which is what keeps the cell inside its airtime while still measuring every link.
    for (size_t attempt = 0; attempt < kMaxPeers; ++attempt) {
        const size_t i = (probe_cursor_ + attempt) % kMaxPeers;
        PeerLink& p = peers_.slot(i);
        if (p.state == PeerState::Alive || p.state == PeerState::Dead) {
            send_probe(p);
            probe_cursor_ = (i + 1) % kMaxPeers;
            return;
        }
    }
    (void)now_ms;
}

void Node::tick(uint32_t now_ms) {
    if (!started_) {
        return;
    }

    if (static_cast<int32_t>(now_ms - next_beacon_ms_) >= 0) {
        // Advance the deadline by the period rather than from now, so the average rate stays exact
        // when a tick is late — §8.2's miss counting is in units of this period.
        next_beacon_ms_ += cfg_.hb_period_ms;
        if (static_cast<int32_t>(now_ms - next_beacon_ms_) >= 0) {
            next_beacon_ms_ = now_ms + cfg_.hb_period_ms;  // far behind; resynchronise, do not spin
        }
        do_beacon_round(now_ms);
    }

    if (static_cast<int32_t>(now_ms - next_probe_ms_) >= 0) {
        next_probe_ms_ = now_ms + cfg_.probe_interval_ms;
        do_probe_round(now_ms);
    }

    pending_expire(now_ms);

    tick_channel(now_ms);

    // M5.1: repeat an announced change, then apply a looser window once the old one has run out.
    if (announce_left_ != 0 && static_cast<int32_t>(now_ms - next_announce_ms_) >= 0) {
        --announce_left_;
        next_announce_ms_ = now_ms + 100;
        if (!departed_) send_hello(true);
    }
    if (window_apply_at_ms_ != 0 && static_cast<int32_t>(now_ms - window_apply_at_ms_) >= 0) {
        window_apply_at_ms_ = 0;
        cfg_.hb_period_ms = announced_period_ms_;
        cfg_.hb_miss_limit = announced_miss_;
        next_beacon_ms_ = now_ms + cfg_.hb_period_ms;
    }

    if (static_cast<int32_t>(now_ms - next_hello_ms_) >= 0) {
        next_hello_ms_ = now_ms + cfg_.hello_interval_ms;
        if (!departed_) {
            send_hello(true);
        }
    }
}

uint32_t Node::next_deadline_in_ms(uint32_t now_ms) const {
    if (!started_) {
        return cfg_.hb_period_ms;
    }
    uint32_t best = 0xFFFFFFFFu;
    const uint32_t announce = announce_left_ != 0 ? next_announce_ms_ : next_hello_ms_;
    const uint32_t apply = window_apply_at_ms_ != 0 ? window_apply_at_ms_ : next_hello_ms_;
    const uint32_t deadlines[] = {next_beacon_ms_, next_probe_ms_, next_hello_ms_, announce, apply};
    for (uint32_t d : deadlines) {
        const uint32_t in = (static_cast<int32_t>(d - now_ms) > 0) ? (d - now_ms) : 0;
        if (in < best) {
            best = in;
        }
    }
    return best;
}

void Node::depart() {
    if (departed_) {
        return;
    }
    departed_ = true;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        PeerLink& p = peers_.slot(i);
        if (p.state != PeerState::Free) {
            send_bye(p);
        }
    }
}

void Node::rejoin() {
    if (!departed_) {
        return;
    }
    departed_ = false;
    send_hello(true);
}

// -------------------------------------------------------------------------------------------
// §7.4 deploy: routing only. Formats and storage are pot_deploy's.
// -------------------------------------------------------------------------------------------

void Node::handle_deploy(PeerLink* p, const Frame& f) {
    if (p == nullptr) {
        return;
    }
    if (deploy_server_ == nullptr) {
        // A node built without deploy support says so, rather than leaving the host to time out.
        ++counters_.rx_unknown_opcode;
        send_err(p, p->mac, kErrUnknownOpcode, f.hdr.msg_id);
        return;
    }
    uint8_t reply[64];
    const size_t n = deploy_server_(deploy_server_ctx_, p->node_id, f.hdr.msg_id, f.hdr.opcode,
                                    f.payload, f.payload_len, reply, sizeof(reply));
    if (n > 0) {
        send_frame(p, p->mac, kOpReply, reply, static_cast<uint16_t>(n), false, f.hdr.msg_id,
                   p->node_id, false);
    }
}

bool Node::send_reply_raw(uint16_t peer_node_id, uint16_t msg_id, const uint8_t* payload,
                          uint16_t len) {
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr) {
        return false;
    }
    return send_frame(p, p->mac, kOpReply, payload, len, false, msg_id, peer_node_id, false);
}

uint16_t Node::send_deploy(uint16_t peer_node_id, uint8_t opcode, const uint8_t* payload,
                           uint16_t len) {
    if (!is_deploy_opcode(opcode)) {
        return 0;
    }
    PeerLink* p = peers_.find_by_node_id(peer_node_id);
    if (p == nullptr || p->state != PeerState::Alive || departed_) {
        return 0;
    }
    PendingNs* q = pending_claim();
    if (q == nullptr) {
        return 0;
    }
    uint16_t msg_id = p->msg_id_next++;
    if (msg_id == 0) {
        msg_id = p->msg_id_next++;
    }
    // Registered before sending, for the reason request_read() gives: a transport can deliver the
    // reply before send() returns.
    q->msg_id = msg_id;
    q->peer_node = peer_node_id;
    q->path_hash = 0;
    q->op = opcode;
    q->sent_ms = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    if (!send_frame(p, p->mac, opcode, payload, len, true, msg_id, peer_node_id, false)) {
        q->msg_id = 0;
        return 0;
    }
    return msg_id;
}

// -------------------------------------------------------------------------------------------
// M4: admission by beacon on a bus with no HELLO, and SAFE_STATE.
// -------------------------------------------------------------------------------------------

PeerLink* Node::admit_from_beacon(const uint8_t src_mac[kMacLen], const Frame& f, int8_t rssi) {
    BeaconPayload b{};
    if (!load_beacon(f.payload, f.payload_len, b) || b.node_id != f.hdr.src) {
        return nullptr;  // only a well-formed beacon that names its own sender admits anyone
    }
    const size_t in_use = kMaxPeers - peers_.count_in_state(PeerState::Free);
    if (in_use >= kMaxUnicastPeers) {
        ++counters_.peer_table_full;
        return nullptr;
    }
    if (hal_.add_peer != nullptr && !hal_.add_peer(hal_.ctx, src_mac)) {
        ++counters_.peer_table_full;
        return nullptr;
    }
    PeerLink* p = peers_.add(src_mac, b.node_id, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0,
                             cfg_.hb_period_ms, cfg_.hb_miss_limit);
    if (p == nullptr) {
        ++counters_.peer_table_full;
        return nullptr;
    }
    emit(EventKind::PeerDiscovered, p, b.boot_epoch);
    ++p->rx_frames;
    p->last_rssi = rssi;
    return p;
}

namespace {
struct SsSignJob {
    const Identity* id;
    uint16_t node_id;
    uint32_t epoch;
    const uint8_t* base;
    uint8_t* sig;
    bool ok;
};
void ss_sign_job(void* a) {
    SsSignJob& j = *static_cast<SsSignJob*>(a);
    j.ok = safe_state_sign(*j.id, j.node_id, j.epoch, j.base, j.sig);
}
struct SsVerifyJob {
    const uint8_t* pub;
    uint16_t node_id;
    uint32_t epoch;
    const uint8_t* base;
    const uint8_t* sig;
    bool ok;
};
void ss_verify_job(void* a) {
    SsVerifyJob& j = *static_cast<SsVerifyJob*>(a);
    j.ok = safe_state_verify(j.pub, j.node_id, j.epoch, j.base, j.sig);
}
uint32_t rd32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

Node::SafeStateFloor* Node::ss_floor_for(uint16_t node_id, bool create) {
    SafeStateFloor* free_slot = nullptr;
    for (SafeStateFloor& e : ss_floor_) {
        if (e.node_id == node_id) return &e;
        if (e.node_id == 0 && free_slot == nullptr) free_slot = &e;
    }
    if (!create || free_slot == nullptr) return nullptr;
    *free_slot = SafeStateFloor{};
    free_slot->node_id = node_id;
    return free_slot;
}

void Node::restore_safe_state_floors(const void* table, size_t bytes) {
    if (table == nullptr || bytes != sizeof(ss_floor_)) return;
    std::memcpy(ss_floor_, table, sizeof(ss_floor_));
    for (SafeStateFloor& e : ss_floor_) e.last_try_ms = 0;
}

uint32_t Node::send_safe_state(uint16_t reason) {
    SafeStatePayload s{};
    s.counter = ++safe_state_tx_;
    s.reason = reason;
    if (trust_ != nullptr && trust_->enrolled) {
        // Step 5: signed and epoch-fenced, 76 bytes, one ESP-NOW frame.
        uint8_t buf[kSafeStateSignedLen];
        std::memcpy(buf, &s, kSafeStateBaseLen);
        const uint32_t ep = cfg_.boot_epoch;
        std::memcpy(buf + kSafeStateBaseLen, &ep, 4);
        SsSignJob job{trust_, cfg_.node_id, ep, buf, buf + kSafeStateBaseLen + 4, false};
        heavy(&ss_sign_job, &job);
        if (!job.ok || !send_frame(nullptr, kBroadcastMacAddr, kOpSafeState, buf, static_cast<uint16_t>(sizeof(buf)),
                                   false, 0, kNodeBroadcast, true, /*single_frame=*/false, kClassL0, kPriorityMax)) {
            return 0;
        }
        return s.counter;
    }
    if (!send_frame(nullptr, kBroadcastMacAddr, kOpSafeState, &s, sizeof(s), false, 0, kNodeBroadcast,
                    true, /*single_frame=*/true, kClassL0, kPriorityMax)) {
        return 0;
    }
    return s.counter;
}

void Node::handle_safe_state(PeerLink* p, const Frame& f) {
    SafeStatePayload s{};
    if (!load_safe_state(f.payload, f.payload_len, s)) {
        ++counters_.rx_short_payload;
        return;
    }
    if (trust_required()) {
        // Section 8.3: signed, and fenced by the sender's (epoch, counter) high-water, persisted.
        if (f.payload_len != kSafeStateSignedLen) {
            ++ss_counters_.unsigned_refused;
            emit(EventKind::SafeStateRejected, p, 1, s.counter);
            return;
        }
        const PeerAuth* pa = peer_auth(p);
        if (p == nullptr || pa == nullptr || !pa->verified || p->node_id != f.hdr.src) {
            ++ss_counters_.unknown_sender;  // only an enrolled, verified source can halt this node
            emit(EventKind::SafeStateRejected, p, 2, s.counter);
            return;
        }
        const uint32_t ep = rd32le(f.payload + kSafeStateBaseLen);
        SafeStateFloor* fl = ss_floor_for(f.hdr.src, true);
        // Two bytes of the verifying key (never 0, which means "not known"). A floor set under another
        // key belongs to a lineage that ended when the sender was re-enrolled: nothing it signed can
        // verify under the key we now hold, so the fence restarts rather than refusing the new one.
        const uint16_t ktag = static_cast<uint16_t>(pa->pub[0] | (pa->pub[1] << 8)) == 0
                                  ? uint16_t{1}
                                  : static_cast<uint16_t>(pa->pub[0] | (pa->pub[1] << 8));
        if (fl != nullptr && fl->key_tag != 0 && fl->key_tag != ktag) {
            fl->epoch = 0;
            fl->counter = 0;
            fl->key_tag = 0;
        }
        if (fl != nullptr && (ep < fl->epoch || (ep == fl->epoch && s.counter <= fl->counter))) {
            // Cheap, and before the signature: an old frame is refused without costing a verify.
            ++ss_counters_.replayed;
            emit(EventKind::SafeStateRejected, p, 4, s.counter);
            return;
        }
        const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
        if (fl != nullptr && fl->last_try_ms != 0 && now - fl->last_try_ms < 100) {
            ++ss_counters_.rate_limited;  // at most ten verifications a second per source
            emit(EventKind::SafeStateRejected, p, 5, s.counter);
            return;
        }
        if (fl != nullptr) fl->last_try_ms = (now == 0) ? 1 : now;
        const uint32_t t0 = hal_.now_us ? hal_.now_us(hal_.ctx) : 0;
        SsVerifyJob job{pa->pub, f.hdr.src, ep, f.payload, f.payload + kSafeStateBaseLen + 4, false};
        heavy(&ss_verify_job, &job);
        const uint32_t took = (hal_.now_us ? hal_.now_us(hal_.ctx) : 0) - t0;
        if (took > ss_counters_.verify_us_max) ss_counters_.verify_us_max = took;
        if (!job.ok) {
            ++ss_counters_.bad_signature;
            emit(EventKind::SafeStateRejected, p, 3, s.counter);
            return;
        }
        if (fl != nullptr) {
            fl->epoch = ep;
            fl->counter = s.counter;
            fl->key_tag = ktag;
            if (hal_.persist_safe_state_floors != nullptr) {
                hal_.persist_safe_state_floors(hal_.ctx, ss_floor_, sizeof(ss_floor_));
            }
        }
        ++ss_counters_.accepted;
    }
    emit(EventKind::SafeStateRx, p, s.counter, s.reason);
    if (safe_state_fn_ != nullptr) {
        safe_state_fn_(safe_state_ctx_, f.hdr.src, s.counter, s.reason);
    }
}

}  // namespace pot
