// M15: telemetry over the cell -- what a board knows about itself, pulled by the resident hub over the radio
// (ARCHITECTURE section 13). Until M15 a board printed it to its USB console, which no deployed board has.
//
// The hub asks the board on its cable (TELEMETRY, 0x3A, inside the deploy range, answered by the deploy server like
// NODE_STATUS) with a target node id and a section; a target other than the cabled board is passed on over the
// radio. Every reply fits the cable's 226-byte payload floor (section 5.3), so nothing is fragmented: a section too
// large for one reply is paged (links by start index, events by sequence number). Binary, not the console's JSON:
// the kill line is the radio's load, and a link here is 40 bytes where its JSON line is ~600. The counters are the
// ones the console's stats lines print (stats_json.hpp), so the two never disagree about a board.
//
// Wire, little-endian.
//   request:  u16 target (0 = the cabled board), u8 section (kTelemetry*), u8 start (links: first peer slot to
//             report; actors: first actor), u32 since (events: report events with seq > since)
//   reply:    u8 version (1), u8 section, u16 node, then the section:
//     Health  (kTelemetryHealth): u32 uptime_s, u32 boot_epoch, u8 channel, u8 flags (kTelHealth*), u16 hb_period_ms,
//             u32 int_free, u32 int_min, u32 int_largest, u32 ext_free, u32 ext_min, u8 peers_alive, u8 peers_dead,
//             u8 actors_pinned, u8 actors_portable_here, u32 deaths, u32 revivals, u32 reboots_seen, u32 rx_total,
//             u32 rx_rejected (bad frame + short payload + unknown opcode), u32 rx_queue_dropped, u32 tx_total,
//             u32 tx_enqueue_err, u32 tx_done_dropped, u32 events_dropped, u32 writes_served, u32 writes_rejected,
//             u16 link_stack_free, u16 int_total_kib (the internal heap's size, for saturation)      (88 bytes)
//     Links   (kTelemetryLinks): u8 total (peers in the table), u8 start, u8 n, n x link (40 bytes):
//             u16 node, u8 state (PeerState), i8 rssi, u8 espnow_version, u8 flags (kTelLink*), u16 hb_period_ms,
//             u8 miss_limit, u8 misses, u32 tx_frames, u32 tx_cb_ok, u32 tx_cb_fail, u32 rx_frames, u32 rx_lost
//             (sequence gaps, unicast and heartbeat), u32 rx_bcast, u16 rtt_p50_us100, u16 rtt_p99_us100 (upper
//             bound of the percentile's bucket, 100 us units; 0xFFFF unknown or past the last bucket),
//             u16 rtt_timeouts (saturating)
//     Actors  (kTelemetryActors): u8 total (portable actors in the package), u8 start, u8 n, n x actor (18 bytes):
//             u32 key (the output path hash), u8 flags (kTelActor*), u8 reserved, u16 owner, u16 assigned,
//             u32 activations, u16 fenced, u16 released (saturating)
//     Events  (kTelemetryEvents): u32 next (the seq the next event will get), u8 n, n x event (19 bytes):
//             u32 seq, u32 at_ms (board uptime), u8 kind (EventKind), u16 node, u32 a, u32 b
//             -- the oldest kept events with seq > since, up to what fits; `next` tells the hub whether it missed any.
//     Auth    (kTelemetryAuth, 2026-10-19): u32 verified, cache_hits, refused, rate_limited, stale_epoch, tags_ok,
//             bad_tag, replayed, untagged, bcast_dropped, beacon_epoch_ignored, group_refusals          (48 bytes)
//             -- Node::AuthCounters, so a board kept out of the cell can be told apart: its HELLOs not heard, heard and
//             fenced as an older epoch, over the verification budget, or its frames under another key. Firmware before
//             it answers section 0 (unreachable), which the hub takes as "not reported".
//     Chip    (kTelemetryChip, 2026-10-23, M21): u8 model (esp_chip_model_t: 9 = ESP32-S3), u8 cores, u16 revision
//             (MXX: major x 100 + minor), u16 flash_mb, u16 psram_mb (0 without PSRAM), u32 features (CHIP_FEATURE_*)
//                                                                                                    (12 bytes)
//             -- what the board is, as the board itself reads it (esp_chip_info, esp_flash_get_size,
//             esp_psram_get_size): read at reply time, nothing kept. Firmware before it answers section 0.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/link_stats.hpp"

namespace pot {

constexpr uint8_t kTelemetryVersion = 1;
constexpr uint8_t kTelemetryHealth = 1;
constexpr uint8_t kTelemetryLinks = 2;
constexpr uint8_t kTelemetryActors = 3;
constexpr uint8_t kTelemetryEvents = 4;
constexpr uint8_t kTelemetryAuth = 5;
constexpr uint8_t kTelemetryChip = 6;

constexpr size_t kTelemetryRequestLen = 8;
constexpr size_t kTelemetryReplyMax = 220;  // under the cable's 226-byte payload floor
constexpr size_t kTelemetryHeaderLen = 4;
constexpr size_t kTelemetryHealthLen = 88;
constexpr size_t kTelemetryLinkLen = 40;
constexpr size_t kTelemetryActorLen = 18;
constexpr size_t kTelemetryEventLen = 19;
constexpr size_t kTelemetryAuthLen = 48;
constexpr size_t kTelemetryChipLen = 12;
constexpr size_t kTelemetryLinksPerReply = (kTelemetryReplyMax - kTelemetryHeaderLen - 3) / kTelemetryLinkLen;    // 5
constexpr size_t kTelemetryActorsPerReply = (kTelemetryReplyMax - kTelemetryHeaderLen - 3) / kTelemetryActorLen;  // 11
constexpr size_t kTelemetryEventsPerReply = (kTelemetryReplyMax - kTelemetryHeaderLen - 5) / kTelemetryEventLen;  // 11

// Health flags.
constexpr uint8_t kTelHealthNoRadio = 1u << 0;
constexpr uint8_t kTelHealthBusy = 1u << 1;       // its HELLO says BUSY (M5.1)
constexpr uint8_t kTelHealthConnected = 1u << 2;  // it has an uplink (M13)
constexpr uint8_t kTelHealthLongWindow = 1u << 3; // it declared a death window longer than the default: may be asleep
constexpr uint8_t kTelHealthEnrolled = 1u << 4;
// Link flags.
constexpr uint8_t kTelLinkCable = 1u << 0;     // the peer is the host on this board's cable
constexpr uint8_t kTelLinkVerified = 1u << 1;  // its identity verified (M5)
// Actor flags.
constexpr uint8_t kTelActorRunningHere = 1u << 0;

struct TelemetryRequest {
    uint16_t target = 0;
    uint8_t section = 0;
    uint8_t start = 0;
    uint32_t since = 0;
};
bool load_telemetry_request(const uint8_t* p, size_t len, TelemetryRequest& out);
size_t store_telemetry_request(const TelemetryRequest& r, uint8_t* out, size_t cap);

struct TelemetryHealth {
    uint32_t uptime_s = 0, boot_epoch = 0;
    uint8_t channel = 0, flags = 0;
    uint16_t hb_period_ms = 0;
    uint32_t int_free = 0, int_min = 0, int_largest = 0, ext_free = 0, ext_min = 0;
    uint8_t peers_alive = 0, peers_dead = 0, actors_pinned = 0, actors_portable_here = 0;
    uint32_t deaths = 0, revivals = 0, reboots_seen = 0;
    uint32_t rx_total = 0, rx_rejected = 0, rx_queue_dropped = 0;
    uint32_t tx_total = 0, tx_enqueue_err = 0, tx_done_dropped = 0;
    uint32_t events_dropped = 0, writes_served = 0, writes_rejected = 0;
    uint16_t link_stack_free = 0;
    uint16_t int_total_kib = 0;
};

struct TelemetryLink {
    uint16_t node = 0;
    uint8_t state = 0;
    int8_t rssi = 0;
    uint8_t espnow_version = 0, flags = 0;
    uint16_t hb_period_ms = 0;
    uint8_t miss_limit = 0, misses = 0;
    uint32_t tx_frames = 0, tx_cb_ok = 0, tx_cb_fail = 0, rx_frames = 0, rx_lost = 0, rx_bcast = 0;
    uint16_t rtt_p50_us100 = 0xFFFF, rtt_p99_us100 = 0xFFFF, rtt_timeouts = 0;
};
// From the peer table's own record and its RTT histogram -- the numbers write_link_json prints.
TelemetryLink telemetry_link(const PeerLink& p, const RttHistogram& h, uint8_t flags);

struct TelemetryActor {
    uint32_t key = 0;
    uint8_t flags = 0;
    uint16_t owner = 0, assigned = 0;
    uint32_t activations = 0;
    uint16_t fenced = 0, released = 0;
};

struct TelemetryEvent {
    uint32_t seq = 0, at_ms = 0;
    uint8_t kind = 0;
    uint16_t node = 0;
    uint32_t a = 0, b = 0;
};

struct TelemetryAuth {
    uint32_t verified = 0, cache_hits = 0, refused = 0, rate_limited = 0, stale_epoch = 0, tags_ok = 0, bad_tag = 0,
             replayed = 0, untagged = 0, bcast_dropped = 0, beacon_epoch_ignored = 0, group_refusals = 0;
};

struct TelemetryChip {
    uint8_t model = 0, cores = 0;
    uint16_t revision = 0, flash_mb = 0, psram_mb = 0;
    uint32_t features = 0;
};

// Each returns the reply's length, 0 if `cap` is too small. n is clipped to what one reply holds.
size_t store_telemetry_auth(uint16_t node, const TelemetryAuth& a, uint8_t* out, size_t cap);
size_t store_telemetry_chip(uint16_t node, const TelemetryChip& c, uint8_t* out, size_t cap);
size_t store_telemetry_health(uint16_t node, const TelemetryHealth& h, uint8_t* out, size_t cap);
size_t store_telemetry_links(uint16_t node, uint8_t total, uint8_t start, const TelemetryLink* l, size_t n,
                             uint8_t* out, size_t cap);
size_t store_telemetry_actors(uint16_t node, uint8_t total, uint8_t start, const TelemetryActor* a, size_t n,
                              uint8_t* out, size_t cap);
size_t store_telemetry_events(uint16_t node, uint32_t next, const TelemetryEvent* e, size_t n, uint8_t* out,
                              size_t cap);

// A board's recent events with sequence numbers, for the hub to page through (the console's ring is drained by the
// stats task; this keeps a copy). Not thread-safe; the board guards it with its node lock.
class TelemetryEventLog {
  public:
    static constexpr size_t kSize = 64;
    void push(uint32_t at_ms, uint8_t kind, uint16_t node, uint32_t a, uint32_t b);
    // Up to `max` events with seq > since, oldest first. Returns how many.
    size_t since(uint32_t since, TelemetryEvent* out, size_t max) const;
    uint32_t next() const { return next_; }

  private:
    TelemetryEvent buf_[kSize] = {};
    uint32_t next_ = 1;  // seq 0 means "before anything": a hub starts by asking for events since 0
};

}  // namespace pot
