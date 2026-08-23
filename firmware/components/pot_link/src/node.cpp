// The Potluck node policy — see node.hpp.

#include "pot/node.hpp"

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
                      bool broadcast) {
    EncodeSpec spec;
    spec.src = cfg_.node_id;
    spec.dst = dst_override;
    spec.opcode = opcode;
    spec.lclass = kClassL3;
    spec.priority = kMembershipPriority;
    // §5.1: seq is per (src,dst). Broadcast is a distinct destination and keeps its own counter.
    spec.seq = broadcast ? seq_tx_bcast_++ : (p != nullptr ? p->seq_tx++ : 0);
    spec.msg_id = msg_id;
    spec.ack_req = ack_req;
    // AUTH stays clear until M5. §14 reserves the eight bytes in §5.3's MTU arithmetic from day
    // one; emitting a zeroed tag now would look authenticated to a receiver that had started
    // checking.
    spec.auth = false;

    const uint16_t cap = (p != nullptr) ? p->max_payload() : kMaxPayloadV1;
    if (payload_len > cap) {
        return false;
    }

    size_t written = 0;
    if (encode(spec, static_cast<const uint8_t*>(payload), payload_len, tx_, sizeof(tx_), written) !=
        FrameError::Ok) {
        return false;
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

void Node::send_hello(bool want_ack) {
    HelloPayload h{};
    h.boot_epoch = cfg_.boot_epoch;
    h.caps = 0;  // M1+ owns the capability bitfield
    h.node_id = cfg_.node_id;
    h.espnow_version = cfg_.espnow_version;
    h.hb_period_cs = static_cast<uint8_t>(cfg_.hb_period_ms / 10);
    h.hb_miss_limit = cfg_.hb_miss_limit;
    h.flags = want_ack ? kHelloFlagWantAck : 0;
    // pubkey_fp stays zero until M5.
    if (send_frame(nullptr, kBroadcastMacAddr, kOpHello, &h, sizeof(h), false, 0, kNodeBroadcast,
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
    a.hb_period_cs = static_cast<uint8_t>(cfg_.hb_period_ms / 10);
    a.hb_miss_limit = cfg_.hb_miss_limit;
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
    HeartbeatPayload hb{};
    fill_heartbeat(hb, nullptr);
    hb.hb_seq = ++hb_seq_bcast_;
    hb.hb_flags = 0;
    // No ACKREQ: a broadcast gets no MAC-layer ACK, and asking N peers to reply would reintroduce
    // exactly the O(N²) cost this mode exists to remove.
    if (send_frame(nullptr, kBroadcastMacAddr, kOpHeartbeat, &hb, sizeof(hb), false, 0,
                   kNodeBroadcast, true)) {
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

    if (p == nullptr) {
        const size_t in_use = kMaxPeers - peers_.count_in_state(PeerState::Free);
        if (in_use >= kMaxUnicastPeers) {
            // §3's twenty-peer ceiling minus the broadcast entry. A hardware limit to report, not
            // a container to grow.
            ++counters_.peer_table_full;
            return;
        }
        if (hal_.add_peer != nullptr && !hal_.add_peer(hal_.ctx, src_mac)) {
            ++counters_.peer_table_full;
            return;
        }
        p = peers_.add(src_mac, h.node_id, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, cfg_.hb_period_ms,
                       cfg_.hb_miss_limit);
        if (p == nullptr) {
            ++counters_.peer_table_full;
            return;
        }
        emit(EventKind::PeerDiscovered, p, h.boot_epoch);
        // on_rx only accounts a frame against a peer it already knew, so the HELLO that creates the
        // peer has to be accounted here or it is missing from rx_frames forever.
        account_rx_seq(*p, f.hdr.seq);
        p->last_rssi = rssi;
    }

    p->node_id = h.node_id;
    p->hb_period_ms = (h.hb_period_cs != 0) ? h.hb_period_cs * 10u : cfg_.hb_period_ms;
    p->miss_limit = (h.hb_miss_limit != 0) ? h.hb_miss_limit : cfg_.hb_miss_limit;
    pin_version(*p, h.espnow_version);
    note(peer_on_frame(*p, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, h.boot_epoch), p);

    if ((h.flags & kHelloFlagWantAck) != 0 && !departed_) {
        send_hello_ack(*p, kAdmitOk, f.hdr.msg_id);
        emit(EventKind::PeerAdmitted, p);
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

Node::PendingNs* Node::pending_find(uint16_t msg_id) {
    if (msg_id == 0) {
        return nullptr;
    }
    for (PendingNs& q : pending_) {
        if (q.msg_id == msg_id) {
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
            q.msg_id = 0;
        }
    }
}

NsError Node::read(uint32_t path_hash, Reading& out, bool* is_local) {
    const NsEntry* e = ns_.find(path_hash);
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

    Reading r;
    // Answering for a resource we own: our own clock, and alive by definition since we are running.
    const NsError st = ns_.read(req.path_hash, hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0, true, r);

    ReplyPayload rep{};
    reply_from_reading(rep, req.path_hash, r, st);
    if (send_frame(p, p->mac, kOpReply, &rep, sizeof(rep), false, f.hdr.msg_id, p->node_id, false)) {
        ++ns_counters_.reads_served;
    }
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
    ReplyPayload rep{};
    if (!load_reply(f.payload, f.payload_len, rep)) {
        ++counters_.rx_short_payload;
        return;
    }

    PendingNs* q = pending_find(f.hdr.msg_id);
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
    if (!quality_has_value(static_cast<Quality>(rep.quality))) {
        // The owner had nothing to give - unavailable, strict-and-stale, or faulty. Caching a
        // value it declined to send is not an option, because it did not send one.
        return;
    }

    const Value v = value_from_wire(rep.value_type, rep.value_len, rep.value_raw);
    const uint32_t now = hal_.now_ms ? hal_.now_ms(hal_.ctx) : 0;
    // The owner's timestamp is carried through untouched; the age will be measured from arrival on
    // our clock, because the two clocks are unsynchronised.
    ns_.apply_remote(rep.path_hash, v, rep.timestamp_ms, now,
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
    if (p == nullptr || departed_) {
        return false;
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

    PeerLink* p = peers_.find_by_mac(src_mac);

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
    if (!was_broadcast && f.hdr.dst != cfg_.node_id) {
        ++counters_.rx_wrong_dst;
        return;
    }

    if (p == nullptr) {
        ++counters_.rx_unknown_peer;
        // Only HELLO admits a stranger. Anything else from an unknown MAC is noise, or a node that
        // believes it knows us; either way it introduces itself first.
        if (f.hdr.opcode != kOpHello) {
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
    }

    switch (f.hdr.opcode) {
        case kOpHello: handle_hello(p, src_mac, f, rssi); break;
        case kOpHelloAck: handle_hello_ack(p, f); break;
        case kOpHeartbeat: handle_heartbeat(p, f, recv_us, was_broadcast); break;
        case kOpBye: handle_bye(p, f); break;
        case kOpRead: handle_read(p, f); break;
        case kOpWrite: handle_write(p, f); break;
        case kOpReply: handle_reply(p, f); break;
        case kOpCall: handle_call(p, f, true); break;
        case kOpCast: handle_call(p, f, false); break;
        case kOpErr: handle_err(p, f); break;
        default:
            ++counters_.rx_unknown_opcode;
            send_err(p, src_mac, kErrUnknownOpcode, f.hdr.msg_id);
            break;
    }
}

void Node::on_tx_done(const uint8_t dst_mac[kMacLen], bool ok, uint32_t done_us) {
    PeerLink* p = peers_.find_by_mac(dst_mac);
    if (p == nullptr) {
        // A broadcast completion. A broadcast has no MAC-layer ACK and is not a link, so it has no
        // PDR — the inbound side of a beacon is measured by its receivers, from hb_seq gaps.
        return;
    }
    if (ok) {
        ++p->tx_cb_ok;
        if (p->probe_msg_id != 0 && p->probe_sendcb_us == 0) {
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
    const uint32_t deadlines[] = {next_beacon_ms_, next_probe_ms_, next_hello_ms_};
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

}  // namespace pot
