// ESP-NOW transport for Potluck Frame — ARCHITECTURE.md §5.3, §8.2.
//
// This is the only file in the firmware that knows what a radio is. Everything above it deals in
// Potluck Frames; everything below is ESP-IDF. The split is not tidiness — it is what lets the codec,
// the heartbeat state machine and the statistics be tested on a laptop (tests/), which is where
// the bugs that matter are cheapest to find.
//
// Concurrency. ESP-IDF delivers ESP-NOW receive and send completions on the Wi-Fi task, not on
// ours. Doing work there would block the Wi-Fi stack, so both callbacks do the minimum — copy into
// a queue, or update one peer's counters — and the real handling happens on the Potluck link task.
// The rule this file follows is: **a callback never parses, never sends, never logs.**

#pragma once

#include <cstdint>

#include "pot/frame.hpp"
#include "pot/link_stats.hpp"

namespace pot {

// A received frame, copied out of the Wi-Fi task's buffer and queued for the link task.
//
// The buffer is a full ESP-NOW v2 MTU even though M0's frames are 64 bytes: §6 budgets
// "RX ring, 8 × 1470 B" and sizing the slot to the profile rather than to today's traffic is what
// makes the budget mean anything. The queue holds kRxRingSlots of these.
struct RxSlot {
    uint8_t src_mac[kMacLen];
    uint16_t len;
    int8_t rssi;         // from wifi_pkt_rx_ctrl_t; useful for correlating the §3 range cliff
    uint8_t reserved0;
    uint32_t recv_us;    // esp_timer_get_time() truncated to 32 bits, on our clock
    uint8_t data[kEspNowV2LinkMtu];
};

// A send completion, likewise queued rather than handled inline.
struct TxDoneSlot {
    uint8_t dst_mac[kMacLen];
    uint8_t ok;  // ESP_NOW_SEND_SUCCESS — the 802.11 MAC-layer ACK, which is the outbound PDR signal
    uint8_t reserved0;
    uint32_t done_us;
};

// §6: "RX ring, 8 × 1470 B | 11.5 KB" and "TX ring, 4 × 1470 B | 5.7 KB". The RX ring is the queue
// depth here. The TX ring is ESP-IDF's own; Potluck does not add a second one at M0, because §5.4
// already forbids stacking an unbounded retry loop on a protocol that retries up to 31 times.
constexpr size_t kRxRingSlots = 8;
constexpr size_t kTxDoneRingSlots = 8;

struct EspNowConfig {
    uint8_t channel = 1;      // must match the channel the local device is on (§3)
    bool long_range = false;  // ESP-IDF's LR PHY; off at M0 so measurements match §3's 802.11b/g
    int8_t tx_power_qdbm = 0; // 0 = leave the driver default alone
};

// Result of bringing the radio up, including the numbers §6's [MEASURE] item asks for.
struct EspNowInitReport {
    bool ok = false;
    uint32_t espnow_version = 0;     // from esp_now_get_version()
    uint8_t mac[kMacLen] = {};       // our station MAC — the ESP-NOW peer address
    uint32_t free_dram_at_boot = 0;  // before esp_wifi_init()
    uint32_t free_dram_after_wifi = 0;
    uint32_t free_dram_after_espnow = 0;
    int32_t last_error = 0;          // esp_err_t of whatever failed, when ok is false
    const char* failed_at = nullptr;
};

// Bring up NVS on its own. Called by espnow_start(), and separately by a radio-less build, because
// the boot epoch lives in NVS and a node without a radio still has an incarnation.
bool nvs_init_once();

// Bring up NVS, Wi-Fi in station mode and ESP-NOW, measuring free internal DRAM at each step.
// Wi-Fi is started before ESP-NOW is initialised, which is the ordering the ESP-IDF documentation
// recommends and the only one in which ESP-NOW data can be transmitted.
EspNowInitReport espnow_start(const EspNowConfig& cfg);

// Add, modify or remove a peer. `add` is idempotent: re-adding an existing peer modifies it, which
// is what happens when a HELLO changes a peer's channel.
bool espnow_add_peer(const uint8_t mac[kMacLen], uint8_t channel);
bool espnow_del_peer(const uint8_t mac[kMacLen]);

// Submit a frame. Returns the esp_err_t from esp_now_send() — ESP_OK means queued, not delivered.
// Delivery is the send callback's business, and conflating the two is how a PDR figure becomes a
// measurement of the local queue rather than of the link.
int32_t espnow_send(const uint8_t mac[kMacLen], const uint8_t* data, size_t len);

// Blocking receive from the RX queue. `timeout_ms` of 0 polls. Returns false on timeout.
bool espnow_rx_pop(RxSlot& out, uint32_t timeout_ms);

// Non-blocking pop from the send-completion queue.
bool espnow_tx_done_pop(TxDoneSlot& out);

// Counters the queues themselves keep: frames the Wi-Fi task had to drop because the link task was
// behind. These are Potluck's own overruns, not the radio's, and must never be folded into PDR.
struct EspNowQueueStats {
    uint32_t rx_queued;
    uint32_t rx_dropped_queue_full;
    uint32_t rx_dropped_oversize;
    uint32_t tx_done_queued;
    uint32_t tx_done_dropped;
};
EspNowQueueStats espnow_queue_stats();

// Whether the radio actually came up. Every espnow_* call is a no-op when it did not:
// on a build with no Wi-Fi driver, calling into ESP-NOW resets the chip rather than
// returning ESP_ERR_ESPNOW_NOT_INIT.
bool espnow_up();

// The broadcast address. §5.1's dst 0xFFFF is a Potluck node id; this is the link-layer equivalent,
// and ESP-IDF requires it to be added as a peer before broadcasting — which costs one of §3's
// twenty peer slots, leaving nineteen for unicast.
extern const uint8_t kBroadcastMac[kMacLen];

// Free-DRAM probes live in dram_probe.hpp, next to §6's [MEASURE] item.

}  // namespace pot
