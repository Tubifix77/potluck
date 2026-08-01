// The serial stats format — one JSON object per line.
//
// This lives in the portable component on purpose. It is a *contract* between the firmware and
// host/potluck, and a contract that only one side can test is a contract that drifts. Keeping
// it here means tests/test_stats_json.cpp checks the exact bytes a board will emit, and
// potluck-capture is tested against the same golden strings.
//
// Format rules, all chosen to survive a serial line that drops characters:
//   * One object per line, newline-terminated, no pretty printing. A truncated line is discarded
//     by the reader rather than corrupting the record before it.
//   * Every line carries "t" (the record type) first, so a reader can dispatch before parsing.
//   * No floats. Durations are integers in their stated unit and ratios are parts per million.
//     Printing a float from firmware costs code space and invites locale trouble for no benefit.
//   * Unknown means absent or null, never zero. A PDR of 0 and an unmeasured PDR are different
//     facts and §13-M0 needs a measured one.
//   * Percentiles are emitted as [lo, hi] intervals, because that is all a histogram knows.
//
// The writer takes a caller-supplied buffer and never allocates.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/link_stats.hpp"

namespace pot {

// Enough for the largest line this module emits: a link record with a 16-bucket histogram.
constexpr size_t kJsonLineMax = 1024;

// Identity and configuration of the emitting node, for the boot record.
struct BootRecord {
    uint16_t node_id;
    uint32_t boot_epoch;
    uint8_t mac[kMacLen];
    uint32_t espnow_version;
    uint8_t channel;
    uint32_t hb_period_ms;
    uint8_t hb_miss_limit;
    const char* fw_version;
    const char* idf_version;
    // §6 [MEASURE]: free internal DRAM at each bring-up step, in bytes.
    uint32_t dram_at_boot;
    uint32_t dram_after_nvs;
    uint32_t dram_after_netif;
    uint32_t dram_after_wifi_init;
    uint32_t dram_after_wifi_start;
    uint32_t dram_after_espnow;
    uint32_t dram_largest_block;
};

// Everything the periodic link record reports about one peer.
struct LinkRecord {
    uint32_t uptime_ms;
    uint16_t node_id;      // ours
    uint16_t peer_node_id;
    const PeerLink* peer;
    const RttHistogram* histogram;
    int8_t last_rssi;
};

// Node-wide record: counters that are not per-peer, plus the transport's own queue statistics.
struct NodeRecord {
    uint32_t uptime_ms;
    uint16_t node_id;
    uint32_t boot_epoch;
    const NodeCounters* counters;
    uint32_t free_dram_now;
    uint32_t largest_free_block;
    uint32_t rx_queue_dropped;
    uint32_t tx_done_queue_dropped;
    uint32_t event_ring_dropped;
    uint32_t peers_alive;
    uint32_t peers_dead;

    // True when the radio never started. A run in that state produces no valid link
    // measurement, and §13-M0 asks for measured figures — so it is on every line rather
    // than mentioned once at boot where a log rotation could lose it.
    bool no_radio;
};

// Each writer returns the number of bytes written, excluding the terminating NUL, or 0 if the
// buffer was too small. The line includes its own trailing '\n'.
size_t write_boot_json(char* out, size_t cap, const BootRecord& r);
size_t write_link_json(char* out, size_t cap, const LinkRecord& r);
size_t write_node_json(char* out, size_t cap, const NodeRecord& r);
size_t write_event_json(char* out, size_t cap, uint16_t node_id, const Event& e);

// Format a MAC as aa:bb:cc:dd:ee:ff into a buffer of at least 18 bytes. Returns `out`.
const char* format_mac(char* out, size_t cap, const uint8_t mac[kMacLen]);

}  // namespace pot
