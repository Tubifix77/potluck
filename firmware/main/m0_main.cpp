// Potluck M0 — two boards, one heartbeat. Node application for ESP-IDF.
//
// ARCHITECTURE.md §13-M0: "Two ESP32s. Potluck Frame codec. HELLO / HEARTBEAT over ESP-NOW. Link
// statistics: PDR, round-trip delay histogram, retry counts."
//
// This file is deliberately thin. All of the policy — membership, beaconing, probing, statistics —
// lives in pot::Node (components/pot_link/node.hpp), which has no ESP-IDF dependency and is the
// same object sim/ runs as N virtual nodes over a modelled channel. What remains here is the glue:
// bring the radio up, pump ESP-NOW callbacks into the node, print what the node accumulated.
//
// That split is what makes the simulator worth anything. A simulator running a *copy* of the policy
// proves things about the copy; this one runs the code that ships.
//
// Two tasks:
//   link_task  (priority 6)  owns the Node. Receives, ticks, sends. Single writer, so the peer
//                            table needs no lock for its own sake.
//   stats_task (priority 3)  formats and prints. Printing ~700 bytes of JSON on a blocking console
//                            UART takes milliseconds; doing it on link_task would delay beacons and
//                            manufacture the very misses §8.2 counts. It holds the mutex only long
//                            enough to copy a peer, then formats from the copy.

#include <cstdio>
#include <cstring>
#include <new>  // placement new, for constructing the Node into static storage

#include "pot/boot_epoch.hpp"
#include "pot/dram_probe.hpp"
#include "pot/espnow_port.hpp"
#include "pot/node.hpp"
#include "pot/serial_port.hpp"
#include "pot/stats_json.hpp"
#include "pot/sys_resources.hpp"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace pot {
namespace {

const char* kTag = "pot.m0";

constexpr uint8_t kChannel = CONFIG_POT_CHANNEL;
constexpr uint32_t kStatsIntervalMs = CONFIG_POT_STATS_INTERVAL_MS;

// ---------------------------------------------------------------------------------------------
// State. All static: §6 budgets it, and a node that can fail to allocate at hour 19 of a 24-hour
// soak is not something to discover at hour 19.
// ---------------------------------------------------------------------------------------------

alignas(Node) uint8_t g_node_storage[sizeof(Node)];
Node* g_node = nullptr;

StaticSemaphore_t g_mutex_buf;
SemaphoreHandle_t g_mutex = nullptr;

// The stats line buffer. Static rather than on stats_task's stack: 1 KB is a quarter of that task's
// 4 KB, and a stack overflow nineteen hours into a soak is the worst way to lose a measurement.
// This 1 KB is M0 instrumentation, not §6 core — M2's bridge tees binary frames and it goes away.
char g_json[kJsonLineMax];

// Set when the radio failed to start. Reported everywhere so a boot without a radio can
// never be mistaken for a valid soak.
bool g_no_radio = false;

StaticTask_t g_link_tcb;
StackType_t g_link_stack[4096 / sizeof(StackType_t)];
StaticTask_t g_stats_tcb;
StackType_t g_stats_stack[4096 / sizeof(StackType_t)];

uint32_t now_ms_() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
uint32_t now_us_() { return static_cast<uint32_t>(esp_timer_get_time()); }

// ---------------------------------------------------------------------------------------------
// Frame tee — §7.6's capture stream at M0's scale. Off by default: at ~40 lines/second the console
// UART's cost lands inside the timing being measured. On to debug the wire, off to measure it.
// ---------------------------------------------------------------------------------------------
#if CONFIG_POT_FRAME_TEE
void tee_frame(const char* dir, const uint8_t mac[kMacLen], int8_t rssi, const uint8_t* data,
               size_t len) {
    static char line[64 + 2 * kEspNowV2LinkMtu];
    char macbuf[18];
    format_mac(macbuf, sizeof(macbuf), mac);
    int n = std::snprintf(line, sizeof(line),
                          "{\"t\":\"frame\",\"at_us\":%u,\"dir\":\"%s\",\"peer\":\"%s\","
                          "\"rssi\":%d,\"len\":%u,\"raw\":\"",
                          static_cast<unsigned>(now_us_()), dir, macbuf, static_cast<int>(rssi),
                          static_cast<unsigned>(len));
    if (n < 0) return;
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < len && static_cast<size_t>(n) + 4 < sizeof(line); ++i) {
        line[n++] = hex[data[i] >> 4];
        line[n++] = hex[data[i] & 0x0F];
    }
    line[n++] = '"';
    line[n++] = '}';
    line[n++] = '\n';
    line[n] = '\0';
    std::fputs(line, stdout);
}
#else
void tee_frame(const char*, const uint8_t*, int8_t, const uint8_t*, size_t) {}
#endif

// ---------------------------------------------------------------------------------------------
// NodeHal — the node's view of this board.
// ---------------------------------------------------------------------------------------------

// The router, such as it is at M0/M1: one reserved MAC means the UART, everything else means the
// radio. §7.0 puts a real router above the transports; this is the two-transport degenerate case of
// it, and keeping the decision in one function is what stops "is this the host?" from spreading.
int32_t hal_send(void*, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    tee_frame("tx", mac, 0, data, len);
#if CONFIG_POT_SERIAL_LINK
    if (std::memcmp(mac, kHostMac, kMacLen) == 0) {
        return serial_port_send(data, len);
    }
#endif
    return espnow_send(mac, data, len);
}

bool hal_add_peer(void*, const uint8_t mac[kMacLen]) {
#if CONFIG_POT_SERIAL_LINK
    if (std::memcmp(mac, kHostMac, kMacLen) == 0) {
        // The UART has no peer list to join; the link either exists or it does not.
        return serial_port_running();
    }
#endif
    return espnow_add_peer(mac, kChannel);
}
uint32_t hal_now_ms(void*) { return now_ms_(); }
uint32_t hal_now_us(void*) { return now_us_(); }
uint32_t hal_free_dram(void*) { return free_internal_dram(); }

void hal_on_event(void*, const Event& e) {
    // The loud ones go to the log as they happen; all of them are in the event ring for the JSON
    // stream regardless. §4 rule 4 wants demotion to be loud, and a membership transition buried in
    // a statistics dump ten seconds later is not loud.
    switch (e.kind) {
        case EventKind::PeerDead:
            ESP_LOGW(kTag, "peer 0x%04x declared DEAD after %u misses (%u ms silent)", e.node_id,
                     static_cast<unsigned>(e.detail_a), static_cast<unsigned>(e.detail_b));
            break;
        case EventKind::PeerRevived:
            ESP_LOGI(kTag, "peer 0x%04x revived", e.node_id);
            break;
        case EventKind::PeerRebooted:
            ESP_LOGW(kTag, "peer 0x%04x rebooted, epoch now %u", e.node_id,
                     static_cast<unsigned>(e.detail_a));
            break;
        case EventKind::PeerDiscovered:
            ESP_LOGI(kTag, "discovered peer 0x%04x", e.node_id);
            break;
        case EventKind::PeerLeft:
            ESP_LOGI(kTag, "peer 0x%04x left", e.node_id);
            break;
        case EventKind::VersionPinned:
            ESP_LOGI(kTag, "peer 0x%04x pinned to ESP-NOW v%u, payload cap %u B", e.node_id,
                     static_cast<unsigned>(e.detail_a), static_cast<unsigned>(e.detail_b));
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// The BYE button — the only way to exercise "left" as distinct from "dead" without unplugging.
// ---------------------------------------------------------------------------------------------
#if CONFIG_POT_BYE_BUTTON_GPIO >= 0
constexpr gpio_num_t kByeButton = static_cast<gpio_num_t>(CONFIG_POT_BYE_BUTTON_GPIO);

void bye_button_init() {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << CONFIG_POT_BYE_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

// Polled once per heartbeat period, which is its own debounce: a bounce shorter than the period
// cannot be seen twice.
void bye_button_poll() {
    static bool last_pressed = false;
    const bool pressed = gpio_get_level(kByeButton) == 0;
    if (pressed == last_pressed) return;
    last_pressed = pressed;
    if (!pressed) return;  // act on press, not release

    if (g_node->departed()) {
        ESP_LOGW(kTag, "rejoining the mesh (button)");
        g_node->rejoin();
    } else {
        ESP_LOGW(kTag, "BYE: leaving the mesh (button)");
        g_node->depart();
    }
}
#else
void bye_button_init() {}
void bye_button_poll() {}
#endif


// ---------------------------------------------------------------------------------------------
// The status LED -- a node's view of the cell, readable without a console.
//
// Built for the distance sweep, where one board walks the house on a mains charger and the person
// carrying it needs to know, there and then, whether it still hears the others. The colour is
// this node's *receive* view: the heartbeats it is getting from its peers. That is a field
// indicator, not the measurement -- the measurement is the board on the PC, which sees both
// directions of every link it has (inbound by sequence gaps, outbound by the MAC ACK).
//
//   blue    no peer heard since power-up
//   green   every peer alive, heartbeat loss under 10 % over the last two seconds
//   yellow  every peer alive, but the worst one losing 10-50 %
//   red     some peer losing over 50 %, or declared dead
//
// Updated once a second. At power-up: red, green, blue, half a second each, so the pin and the
// colour order can be checked by eye before the colours are trusted.
//
// The pixel is a WS2812-style part on GPIO48 (DevKitC-1 v1.0) or GPIO38 (v1.1), and listings do
// not say which revision ships, so both pins are driven. Bit timings and the encoder shape follow
// ESP-IDF's own examples/peripherals/rmt/led_strip_simple_encoder. The RMT driver allocates its
// channel at boot, once, like the Wi-Fi stack does; nothing here allocates afterwards.
// ---------------------------------------------------------------------------------------------
namespace status_led {

constexpr uint32_t kResolutionHz = 10000000;  // 0.1 us per RMT tick
constexpr uint8_t kBrightness = 24;           // of 255: readable indoors, not a torch
constexpr uint32_t kUpdateMs = 1000;

rmt_symbol_word_t symbol(uint16_t high_ticks, uint16_t low_ticks) {
    rmt_symbol_word_t s{};
    s.level0 = 1;
    s.duration0 = high_ticks;
    s.level1 = 0;
    s.duration1 = low_ticks;
    return s;
}

size_t encode(const void* data, size_t data_size, size_t symbols_written, size_t symbols_free,
              rmt_symbol_word_t* symbols, bool* done, void*) {
    if (symbols_free < 8) return 0;
    const size_t pos = symbols_written / 8;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (pos < data_size) {
        size_t n = 0;
        for (int mask = 0x80; mask != 0; mask >>= 1) {
            // 1: 0.9 us high, 0.3 us low.  0: 0.3 us high, 0.9 us low.
            symbols[n++] = (bytes[pos] & mask) ? symbol(9, 3) : symbol(3, 9);
        }
        return n;
    }
    // Latch: 50 us low.
    rmt_symbol_word_t reset{};
    reset.duration0 = 250;
    reset.duration1 = 250;
    symbols[0] = reset;
    *done = true;
    return 1;
}

struct Channel {
    rmt_channel_handle_t chan = nullptr;
    uint8_t grb[3] = {};  // must outlive the transmission, so not on a stack
};

Channel g_ch[2];
rmt_encoder_handle_t g_enc = nullptr;
bool g_up = false;

void open(Channel& c, int gpio) {
    if (gpio < 0) return;
    rmt_tx_channel_config_t cfg{};
    cfg.gpio_num = static_cast<gpio_num_t>(gpio);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = 48;
    cfg.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&cfg, &c.chan) != ESP_OK || rmt_enable(c.chan) != ESP_OK) {
        ESP_LOGW(kTag, "status LED: could not open GPIO%d", gpio);
        c.chan = nullptr;
    }
}

void show(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_up) return;
    rmt_transmit_config_t tx{};
    for (Channel& c : g_ch) {
        if (c.chan == nullptr) continue;
        rmt_tx_wait_all_done(c.chan, 5);  // never rewrite a buffer the RMT is still reading
        c.grb[0] = g;
        c.grb[1] = r;
        c.grb[2] = b;
        rmt_transmit(c.chan, g_enc, c.grb, sizeof(c.grb), &tx);
    }
}

void init() {
    rmt_simple_encoder_config_t ecfg{};
    ecfg.callback = encode;
    if (rmt_new_simple_encoder(&ecfg, &g_enc) != ESP_OK) {
        ESP_LOGW(kTag, "status LED: no encoder, LED disabled");
        return;
    }
    open(g_ch[0], CONFIG_POT_STATUS_LED_GPIO);
    open(g_ch[1], CONFIG_POT_STATUS_LED_GPIO_ALT);
    g_up = g_ch[0].chan != nullptr || g_ch[1].chan != nullptr;
    ESP_LOGI(kTag, "status LED on GPIO%d and GPIO%d; self-test red, green, blue",
             CONFIG_POT_STATUS_LED_GPIO, CONFIG_POT_STATUS_LED_GPIO_ALT);
    show(kBrightness, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    show(0, kBrightness, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    show(0, 0, kBrightness);
    vTaskDelay(pdMS_TO_TICKS(500));
}

enum class Colour : uint8_t { Blue, Green, Yellow, Red };

// Per peer slot: the (broadcast received, heartbeats lost) counters one and two seconds ago.
struct Snap {
    uint32_t bcast;
    uint32_t lost;
};
Snap g_hist[kMaxPeers][2];

// Called with g_mutex held: reads the peer table, touches nothing in it.
Colour assess(const Node& n) {
    bool any = false;
    bool dead = false;
    uint32_t worst_ppm = 0;
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& p = n.peers().slot(i);
        const Snap now{p.rx_bcast_frames, p.rx_hb_lost_seqgap};
        const Snap old = g_hist[i][1];
        g_hist[i][1] = g_hist[i][0];
        g_hist[i][0] = now;
        if (p.state == PeerState::Dead) {
            any = true;
            dead = true;
            continue;
        }
        if (p.state != PeerState::Alive) continue;
        any = true;
        const uint32_t rx = now.bcast >= old.bcast ? now.bcast - old.bcast : 0;
        const uint32_t lost = now.lost >= old.lost ? now.lost - old.lost : 0;
        if (rx + lost == 0) continue;  // admitted too recently to judge
        const uint32_t ppm =
            static_cast<uint32_t>((static_cast<uint64_t>(lost) * 1000000u) / (rx + lost));
        if (ppm > worst_ppm) worst_ppm = ppm;
    }
    if (!any) return Colour::Blue;
    if (dead || worst_ppm > 500000) return Colour::Red;
    if (worst_ppm >= 100000) return Colour::Yellow;
    return Colour::Green;
}

void render(Colour c) {
    switch (c) {
        case Colour::Blue: show(0, 0, kBrightness); break;
        case Colour::Green: show(0, kBrightness, 0); break;
        case Colour::Yellow: show(kBrightness, kBrightness, 0); break;
        case Colour::Red: show(kBrightness, 0, 0); break;
    }
}

}  // namespace status_led

// ---------------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------------

void link_task(void*) {
    g_node->start();
    uint32_t next_button_poll_ms = now_ms_();
    uint32_t next_led_ms = now_ms_() + status_led::kUpdateMs;

    for (;;) {
        // Send completions first: cheap, and they carry the MAC-layer ACK an in-flight probe waits
        // for.
        TxDoneSlot done;
        while (espnow_tx_done_pop(done)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_node->on_tx_done(done.dst_mac, done.ok != 0, done.done_us);
            xSemaphoreGive(g_mutex);
        }

        const uint32_t t = now_ms_();
        uint32_t wait_ms = g_node->next_deadline_in_ms(t);
        if (next_button_poll_ms > t && (next_button_poll_ms - t) < wait_ms) {
            wait_ms = next_button_poll_ms - t;
        }

        // Drain a bounded number of frames, then always fall through to the timer work. Looping
        // back after each frame would be simpler and wrong: once a deadline has passed the wait is
        // zero, so a sender fast enough to keep the queue non-empty would starve the beacon — and a
        // starved beacon means the peer declares *us* dead while we are busy listening to it.
        // Cap the sleep so the serial link is still polled on a node whose radio never started:
        // espnow_rx_pop() on a dead radio would block for the whole wait.
        if (wait_ms > 20) {
            wait_ms = 20;
        }

        // This loop's only sleep is inside espnow_rx_pop(), and on a node whose radio never came up
        // that function returns *immediately* rather than blocking — so the loop became a hard busy
        // spin at priority 6. Nothing crashed and nothing logged; the statistics task at priority 3
        // simply never ran again, which presents as a node that boots perfectly and then goes silent
        // for ever. Found under emulation, but §8.1 makes this a hardware path too: a node whose
        // radio fails to start is meant to keep running *and stay diagnosable*, and starving the one
        // task that reports anything is the opposite of that.
        //
        // So the sleep is now the loop's own responsibility rather than a side effect of whichever
        // transport happens to exist. `blocked` says whether something already waited.
        const bool radio_can_block = espnow_up();

        RxSlot rx;
        if (espnow_rx_pop(rx, wait_ms)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            tee_frame("rx", rx.src_mac, rx.rssi, rx.data, rx.len);
            g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            for (size_t drained = 1; drained < kRxRingSlots && espnow_rx_pop(rx, 0); ++drained) {
                tee_frame("rx", rx.src_mac, rx.rssi, rx.data, rx.len);
                g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            }
            xSemaphoreGive(g_mutex);
        }

#if CONFIG_POT_SERIAL_LINK
        // Frames from the host, handed to the same Node as radio frames. A host is not a special
        // case above the transport — §8.1 says a mode transition is a non-event, and that only holds
        // if the code does not branch on where a frame came from.
        {
            static uint8_t sbuf[kEspNowV2LinkMtu];
            size_t slen = 0;
            uint32_t srecv = 0;
            while (serial_port_rx_pop(sbuf, sizeof(sbuf), slen, srecv)) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                tee_frame("rx", kHostMac, 0, sbuf, slen);
                g_node->on_rx(kHostMac, sbuf, slen, srecv, 0);
                xSemaphoreGive(g_mutex);
            }
        }
#endif

        const uint32_t nt = now_ms_();
        if (static_cast<int32_t>(nt - next_button_poll_ms) >= 0) {
            next_button_poll_ms = nt + 50;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            bye_button_poll();
            xSemaphoreGive(g_mutex);
        }

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_node->tick(nt);
        xSemaphoreGive(g_mutex);

        if (static_cast<int32_t>(nt - next_led_ms) >= 0) {
            next_led_ms = nt + status_led::kUpdateMs;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const status_led::Colour colour = status_led::assess(*g_node);
            xSemaphoreGive(g_mutex);
            status_led::render(colour);  // outside the lock: the RMT never holds up the node
        }

        // Nothing above could have blocked, so yield explicitly. One tick minimum even when a
        // deadline has already passed: this task must never be able to spin, whatever the transports
        // below it are doing. At the 1 ms tick that still polls the serial link at 1 kHz, which is
        // far faster than a host talks, while leaving the CPU to everything else.
        if (!radio_can_block) {
            vTaskDelay(wait_ms == 0 ? 1 : pdMS_TO_TICKS(wait_ms));
        }
    }
}

void emit_boot_record() {
    const esp_app_desc_t* desc = esp_app_get_description();
    const DramProfile& d = dram_profile();
    const NodeConfig& cfg = g_node->config();

    BootRecord r{};
    r.node_id = cfg.node_id;
    r.boot_epoch = cfg.boot_epoch;
    std::memcpy(r.mac, cfg.mac, kMacLen);
    r.espnow_version = cfg.espnow_version;
    r.channel = kChannel;
    r.hb_period_ms = cfg.hb_period_ms;
    r.hb_miss_limit = cfg.hb_miss_limit;
    r.fw_version = (desc != nullptr) ? desc->version : "?";
    r.idf_version = (desc != nullptr) ? desc->idf_ver : "?";
    r.dram_at_boot = d.at_boot;
    r.dram_after_nvs = d.after_nvs;
    r.dram_after_netif = d.after_netif;
    r.dram_after_wifi_init = d.after_wifi_init;
    r.dram_after_wifi_start = d.after_wifi_start;
    r.dram_after_espnow = d.after_espnow;
    r.dram_largest_block = d.largest_block_after_espnow;

    if (write_boot_json(g_json, sizeof(g_json), r) > 0) {
        std::fputs(g_json, stdout);
    }
    ESP_LOGI(kTag, "beacon mode: %s%s", beacon_mode_str(cfg.beacon_mode),
             g_no_radio ? "  [NO RADIO - measurements void]" : "");

    // §6's [MEASURE] trigger, evaluated on the board rather than left for someone to notice.
    if (d.exceeds_section6_expectation()) {
        ESP_LOGW(kTag,
                 "Wi-Fi stack used %u B of DRAM, above §6's ~40 KB expectation - §6 says the RX "
                 "ring shrinks first",
                 static_cast<unsigned>(d.wifi_stack_bytes()));
    }
}

// Refresh the resources this node owns. Called each statistics interval, which is also their
// staleness bound's timescale — a resource updated far less often than its bound would report GOOD
// while being anything but, and §4 rule 2 is only as good as the bounds behind it.
void refresh_sys_resources() {
    const uint16_t me = g_node->config().node_id;

    int8_t worst_rssi = 0;
    uint32_t alive = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const PeerLink& p = g_node->peers().slot(i);
        if (p.state != PeerState::Alive) {
            continue;
        }
        ++alive;
        if (p.last_rssi < worst_rssi) {
            worst_rssi = p.last_rssi;
        }
    }

    // publish(), not write_local(): these are read-only to the cluster and written constantly by
    // the node that owns them. Access governs the wire, not the driver.
    g_node->publish(sys_resource_hash(me, SysResource::HeapFree),
                    Value::of_u32(free_internal_dram()));
    g_node->publish(sys_resource_hash(me, SysResource::HeapLargest),
                    Value::of_u32(largest_free_internal_block()));
    g_node->publish(sys_resource_hash(me, SysResource::UptimeSeconds),
                    Value::of_u32(now_ms_() / 1000));
    g_node->publish(sys_resource_hash(me, SysResource::BootEpoch),
                    Value::of_u32(g_node->config().boot_epoch));
    g_node->publish(sys_resource_hash(me, SysResource::PeersAlive), Value::of_u32(alive));
    g_node->publish(sys_resource_hash(me, SysResource::LinkRssi),
                    Value::of_i32(static_cast<int32_t>(worst_rssi)));
}

// One line per namespace entry, so a capture shows what this node offers and what it currently
// believes about it. §4 rule 2's tuple in full, including for entries that have no value.
void emit_ns_records() {
    const uint16_t me = g_node->config().node_id;
    char vbuf[40];
    for (size_t i = 0; i < Namespace::capacity(); ++i) {
        NsEntry e;
        bool present = false;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (!g_node->ns().slot(i).free()) {
            e = g_node->ns().slot(i);
            present = true;
        }
        xSemaphoreGive(g_mutex);
        if (!present) {
            continue;
        }

        Reading r;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        const NsError st = g_node->read(e.path_hash, r);
        xSemaphoreGive(g_mutex);
        if (st != NsError::Ok) {
            continue;
        }

        r.value.format(vbuf, sizeof(vbuf));
        std::snprintf(g_json, sizeof(g_json),
                      "{\"t\":\"ns\",\"node\":%u,\"hash\":%lu,\"owner\":%u,"
                      "\"kind\":\"%s\",\"unit\":%u,\"class\":%u,\"bound_ms\":%lu,"
                      "\"quality\":\"%s\",\"age_ms\":%lu,\"ts\":%lu,\"updates\":%lu,"
                      "\"value\":%s}\n",
                      static_cast<unsigned>(me), static_cast<unsigned long>(e.path_hash),
                      static_cast<unsigned>(e.owner_node),
                      resource_kind_str(static_cast<ResourceKind>(e.kind)),
                      static_cast<unsigned>(e.unit), static_cast<unsigned>(e.latency_class),
                      static_cast<unsigned long>(e.staleness_bound_ms), quality_str(r.quality),
                      static_cast<unsigned long>(r.age_ms),
                      static_cast<unsigned long>(r.timestamp_ms),
                      static_cast<unsigned long>(e.update_count),
                      r.usable() ? vbuf : "null");
        std::fputs(g_json, stdout);
    }
}

void stats_task(void*) {
    emit_boot_record();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kStatsIntervalMs));
        const uint16_t node_id = g_node->config().node_id;

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        refresh_sys_resources();
        xSemaphoreGive(g_mutex);

        // Events first, so one that preceded a statistics line appears before it.
        for (;;) {
            Event e{};
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const bool got = g_node->events().pop(e);
            xSemaphoreGive(g_mutex);
            if (!got) break;
            if (write_event_json(g_json, sizeof(g_json), node_id, e) > 0) {
                std::fputs(g_json, stdout);
            }
        }

        // One line per peer. The mutex is held only for the copy.
        for (size_t i = 0; i < kMaxPeers; ++i) {
            PeerLink snapshot;
            RttHistogram hist;
            bool present = false;

            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (g_node->peers().slot(i).state != PeerState::Free) {
                snapshot = g_node->peers().slot(i);
                hist = g_node->peers().histogram(i);
                present = true;
            }
            xSemaphoreGive(g_mutex);
            if (!present) continue;

            LinkRecord lr{};
            lr.uptime_ms = now_ms_();
            lr.node_id = node_id;
            lr.peer_node_id = snapshot.node_id;
            lr.peer = &snapshot;
            lr.histogram = &hist;
            lr.last_rssi = snapshot.last_rssi;
            if (write_link_json(g_json, sizeof(g_json), lr) > 0) {
                std::fputs(g_json, stdout);
            }
        }

        NodeCounters counters;
        size_t alive = 0, dead = 0;
        uint32_t events_dropped = 0;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        counters = g_node->counters();
        alive = g_node->peers().count_in_state(PeerState::Alive);
        dead = g_node->peers().count_in_state(PeerState::Dead);
        events_dropped = g_node->events().dropped();
        xSemaphoreGive(g_mutex);

        const EspNowQueueStats q = espnow_queue_stats();
        NodeRecord nr{};
        nr.uptime_ms = now_ms_();
        nr.node_id = node_id;
        nr.boot_epoch = g_node->config().boot_epoch;
        nr.counters = &counters;
        nr.free_dram_now = free_internal_dram();
        nr.largest_free_block = largest_free_internal_block();
        nr.rx_queue_dropped = q.rx_dropped_queue_full + q.rx_dropped_oversize;
        nr.tx_done_queue_dropped = q.tx_done_dropped;
        nr.event_ring_dropped = events_dropped;
        nr.peers_alive = static_cast<uint32_t>(alive);
        nr.peers_dead = static_cast<uint32_t>(dead);
        nr.no_radio = g_no_radio;
        if (write_node_json(g_json, sizeof(g_json), nr) > 0) {
            std::fputs(g_json, stdout);
        }

        emit_ns_records();
        std::fflush(stdout);
    }
}

}  // namespace
}  // namespace pot

#if CONFIG_POT_SELFTEST
extern "C" int pot_selftest_run(void);
#endif

extern "C" void app_main(void) {
    using namespace pot;

#if CONFIG_POT_SELFTEST
    // First, and then nothing else. The self-test starts no radio, no serial link and no tasks of
    // its own beyond the one it pins to core 1, so its console output is only its own -- which is
    // what lets tools\run_selftest.ps1 read a verdict off the console instead of parsing around a
    // running node's statistics lines.
    const int failed = pot_selftest_run();
    ESP_LOGI(kTag, "selftest finished with %d failure(s)", failed);
    return;
#endif

    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_buf);

    EspNowConfig ecfg;
    ecfg.channel = kChannel;
#ifdef CONFIG_POT_LONG_RANGE
    ecfg.long_range = true;
#else
    ecfg.long_range = false;
#endif

#if CONFIG_POT_RADIO_DISABLE
    // No radio by build-time choice. NVS still comes up, because the boot epoch lives there and a
    // node without a radio still has an incarnation.
    EspNowInitReport rep{};
    rep.ok = false;
    rep.failed_at = "radio disabled at build time";
    nvs_init_once();
    ESP_LOGW(kTag, "built with CONFIG_POT_RADIO_DISABLE: serial and namespace only");
#else
    const EspNowInitReport rep = espnow_start(ecfg);
#endif
    if (!rep.ok) {
        // The radio did not come up. Say so on every statistics line rather than halting.
        //
        // Halting was the first instinct and it was wrong twice over. §8.1 makes Mode C the base
        // state, so a node with no radio is degraded rather than broken, and a node that is still
        // answering over its serial link is diagnosable while a halted one is a mystery. It is also
        // the only way the firmware runs under QEMU, which emulates the S3's CPU, memory and UARTs
        // but has no Wi-Fi device at all.
        //
        // What must not happen is a run like this being mistaken for a measurement: g_no_radio is
        // reported in the boot record and in every node record, so a soak taken without a radio is
        // self-evidently void.
        g_no_radio = true;
        ESP_LOGE(kTag, "ESP-NOW bring-up FAILED at %s: 0x%x - continuing without a radio.",
                 rep.failed_at != nullptr ? rep.failed_at : "?",
                 static_cast<unsigned>(rep.last_error));
        ESP_LOGE(kTag, "No link measurement from this boot is valid. Serial and namespace only.");
    }

    NodeConfig cfg;
#if CONFIG_POT_RADIO_DISABLE
    // esp_wifi_get_mac() is unavailable, so take the factory MAC straight from efuse. It is the same
    // number the radio would have reported, which keeps node ids stable between a radio-less build
    // and a real one on the same board.
    esp_read_mac(cfg.mac, ESP_MAC_WIFI_STA);
#else
    std::memcpy(cfg.mac, rep.mac, kMacLen);
#endif
    cfg.espnow_version = static_cast<uint8_t>(rep.espnow_version);
    cfg.boot_epoch = boot_epoch_next();
    cfg.hb_period_ms = CONFIG_POT_HB_PERIOD_MS;
    cfg.hb_miss_limit = CONFIG_POT_HB_MISS_LIMIT;
    cfg.hello_interval_ms = CONFIG_POT_HELLO_INTERVAL_MS;
    // Polarity is "opt in to the bad one", because the knob is a plain bool rather than the Kconfig
    // `choice` it wants to be — a choice member cannot be set from an sdkconfig.defaults overlay and
    // fails *silently*, which for a knob whose only purpose is a scripted A/B measurement would mean
    // broadcast numbers reported as unicast. See Kconfig.projbuild for the measurement that showed it.
#if CONFIG_POT_BEACON_UNICAST
    cfg.beacon_mode = BeaconMode::UnicastFullMesh;
#else
    cfg.beacon_mode = BeaconMode::BroadcastBeacon;
    cfg.probe_interval_ms = CONFIG_POT_PROBE_INTERVAL_MS;
#endif

    // Derive the node id from the MAC unless one is configured, so both boards take the same
    // binary. §5.1 reserves 0x0000 and 0xFFFF, so a derived value landing on either is nudged off.
    cfg.node_id = CONFIG_POT_NODE_ID;
    if (cfg.node_id == 0) {
        // A blank MAC is not a MAC. Under QEMU the efuse MAC block is unset and this reads all
        // zeros; on real hardware an unprogrammed efuse block does the same. Either way the derived
        // id lands on the reserved 0x0000, gets nudged to 0x0001, and *every* such node claims the
        // same id — which presents as a mesh where peers appear and vanish for no visible reason.
        // §5.1 reserves 0x0000 precisely so "unprovisioned" is representable; say so rather than
        // quietly inventing an identity.
        bool blank = true;
        for (size_t i = 0; i < kMacLen; ++i) {
            if (cfg.mac[i] != 0) {
                blank = false;
                break;
            }
        }
        if (blank) {
            ESP_LOGE(kTag,
                     "MAC efuse reads 00:00:00:00:00:00 - no identity to derive a node id from. "
                     "Set CONFIG_POT_NODE_ID; otherwise every such node claims 0x0001.");
        }
        cfg.node_id = static_cast<uint16_t>((cfg.mac[4] << 8) | cfg.mac[5]);
        if (cfg.node_id == kNodeUnprovisioned || cfg.node_id == kNodeBroadcast) {
            cfg.node_id = 0x0001;
        }
    }

    NodeHal hal;
    hal.ctx = nullptr;
    hal.send = &hal_send;
    hal.add_peer = &hal_add_peer;
    hal.now_ms = &hal_now_ms;
    hal.now_us = &hal_now_us;
    hal.free_dram = &hal_free_dram;
    hal.on_event = &hal_on_event;

    // Placement new into static storage: the node owns the peer table and the histograms, which §6
    // budgets statically, and there is no heap allocation anywhere on this path.
    g_node = new (g_node_storage) Node(cfg, hal);

    // §7.2: declare what this node owns. Six built-ins every board has, so a fresh fleet has
    // something real to read across a link on the day it is switched on.
    const size_t declared = declare_sys_resources(g_node->ns(), cfg.node_id);
    ESP_LOGI(kTag, "namespace: %u resources declared", static_cast<unsigned>(declared));

#if CONFIG_POT_SERIAL_LINK
    {
        SerialPortConfig scfg;
        scfg.uart_num = 1;
        scfg.tx_gpio = CONFIG_POT_SERIAL_TX_GPIO;
        scfg.rx_gpio = CONFIG_POT_SERIAL_RX_GPIO;
        scfg.baud = CONFIG_POT_SERIAL_BAUD;
        if (!serial_port_start(scfg)) {
            ESP_LOGW(kTag, "frame link unavailable; radio only");
        }
    }
#endif

    bye_button_init();
    status_led::init();

    // The broadcast address needs a peer entry before anything can be broadcast, and it consumes
    // one of §3's twenty slots — leaving nineteen for unicast.
    if (espnow_up()) {
        if (!espnow_add_peer(kBroadcastMacAddr, kChannel)) {
            ESP_LOGE(kTag, "could not add the broadcast peer");
        }
    }

    ESP_LOGI(kTag, "Potluck M0 node 0x%04x epoch %u, heartbeat %u ms x %u misses = %u ms",
             cfg.node_id, static_cast<unsigned>(cfg.boot_epoch),
             static_cast<unsigned>(cfg.hb_period_ms), static_cast<unsigned>(cfg.hb_miss_limit),
             static_cast<unsigned>(cfg.hb_period_ms * cfg.hb_miss_limit));

    // The link task owns core 0 alongside the Wi-Fi task; statistics go to core 1 so a slow console
    // can never delay a heartbeat. On a single-core build there is no core 1 to pin to, and asking
    // for one is an assertion failure rather than a graceful fallback — so the target's actual core
    // count decides, not a #ifdef for one particular chip.
    constexpr BaseType_t kStatsCore = (portNUM_PROCESSORS > 1) ? 1 : 0;

    xTaskCreateStaticPinnedToCore(link_task, "pot_link",
                                  sizeof(g_link_stack) / sizeof(StackType_t), nullptr, 6,
                                  g_link_stack, &g_link_tcb, 0);
    xTaskCreateStaticPinnedToCore(stats_task, "pot_stats",
                                  sizeof(g_stats_stack) / sizeof(StackType_t), nullptr, 3,
                                  g_stats_stack, &g_stats_tcb, kStatsCore);
}
