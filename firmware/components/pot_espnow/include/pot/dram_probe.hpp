// Free-DRAM probe — ARCHITECTURE.md §6's [MEASURE] item.
//
// §6 says: "The DRAM actually consumed by the Wi-Fi stack with ESP-NOW active is not stated on the
// page cited above and must be measured on your bench at M0. If it exceeds ~40 KB, the RX ring
// shrinks first."
//
// Measuring that as a single number would hide where it went, so the probe samples free internal
// DRAM at every step of bring-up and reports the deltas. The Wi-Fi figure §6 actually wants is
// `wifi_stack_bytes()` — esp_wifi_init plus esp_wifi_start — with NVS, netif and the event loop
// attributed separately, because those are Potluck's choices rather than the radio's cost.

#pragma once

#include <cstdint>

namespace pot {

// Free internal 8-bit-accessible DRAM, in bytes.
uint32_t free_internal_dram();

// Largest single allocatable internal block. Free space alone can hide fragmentation, and a node
// that cannot allocate a 1470-byte RX slot is broken regardless of how much free memory it reports.
uint32_t largest_free_internal_block();

// Free DRAM sampled at each bring-up step, in order. Every field is a *free memory* reading, so
// the cost of a step is the previous field minus this one.
struct DramProfile {
    uint32_t at_boot;         // first line of app_main
    uint32_t after_nvs;       // nvs_flash_init()
    uint32_t after_netif;     // esp_netif_init() + esp_event_loop_create_default()
    uint32_t after_wifi_init; // esp_wifi_init()
    uint32_t after_wifi_start;// esp_wifi_start()
    uint32_t after_espnow;    // esp_now_init() + callbacks registered
    uint32_t largest_block_after_espnow;

    // The §6 [MEASURE] answer: what the Wi-Fi stack itself cost.
    uint32_t wifi_stack_bytes() const { return after_netif - after_wifi_start; }
    // What ESP-NOW added on top of a started Wi-Fi stack.
    uint32_t espnow_bytes() const { return after_wifi_start - after_espnow; }
    // Everything from boot to a working radio.
    uint32_t total_bytes() const { return at_boot - after_espnow; }

    // §6's stated trigger: "If it exceeds ~40 KB, the RX ring shrinks first."
    bool exceeds_section6_expectation() const { return wifi_stack_bytes() > 40u * 1024u; }
};

// The profile captured during espnow_start(). Zeroed until then.
const DramProfile& dram_profile();

}  // namespace pot
