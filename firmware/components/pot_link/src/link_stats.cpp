// Per-peer link statistics implementation — see link_stats.hpp, especially the delay methodology.

#include "pot/link_stats.hpp"

#include <cstring>

#include "pot/frame.hpp"

namespace pot {

const char* peer_state_str(PeerState s) {
    switch (s) {
        case PeerState::Free: return "free";
        case PeerState::Known: return "known";
        case PeerState::Alive: return "alive";
        case PeerState::Dead: return "dead";
        case PeerState::Left: return "left";
    }
    return "unknown";
}

const char* event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::None: return "none";
        case EventKind::PeerDiscovered: return "peer_discovered";
        case EventKind::PeerAdmitted: return "peer_admitted";
        case EventKind::PeerAlive: return "peer_alive";
        case EventKind::PeerDead: return "peer_dead";
        case EventKind::PeerRevived: return "peer_revived";
        case EventKind::PeerRebooted: return "peer_rebooted";
        case EventKind::PeerLeft: return "peer_left";
        case EventKind::BadFrame: return "bad_frame";
        case EventKind::TxError: return "tx_error";
        case EventKind::ProbeTimeout: return "probe_timeout";
        case EventKind::VersionPinned: return "version_pinned";
    }
    return "unknown";
}

// -------------------------------------------------------------------------------------------
// RttHistogram
// -------------------------------------------------------------------------------------------

void RttHistogram::reset() { std::memset(bucket, 0, sizeof(bucket)); }

void RttHistogram::add(uint32_t us) {
    for (size_t i = 0; i < kRttBuckets; ++i) {
        if (us <= kRttBucketEdgeUs[i]) {
            // Saturate rather than wrap. A 24-hour soak at 100 ms is 864,000 samples per link, so
            // u32 has four orders of magnitude of headroom; saturating anyway costs one compare
            // and means the number is never silently wrong.
            if (bucket[i] != 0xFFFFFFFFu) {
                ++bucket[i];
            }
            return;
        }
    }
}

uint32_t RttHistogram::count() const {
    uint32_t n = 0;
    for (size_t i = 0; i < kRttBuckets; ++i) {
        n += bucket[i];
    }
    return n;
}

bool RttHistogram::percentile(uint8_t pct, uint32_t& lo_us, uint32_t& hi_us) const {
    const uint32_t total = count();
    if (total == 0) {
        return false;
    }
    if (pct > 100) {
        pct = 100;
    }

    // Rank of the requested percentile, 1-based, rounded up: p50 of 4 samples is the 2nd, p99 of
    // 100 is the 99th, p100 is the last. Integer arithmetic throughout — no float in firmware.
    uint32_t rank = static_cast<uint32_t>((static_cast<uint64_t>(total) * pct + 99) / 100);
    if (rank == 0) {
        rank = 1;
    }

    uint32_t cumulative = 0;
    for (size_t i = 0; i < kRttBuckets; ++i) {
        cumulative += bucket[i];
        if (cumulative >= rank) {
            lo_us = (i == 0) ? 0 : kRttBucketEdgeUs[i - 1];
            hi_us = kRttBucketEdgeUs[i];
            return true;
        }
    }

    // Unreachable while `total` and the buckets agree, but returning the top interval is better
    // than leaving the out-params untouched if they ever disagree.
    lo_us = kRttBucketEdgeUs[kRttBuckets - 2];
    hi_us = kRttBucketEdgeUs[kRttBuckets - 1];
    return true;
}

// -------------------------------------------------------------------------------------------
// PeerLink
// -------------------------------------------------------------------------------------------

void PeerLink::reset() {
    std::memset(this, 0, sizeof(*this));
    state = PeerState::Free;
    version = EspNowVersion::Unknown;
    rtt_min_us = 0xFFFFFFFFu;  // so the first sample always wins the minimum
}

uint16_t PeerLink::max_payload() const {
    // §5.3: pin to the 226 B floor unless the peer is known to be v2. Unknown means unknown — a
    // v1 receiver handed a longer frame truncates it silently, which the receiver's total_len
    // check will reject, but the frame is still lost and the loss looks like a link problem.
    return version == EspNowVersion::V2 ? kMaxPayloadV2 : kMaxPayloadV1;
}

bool PeerLink::pdr_tx_ppm(uint32_t& ppm) const {
    // Outbound delivery is measured from the send callback, which reports the 802.11 MAC-layer
    // ACK. tx_enqueue_err is excluded from the denominator on purpose: a frame the transport
    // refused to accept was never transmitted, so counting it as a delivery failure would blame
    // the radio for a local queue overflow.
    const uint64_t attempted = static_cast<uint64_t>(tx_cb_ok) + tx_cb_fail;
    if (attempted == 0) {
        return false;
    }
    ppm = static_cast<uint32_t>((static_cast<uint64_t>(tx_cb_ok) * 1000000u) / attempted);
    return true;
}

bool PeerLink::pdr_rx_ppm(uint32_t& ppm) const {
    // Inbound delivery is inferred from seq gaps (§5.1). The denominator is what the peer must
    // have sent for the seq numbers we saw to make sense.
    const uint64_t expected = static_cast<uint64_t>(rx_frames) + rx_lost_seqgap;
    if (expected == 0) {
        return false;
    }
    ppm = static_cast<uint32_t>((static_cast<uint64_t>(rx_frames) * 1000000u) / expected);
    return true;
}

// -------------------------------------------------------------------------------------------
// NodeCounters
// -------------------------------------------------------------------------------------------

void NodeCounters::reset() { std::memset(this, 0, sizeof(*this)); }

// -------------------------------------------------------------------------------------------
// EventRing
// -------------------------------------------------------------------------------------------

void EventRing::reset() {
    std::memset(buf_, 0, sizeof(buf_));
    head_ = 0;
    tail_ = 0;
    dropped_ = 0;
}

void EventRing::push(const Event& e) {
    buf_[head_ % kEventRingSize] = e;
    ++head_;
    if (head_ - tail_ > kEventRingSize) {
        // Overwrite-oldest, and count it. A full event ring must never be able to stall a
        // heartbeat, so there is no back-pressure here — but a silent overwrite would mean the
        // JSON stream lies about how many events happened.
        tail_ = head_ - kEventRingSize;
        ++dropped_;
    }
}

bool EventRing::pop(Event& out) {
    if (tail_ == head_) {
        return false;
    }
    out = buf_[tail_ % kEventRingSize];
    ++tail_;
    return true;
}

size_t EventRing::size() const { return head_ - tail_; }

// -------------------------------------------------------------------------------------------
// Sequence accounting
// -------------------------------------------------------------------------------------------

void account_rx_seq(PeerLink& p, uint16_t seq) {
    ++p.rx_frames;

    if (!p.seq_rx_valid) {
        // First frame from this peer: it establishes the baseline and proves nothing about loss.
        // Without this flag, a peer whose first seq is 5 would be charged with 5 lost frames.
        p.seq_rx_valid = true;
        p.seq_rx_last = seq;
        return;
    }

    // §5.1: seq wraps. Interpreting the difference as a signed 16-bit quantity makes 0xFFFF → 0x0000
    // a delta of +1 rather than −65535, and makes a genuinely late frame a small negative number
    // rather than an enormous positive one.
    const int16_t delta = static_cast<int16_t>(static_cast<uint16_t>(seq - p.seq_rx_last));
    if (delta > 0) {
        p.rx_lost_seqgap += static_cast<uint32_t>(delta - 1);
        p.seq_rx_last = seq;
    } else {
        // Backwards or repeated. Either a duplicate from ESP-NOW's retransmission machinery or a
        // reorder; both are real, neither is a loss, and seq_rx_last must not move backwards or
        // the next in-order frame would be counted as a huge gap.
        ++p.rx_reorder_dup;
    }
}

void account_rx_hb_seq(PeerLink& p, uint32_t hb_seq) {
    // hb_seq is 32-bit and does not wrap within any plausible uptime (a 100 ms period reaches
    // 2^32 after 13.6 years), so the arithmetic is simpler than for seq.
    if (p.hb_seq_last != 0 && hb_seq > p.hb_seq_last + 1) {
        p.rx_hb_lost_seqgap += hb_seq - p.hb_seq_last - 1;
    }
    if (hb_seq > p.hb_seq_last) {
        p.hb_seq_last = hb_seq;
    }
}

void account_rtt(PeerLink& p, RttHistogram* histogram, uint32_t rtt_us) {
    ++p.rtt_samples;
    if (rtt_us < p.rtt_min_us) {
        p.rtt_min_us = rtt_us;
    }
    if (rtt_us > p.rtt_max_us) {
        p.rtt_max_us = rtt_us;
    }
    if (histogram != nullptr) {
        histogram->add(rtt_us);
    }
}

}  // namespace pot
