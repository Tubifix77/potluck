// M15: telemetry over the cell. See pot/telemetry.hpp.

#include "pot/telemetry.hpp"

#include <initializer_list>

namespace pot {

namespace {

struct W {
    uint8_t* p;
    size_t at;
    void u8(uint8_t v) { p[at++] = v; }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
};

uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(v); }

uint16_t pct_us100(const RttHistogram& h, uint8_t pct) {
    uint32_t lo = 0, hi = 0;
    if (!h.percentile(pct, lo, hi) || hi == 0xFFFFFFFFu) return 0xFFFF;
    const uint32_t v = (hi + 99) / 100;
    return v >= 0xFFFFu ? 0xFFFE : static_cast<uint16_t>(v);
}

size_t header(W& w, uint8_t section, uint16_t node) {
    w.u8(kTelemetryVersion);
    w.u8(section);
    w.u16(node);
    return w.at;
}

}  // namespace

bool load_telemetry_request(const uint8_t* p, size_t len, TelemetryRequest& out) {
    if (p == nullptr || len < kTelemetryRequestLen) return false;
    out.target = static_cast<uint16_t>(p[0] | (p[1] << 8));
    out.section = p[2];
    out.start = p[3];
    out.since = static_cast<uint32_t>(p[4]) | (static_cast<uint32_t>(p[5]) << 8) | (static_cast<uint32_t>(p[6]) << 16) |
                (static_cast<uint32_t>(p[7]) << 24);
    return true;
}

size_t store_telemetry_request(const TelemetryRequest& r, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kTelemetryRequestLen) return 0;
    W w{out, 0};
    w.u16(r.target);
    w.u8(r.section);
    w.u8(r.start);
    w.u32(r.since);
    return w.at;
}

TelemetryLink telemetry_link(const PeerLink& p, const RttHistogram& h, uint8_t flags) {
    TelemetryLink l;
    l.node = p.node_id;
    l.state = static_cast<uint8_t>(p.state);
    l.rssi = p.last_rssi;
    l.espnow_version = static_cast<uint8_t>(p.version);
    l.flags = flags;
    l.hb_period_ms = sat16(p.hb_period_ms);
    l.miss_limit = p.miss_limit;
    l.misses = p.misses;
    l.tx_frames = p.tx_frames;
    l.tx_cb_ok = p.tx_cb_ok;
    l.tx_cb_fail = p.tx_cb_fail;
    l.rx_frames = p.rx_frames;
    l.rx_lost = p.rx_lost_seqgap + p.rx_hb_lost_seqgap;
    l.rx_bcast = p.rx_bcast_frames;
    l.rtt_p50_us100 = pct_us100(h, 50);
    l.rtt_p99_us100 = pct_us100(h, 99);
    l.rtt_timeouts = sat16(p.rtt_timeouts);
    return l;
}

size_t store_telemetry_health(uint16_t node, const TelemetryHealth& h, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kTelemetryHeaderLen + kTelemetryHealthLen) return 0;
    W w{out, 0};
    header(w, kTelemetryHealth, node);
    w.u32(h.uptime_s);
    w.u32(h.boot_epoch);
    w.u8(h.channel);
    w.u8(h.flags);
    w.u16(h.hb_period_ms);
    w.u32(h.int_free);
    w.u32(h.int_min);
    w.u32(h.int_largest);
    w.u32(h.ext_free);
    w.u32(h.ext_min);
    w.u8(h.peers_alive);
    w.u8(h.peers_dead);
    w.u8(h.actors_pinned);
    w.u8(h.actors_portable_here);
    for (uint32_t v : {h.deaths, h.revivals, h.reboots_seen, h.rx_total, h.rx_rejected, h.rx_queue_dropped, h.tx_total,
                       h.tx_enqueue_err, h.tx_done_dropped, h.events_dropped, h.writes_served, h.writes_rejected}) {
        w.u32(v);
    }
    w.u16(h.link_stack_free);
    w.u16(0);
    return w.at;
}

size_t store_telemetry_links(uint16_t node, uint8_t total, uint8_t start, const TelemetryLink* l, size_t n,
                             uint8_t* out, size_t cap) {
    if (n > kTelemetryLinksPerReply) n = kTelemetryLinksPerReply;
    if (out == nullptr || cap < kTelemetryHeaderLen + 3 + n * kTelemetryLinkLen) return 0;
    W w{out, 0};
    header(w, kTelemetryLinks, node);
    w.u8(total);
    w.u8(start);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const TelemetryLink& k = l[i];
        w.u16(k.node);
        w.u8(k.state);
        w.u8(static_cast<uint8_t>(k.rssi));
        w.u8(k.espnow_version);
        w.u8(k.flags);
        w.u16(k.hb_period_ms);
        w.u8(k.miss_limit);
        w.u8(k.misses);
        for (uint32_t v : {k.tx_frames, k.tx_cb_ok, k.tx_cb_fail, k.rx_frames, k.rx_lost, k.rx_bcast}) w.u32(v);
        w.u16(k.rtt_p50_us100);
        w.u16(k.rtt_p99_us100);
        w.u16(k.rtt_timeouts);
    }
    return w.at;
}

size_t store_telemetry_actors(uint16_t node, uint8_t total, uint8_t start, const TelemetryActor* a, size_t n,
                              uint8_t* out, size_t cap) {
    if (n > kTelemetryActorsPerReply) n = kTelemetryActorsPerReply;
    if (out == nullptr || cap < kTelemetryHeaderLen + 3 + n * kTelemetryActorLen) return 0;
    W w{out, 0};
    header(w, kTelemetryActors, node);
    w.u8(total);
    w.u8(start);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.u32(a[i].key);
        w.u8(a[i].flags);
        w.u8(0);
        w.u16(a[i].owner);
        w.u16(a[i].assigned);
        w.u32(a[i].activations);
        w.u16(a[i].fenced);
        w.u16(a[i].released);
    }
    return w.at;
}

size_t store_telemetry_events(uint16_t node, uint32_t next, const TelemetryEvent* e, size_t n, uint8_t* out,
                              size_t cap) {
    if (n > kTelemetryEventsPerReply) n = kTelemetryEventsPerReply;
    if (out == nullptr || cap < kTelemetryHeaderLen + 5 + n * kTelemetryEventLen) return 0;
    W w{out, 0};
    header(w, kTelemetryEvents, node);
    w.u32(next);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.u32(e[i].seq);
        w.u32(e[i].at_ms);
        w.u8(e[i].kind);
        w.u16(e[i].node);
        w.u32(e[i].a);
        w.u32(e[i].b);
    }
    return w.at;
}

void TelemetryEventLog::push(uint32_t at_ms, uint8_t kind, uint16_t node, uint32_t a, uint32_t b) {
    TelemetryEvent& e = buf_[next_ % kSize];
    e.seq = next_++;
    if (next_ == 0) next_ = 1;
    e.at_ms = at_ms;
    e.kind = kind;
    e.node = node;
    e.a = a;
    e.b = b;
}

size_t TelemetryEventLog::since(uint32_t since, TelemetryEvent* out, size_t max) const {
    // The kept window is the last kSize seqs: (next_ - kSize, next_).
    const uint32_t oldest = next_ > kSize ? next_ - static_cast<uint32_t>(kSize) : 1;
    uint32_t from = since + 1 > oldest ? since + 1 : oldest;
    size_t n = 0;
    for (uint32_t s = from; s < next_ && n < max; ++s) {
        const TelemetryEvent& e = buf_[s % kSize];
        if (e.seq == s) out[n++] = e;
    }
    return n;
}

}  // namespace pot
