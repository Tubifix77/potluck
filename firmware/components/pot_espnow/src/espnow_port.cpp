// ESP-NOW transport implementation — see espnow_port.hpp.

#include "pot/espnow_port.hpp"

#include <cstring>

#include "pot/dram_probe.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

namespace pot {

const uint8_t kBroadcastMac[kMacLen] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

namespace {

const char* kTag = "pot.espnow";

// Statically allocated queues. §6 budgets the RX ring at 8 × 1470 B; xQueueCreateStatic with our
// own storage keeps that allocation out of the heap and visible to `idf.py size`, which is the
// difference between a budget the build can check and a budget written in a document.
StaticQueue_t g_rx_queue_ctrl;
uint8_t g_rx_queue_storage[kRxRingSlots * sizeof(RxSlot)];
QueueHandle_t g_rx_queue = nullptr;

StaticQueue_t g_tx_done_queue_ctrl;
uint8_t g_tx_done_queue_storage[kTxDoneRingSlots * sizeof(TxDoneSlot)];
QueueHandle_t g_tx_done_queue = nullptr;

// A single scratch slot for the receive callback, so a 1470-byte frame is not built on the Wi-Fi
// task's stack. Safe as a single instance because ESP-IDF delivers receive callbacks serially from
// one task; it is not safe to call on_recv() from anywhere else.
RxSlot g_rx_scratch;

EspNowQueueStats g_queue_stats{};

// Whether esp_now_init() has actually run. Every entry point below checks it.
//
// The ESP-NOW API documents ESP_ERR_ESPNOW_NOT_INIT for calls made too early, but the guarantee
// only extends as far as the driver being present: on a build with no Wi-Fi at all — a radio-less
// bench bring-up, or QEMU, which emulates the S3's CPU and UARTs but has no Wi-Fi device — calling
// into it resets the chip. That is how this was found: the node booted, declared its namespace,
// brought up its frame link, and then rebooted in a loop, with the boot epoch counting up as the
// only evidence.
bool g_espnow_up = false;

// ---------------------------------------------------------------------------------------------
// Callbacks.
//
// These run in *task* context — the Wi-Fi task — not in an ISR. The reference example proves it by
// calling malloc() and ESP_LOGE() inside them, neither of which is ISR-safe. So the queue calls
// here are xQueueSend, not xQueueSendFromISR.
//
// They are given a **zero timeout**, unlike the reference example, which blocks for up to
// portMAX_DELAY. Blocking here would stall the Wi-Fi task, which would delay every other frame in
// flight and corrupt the very timing M0 exists to measure. A full queue means the link task is
// behind, and the honest response is to drop the frame and count it — never to make the radio wait
// for us.
// ---------------------------------------------------------------------------------------------

void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (info == nullptr || info->src_addr == nullptr || data == nullptr || len <= 0) {
        return;
    }
    if (static_cast<size_t>(len) > sizeof(g_rx_scratch.data)) {
        // Longer than an ESP-NOW v2 MTU should be impossible; counting it rather than truncating
        // means that if it ever happens we find out instead of parsing a partial frame.
        ++g_queue_stats.rx_dropped_oversize;
        return;
    }

    // Timestamp first, before the copy, so the recorded arrival is as close to the radio as this
    // layer can get. The copy of a 48-byte heartbeat is microseconds, but the ordering costs
    // nothing and makes the number mean what it says.
    g_rx_scratch.recv_us = static_cast<uint32_t>(esp_timer_get_time());
    g_rx_scratch.len = static_cast<uint16_t>(len);
    g_rx_scratch.rssi = (info->rx_ctrl != nullptr) ? static_cast<int8_t>(info->rx_ctrl->rssi) : 0;
    g_rx_scratch.reserved0 = 0;
    std::memcpy(g_rx_scratch.src_mac, info->src_addr, kMacLen);
    std::memcpy(g_rx_scratch.data, data, static_cast<size_t>(len));

    if (xQueueSend(g_rx_queue, &g_rx_scratch, 0) != pdTRUE) {
        // A Potluck overrun, not a radio loss. Counted separately so it can never be folded into the
        // PDR figure M0 exists to produce.
        ++g_queue_stats.rx_dropped_queue_full;
    } else {
        ++g_queue_stats.rx_queued;
    }
}

void on_send(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
    if (tx_info == nullptr || tx_info->des_addr == nullptr) {
        return;
    }
    TxDoneSlot slot{};
    std::memcpy(slot.dst_mac, tx_info->des_addr, kMacLen);
    slot.ok = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
    slot.reserved0 = 0;
    slot.done_us = static_cast<uint32_t>(esp_timer_get_time());

    if (xQueueSend(g_tx_done_queue, &slot, 0) != pdTRUE) {
        ++g_queue_stats.tx_done_dropped;
    } else {
        ++g_queue_stats.tx_done_queued;
    }
}

}  // namespace

// §6's [MEASURE] probe. MALLOC_CAP_8BIT is what the ESP-IDF documentation names for "the free size
// of all DRAM heaps"; MALLOC_CAP_INTERNAL excludes any PSRAM a board might carry, which would
// otherwise make a node with external RAM look like it had cheap Wi-Fi.
uint32_t free_internal_dram() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

uint32_t largest_free_internal_block() {
    return static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

namespace {
DramProfile g_dram_profile{};
}  // namespace

const DramProfile& dram_profile() { return g_dram_profile; }

bool nvs_init_once() {
    static bool done = false;
    if (done) {
        return true;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A first boot, or a partition left over from a different layout. Erasing is the documented
        // recovery and it costs the boot epoch, which is why boot_epoch_next() treats a missing key
        // as "start at 1" rather than as an error.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }
    if (g_dram_profile.at_boot == 0) {
        g_dram_profile.at_boot = free_internal_dram();
    }
    g_dram_profile.after_nvs = free_internal_dram();
    done = true;
    return true;
}

EspNowInitReport espnow_start(const EspNowConfig& cfg) {
    EspNowInitReport rep;
    g_dram_profile.at_boot = free_internal_dram();

    // --- NVS ---------------------------------------------------------------------------------
    // Wi-Fi calibration data lives here, and so does the boot epoch (see boot_epoch.cpp).
    if (!nvs_init_once()) {
        rep.last_error = ESP_FAIL;
        rep.failed_at = "nvs_flash_init";
        return rep;
    }
    g_dram_profile.after_nvs = free_internal_dram();

    esp_err_t err = ESP_OK;

    // --- netif and the default event loop ------------------------------------------------------
    // ESP-NOW carries no IP traffic, so a netif is not strictly required — but the reference
    // example initialises one, and M0 is a measurement whose value depends on matching the
    // reference configuration. The cost is measured separately above so that a later milestone can
    // decide whether to drop it with a number in hand rather than a guess.
    err = esp_netif_init();
    if (err != ESP_OK) {
        rep.last_error = err;
        rep.failed_at = "esp_netif_init";
        return rep;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        rep.last_error = err;
        rep.failed_at = "esp_event_loop_create_default";
        return rep;
    }
    g_dram_profile.after_netif = free_internal_dram();

    // --- Wi-Fi -------------------------------------------------------------------------------
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        rep.last_error = err;
        rep.failed_at = "esp_wifi_init";
        return rep;
    }
    g_dram_profile.after_wifi_init = free_internal_dram();

    // Keep the configuration in RAM: writing it to flash on every boot would wear the part for no
    // benefit, since ESP-NOW never associates.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_start();
    if (err != ESP_OK) {
        rep.last_error = err;
        rep.failed_at = "esp_wifi_start";
        return rep;
    }

    // No power save. M0 is measuring delay, and modem sleep would add a wake latency that has
    // nothing to do with the link — the figure would be about our power policy, not about
    // ESP-NOW. A battery node will want this back (§7.8 makes power the honest constraint), and
    // that is a deliberate later decision, not a default to inherit silently.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // §3: "The channel must be set as the channel that the local device is on." Fixing it here and
    // pinning every peer to the same value is what keeps that true.
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg.channel, WIFI_SECOND_CHAN_NONE));

    if (cfg.long_range) {
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                                               WIFI_PROTOCOL_11N |
                                                               WIFI_PROTOCOL_LR));
    }
    if (cfg.tx_power_qdbm != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(cfg.tx_power_qdbm));
    }
    g_dram_profile.after_wifi_start = free_internal_dram();

    // --- queues ------------------------------------------------------------------------------
    g_rx_queue = xQueueCreateStatic(kRxRingSlots, sizeof(RxSlot), g_rx_queue_storage,
                                    &g_rx_queue_ctrl);
    g_tx_done_queue = xQueueCreateStatic(kTxDoneRingSlots, sizeof(TxDoneSlot),
                                         g_tx_done_queue_storage, &g_tx_done_queue_ctrl);
    if (g_rx_queue == nullptr || g_tx_done_queue == nullptr) {
        rep.last_error = ESP_ERR_NO_MEM;
        rep.failed_at = "xQueueCreateStatic";
        return rep;
    }

    // --- ESP-NOW -----------------------------------------------------------------------------
    // Started after Wi-Fi, per the ESP-IDF documentation: "ESP-NOW data must be transmitted after
    // Wi-Fi is started."
    err = esp_now_init();
    if (err != ESP_OK) {
        rep.last_error = err;
        rep.failed_at = "esp_now_init";
        return rep;
    }
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
    g_espnow_up = true;
    g_dram_profile.after_espnow = free_internal_dram();
    g_dram_profile.largest_block_after_espnow = largest_free_internal_block();

    // --- report ------------------------------------------------------------------------------
    ESP_ERROR_CHECK(esp_now_get_version(&rep.espnow_version));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, rep.mac));

    rep.free_dram_at_boot = g_dram_profile.at_boot;
    rep.free_dram_after_wifi = g_dram_profile.after_wifi_start;
    rep.free_dram_after_espnow = g_dram_profile.after_espnow;
    rep.ok = true;

    ESP_LOGI(kTag, "esp-now v%u up on channel %u, mac %02x:%02x:%02x:%02x:%02x:%02x",
             static_cast<unsigned>(rep.espnow_version), static_cast<unsigned>(cfg.channel),
             rep.mac[0], rep.mac[1], rep.mac[2], rep.mac[3], rep.mac[4], rep.mac[5]);
    return rep;
}

bool espnow_add_peer(const uint8_t mac[kMacLen], uint8_t channel) {
    if (!g_espnow_up) {
        return false;
    }
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, kMacLen);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;  // M5 owns link crypto; M0 measures an unencrypted link (§9, §13-M5)

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        err = esp_now_mod_peer(&peer);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "add_peer %02x:%02x:%02x:%02x:%02x:%02x failed: %s", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5], esp_err_to_name(err));
        return false;
    }
    return true;
}

bool espnow_del_peer(const uint8_t mac[kMacLen]) {
    return g_espnow_up && esp_now_del_peer(mac) == ESP_OK;
}

int32_t espnow_send(const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    if (!g_espnow_up) {
        // Not an error worth logging on every beacon: a radio-less node still beacons, and the
        // caller counts the enqueue failure. Returning rather than calling in is what keeps the
        // chip alive.
        return -1;
    }
    return static_cast<int32_t>(esp_now_send(mac, data, len));
}

bool espnow_rx_pop(RxSlot& out, uint32_t timeout_ms) {
    if (!g_espnow_up || g_rx_queue == nullptr) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(g_rx_queue, &out, ticks) == pdTRUE;
}

bool espnow_tx_done_pop(TxDoneSlot& out) {
    if (!g_espnow_up || g_tx_done_queue == nullptr) {
        return false;
    }
    return xQueueReceive(g_tx_done_queue, &out, 0) == pdTRUE;
}

EspNowQueueStats espnow_queue_stats() { return g_queue_stats; }

bool espnow_up() { return g_espnow_up; }

}  // namespace pot
