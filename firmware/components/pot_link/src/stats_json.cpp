// Serial stats format — see stats_json.hpp for the format rules.

#include "pot/stats_json.hpp"

#include <cstdio>
#include <cstring>

namespace pot {
namespace {

// A bounded append helper. Every writer below is a sequence of these, and each one checks the
// remaining space, so a line that would overflow returns 0 rather than emitting a truncated object
// that the reader would have to guess about.
struct Sink {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool ok = true;

    void put(const char* s) {
        if (!ok) return;
        const size_t n = std::strlen(s);
        if (len + n + 1 > cap) {
            ok = false;
            return;
        }
        std::memcpy(buf + len, s, n);
        len += n;
        buf[len] = '\0';
    }

    void u32(uint32_t v) {
        char tmp[12];
        std::snprintf(tmp, sizeof(tmp), "%u", static_cast<unsigned>(v));
        put(tmp);
    }

    void i32(int32_t v) {
        char tmp[13];
        std::snprintf(tmp, sizeof(tmp), "%d", static_cast<int>(v));
        put(tmp);
    }

    // "key":value for an unsigned integer.
    void kv(const char* key, uint32_t v) {
        put("\"");
        put(key);
        put("\":");
        u32(v);
    }

    void kv_i(const char* key, int32_t v) {
        put("\"");
        put(key);
        put("\":");
        i32(v);
    }

    void kv_s(const char* key, const char* v) {
        put("\"");
        put(key);
        put("\":");
        if (v == nullptr) {
            put("null");
        } else {
            put("\"");
            put(v);  // no escaping: every string this module emits is a literal or a MAC
            put("\"");
        }
    }

    // An unsigned value that may be unknown. §13-M0 wants measured numbers, so "not measured" has
    // to be representable as something other than zero.
    void kv_opt(const char* key, bool known, uint32_t v) {
        put("\"");
        put(key);
        put("\":");
        if (known) {
            u32(v);
        } else {
            put("null");
        }
    }

    void comma() { put(","); }
};

// Emit a percentile as [lo,hi] or null. A histogram cannot support a single number, and printing
// one would be the sort of invented precision §13-M0 rules out.
void put_percentile(Sink& s, const char* key, const RttHistogram& h, uint8_t pct) {
    s.put("\"");
    s.put(key);
    s.put("\":");
    uint32_t lo = 0, hi = 0;
    if (!h.percentile(pct, lo, hi)) {
        s.put("null");
        return;
    }
    s.put("[");
    s.u32(lo);
    s.put(",");
    if (hi == 0xFFFFFFFFu) {
        s.put("null");  // the overflow bucket has no upper bound
    } else {
        s.u32(hi);
    }
    s.put("]");
}

size_t finish(Sink& s) {
    s.put("}\n");
    return s.ok ? s.len : 0;
}

}  // namespace

const char* format_mac(char* out, size_t cap, const uint8_t mac[kMacLen]) {
    std::snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return out;
}

size_t write_boot_json(char* out, size_t cap, const BootRecord& r) {
    Sink s{out, cap};
    char mac[18];
    s.put("{");
    s.kv_s("t", "boot");
    s.comma();
    s.kv("node", r.node_id);
    s.comma();
    s.kv("epoch", r.boot_epoch);
    s.comma();
    s.kv_s("mac", format_mac(mac, sizeof(mac), r.mac));
    s.comma();
    s.kv("espnow_ver", r.espnow_version);
    s.comma();
    s.kv("chan", r.channel);
    s.comma();
    s.kv("hb_period_ms", r.hb_period_ms);
    s.comma();
    s.kv("hb_miss_limit", r.hb_miss_limit);
    s.comma();
    s.kv_s("fw", r.fw_version);
    s.comma();
    s.kv_s("idf", r.idf_version);
    s.comma();

    // §6's [MEASURE] item, decomposed so the Wi-Fi figure is separable from our own choices.
    s.put("\"dram\":{");
    s.kv("at_boot", r.dram_at_boot);
    s.comma();
    s.kv("after_nvs", r.dram_after_nvs);
    s.comma();
    s.kv("after_netif", r.dram_after_netif);
    s.comma();
    s.kv("after_wifi_init", r.dram_after_wifi_init);
    s.comma();
    s.kv("after_wifi_start", r.dram_after_wifi_start);
    s.comma();
    s.kv("after_espnow", r.dram_after_espnow);
    s.comma();
    s.kv("largest_block", r.dram_largest_block);
    s.comma();
    // The derived figures, so a reader does not have to redo the subtraction and get the sign
    // wrong. wifi_stack is the number §6 asked for.
    s.kv("wifi_stack", r.dram_after_netif - r.dram_after_wifi_start);
    s.comma();
    s.kv("espnow", r.dram_after_wifi_start - r.dram_after_espnow);
    s.comma();
    s.kv("total_to_radio", r.dram_at_boot - r.dram_after_espnow);
    s.put("}");
    return finish(s);
}

size_t write_link_json(char* out, size_t cap, const LinkRecord& r) {
    if (r.peer == nullptr) {
        return 0;
    }
    const PeerLink& p = *r.peer;

    Sink s{out, cap};
    char mac[18];
    s.put("{");
    s.kv_s("t", "link");
    s.comma();
    s.kv("up_ms", r.uptime_ms);
    s.comma();
    s.kv("node", r.node_id);
    s.comma();
    s.kv("peer", r.peer_node_id);
    s.comma();
    s.kv_s("mac", format_mac(mac, sizeof(mac), p.mac));
    s.comma();
    s.kv_s("state", peer_state_str(p.state));
    s.comma();
    s.kv("epoch", p.boot_epoch);
    s.comma();
    s.kv("espnow_ver", static_cast<uint32_t>(p.version));
    s.comma();
    s.kv("mtu", p.max_payload());
    s.comma();
    s.kv("misses", p.misses);
    s.comma();
    s.kv_i("rssi", r.last_rssi);
    s.comma();

    // --- outbound ---
    s.put("\"tx\":{");
    s.kv("frames", p.tx_frames);
    s.comma();
    s.kv("cb_ok", p.tx_cb_ok);
    s.comma();
    s.kv("cb_fail", p.tx_cb_fail);
    s.comma();
    s.kv("enqueue_err", p.tx_enqueue_err);
    s.comma();
    // Compute before passing. The order in which C++ evaluates function arguments is unspecified,
    // so `kv_opt(key, p.pdr_tx_ppm(ppm), ppm)` may read `ppm` before pdr_tx_ppm() has written it —
    // MSVC does exactly that, and the result was a PDR of 0 in every line.
    uint32_t tx_ppm = 0;
    const bool tx_ppm_known = p.pdr_tx_ppm(tx_ppm);
    s.kv_opt("pdr_ppm", tx_ppm_known, tx_ppm);
    s.put("},");

    // --- inbound ---
    s.put("\"rx\":{");
    s.kv("frames", p.rx_frames);
    s.comma();
    s.kv("lost_seqgap", p.rx_lost_seqgap);
    s.comma();
    s.kv("reorder_dup", p.rx_reorder_dup);
    s.comma();
    s.kv("dropped_bad", p.rx_dropped_bad);
    s.comma();
    s.kv("hb_lost", p.rx_hb_lost_seqgap);
    s.comma();
    uint32_t rx_ppm = 0;
    const bool rx_ppm_known = p.pdr_rx_ppm(rx_ppm);
    s.kv_opt("pdr_ppm", rx_ppm_known, rx_ppm);
    s.put("},");

    // --- RTT ---
    // Note what is absent: there is no one-way figure. See link_stats.hpp for why, and note that
    // the field names say exactly which clock each duration was measured on.
    s.put("\"rtt\":{");
    s.kv("samples", p.rtt_samples);
    s.comma();
    s.kv_opt("min_us", p.rtt_samples > 0, p.rtt_min_us);
    s.comma();
    s.kv_opt("max_us", p.rtt_samples > 0, p.rtt_max_us);
    s.comma();
    s.kv("timeouts", p.rtt_timeouts);
    s.comma();
    s.kv("txq_last_us", p.txq_last_us);
    s.comma();
    s.kv("txq_max_us", p.txq_max_us);
    s.comma();
    s.kv("remote_turnaround_us", p.remote_turnaround_last_us);
    s.comma();
    s.kv("remote_turnaround_max_us", p.remote_turnaround_max_us);
    if (r.histogram != nullptr) {
        s.comma();
        put_percentile(s, "p50_us", *r.histogram, 50);
        s.comma();
        put_percentile(s, "p99_us", *r.histogram, 99);
        s.comma();
        s.put("\"hist\":[");
        for (size_t i = 0; i < kRttBuckets; ++i) {
            if (i) s.comma();
            s.u32(r.histogram->bucket[i]);
        }
        s.put("]");
    }
    s.put("}");

    return finish(s);
}

size_t write_node_json(char* out, size_t cap, const NodeRecord& r) {
    if (r.counters == nullptr) {
        return 0;
    }
    const NodeCounters& c = *r.counters;

    Sink s{out, cap};
    s.put("{");
    s.kv_s("t", "node");
    s.comma();
    s.kv("up_ms", r.uptime_ms);
    s.comma();
    s.kv("node", r.node_id);
    s.comma();
    s.kv("epoch", r.boot_epoch);
    s.comma();
    s.kv("peers_alive", r.peers_alive);
    s.comma();
    s.kv("peers_dead", r.peers_dead);
    s.comma();
    s.kv("no_radio", r.no_radio ? 1u : 0u);
    s.comma();
    s.kv("free_dram", r.free_dram_now);
    s.comma();
    s.kv("largest_block", r.largest_free_block);
    s.comma();

    s.put("\"rx\":{");
    s.kv("total", c.rx_total);
    s.comma();
    s.kv("bad_frame", c.rx_bad_frame);
    s.comma();
    s.kv("unknown_peer", c.rx_unknown_peer);
    s.comma();
    s.kv("wrong_dst", c.rx_wrong_dst);
    s.comma();
    s.kv("short_payload", c.rx_short_payload);
    s.comma();
    s.kv("unknown_opcode", c.rx_unknown_opcode);
    s.comma();
    s.kv("queue_dropped", r.rx_queue_dropped);
    s.put("},");

    s.put("\"tx\":{");
    s.kv("total", c.tx_total);
    s.comma();
    s.kv("enqueue_err", c.tx_enqueue_err);
    s.comma();
    s.kv("done_queue_dropped", r.tx_done_queue_dropped);
    s.put("},");

    s.put("\"membership\":{");
    s.kv("deaths", c.deaths_declared);
    s.comma();
    s.kv("revivals", c.revivals);
    s.comma();
    s.kv("reboots_seen", c.reboots_seen);
    s.comma();
    s.kv("table_full", c.peer_table_full);
    s.comma();
    s.kv("events_dropped", r.event_ring_dropped);
    s.put("}");

    return finish(s);
}

size_t write_event_json(char* out, size_t cap, uint16_t node_id, const Event& e) {
    Sink s{out, cap};
    s.put("{");
    s.kv_s("t", "event");
    s.comma();
    s.kv("at_ms", e.at_ms);
    s.comma();
    s.kv("node", node_id);
    s.comma();
    s.kv_s("kind", event_kind_str(e.kind));
    s.comma();
    s.kv("peer", e.node_id);
    s.comma();
    s.kv("slot", e.peer_slot);
    s.comma();
    s.kv("a", e.detail_a);
    s.comma();
    s.kv("b", e.detail_b);
    return finish(s);
}

}  // namespace pot
