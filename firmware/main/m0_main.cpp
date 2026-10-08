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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>  // placement new, for constructing the Node into static storage

#include "actor_table.hpp"
#include "extender.hpp"
#include "pot/boot_epoch.hpp"
#if CONFIG_POT_CAN
#include "pot/can_port.hpp"
#include "pot/frame.hpp"
#endif
#include "pot/deploy.hpp"
#include "pot/deploy_esp.hpp"
#include "pot/dram_probe.hpp"
#include "pot/espnow_port.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/reconcile.hpp"
#include "pot/actor.hpp"
#include "pot/serial_port.hpp"
#include "pot/stats_json.hpp"
#include "pot/sys_resources.hpp"
#include "pot/trust.hpp"
#include "pot/trust_store.hpp"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
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

#if CONFIG_POT_RECONCILER
// M6: section 7.7's reconciler, created only when the running image has portable actors. Touched by
// the link task and, for its stats line, the stats task -- both under g_mutex, never from an ISR.
EXT_RAM_BSS_ATTR alignas(Reconciler) uint8_t g_rec_storage[sizeof(Reconciler)];
#endif
Reconciler* g_rec = nullptr;

// M8.1: the image's actors pinned to this node, built from the registration table (actor_table.cpp)
// and driven through the actor API -- tick, CALL results, stats -- with no type known here. PSRAM when
// present: only tasks touch it.
EXT_RAM_BSS_ATTR ActorRuntime g_actors(app::kActorTable, app::kActorTableLen);
ActorEnv g_actor_env;

StaticSemaphore_t g_mutex_buf;
SemaphoreHandle_t g_mutex = nullptr;

// The stats line buffer. Static rather than on stats_task's stack: 1 KB is a quarter of that task's
// 4 KB, and a stack overflow nineteen hours into a soak is the worst way to lose a measurement.
// This 1 KB is M0 instrumentation, not §6 core — M2's bridge tees binary frames and it goes away.
// M6: in PSRAM when the build has it -- only the stats and console tasks touch it.
EXT_RAM_BSS_ATTR char g_json[kJsonLineMax];

// Set when the radio failed to start. Reported everywhere so a boot without a radio can
// never be mistaken for a valid soak.
bool g_no_radio = false;

StaticTask_t g_link_tcb;
// 5 KB since M5 step 4: every unicast frame is tagged and checked here (HMAC-SHA-512, ~0.4 KB of
// stack); the heavier Ed25519/X25519 work goes to the crypto worker instead (run_heavy).
StackType_t g_link_stack[5120 / sizeof(StackType_t)];
TaskHandle_t g_link_handle = nullptr;
// M5: the lowest free stack the crypto worker (the console task) has had after a job, in bytes.
uint32_t g_heavy_stack_free_min = 0;
StaticTask_t g_stats_tcb;
StackType_t g_stats_stack[4096 / sizeof(StackType_t)];

uint32_t now_ms_() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
uint32_t now_us_() { return static_cast<uint32_t>(esp_timer_get_time()); }

// ---------------------------------------------------------------------------------------------
// Frame tee — §7.6's capture stream at M0's scale. Off by default: at ~40 lines/second the console
// UART's cost lands inside the timing being measured. On to debug the wire, off to measure it.
// ---------------------------------------------------------------------------------------------
#if CONFIG_POT_RSSI_TEE
// M8.2 (PS-0): the radio metadata of the frame on_rx is handling now. The drain loop sets it around
// each on_rx call; the node's sample hook, called synchronously inside, adds what only the node knows.
const RxSlot* g_rx_now = nullptr;
void rssi_tee(void*, const RxSample& s) {
    static const char* const kKind[] = {"?", "beacon", "unicast", "bcast"};
    std::printf("{\"t\":\"rssi\",\"us\":%u,\"peer\":%u,\"rssi\":%d,\"nf\":%d,\"sig\":%u,\"rate\":%u,"
                "\"kind\":\"%s\",\"seq\":%u,\"rel\":%d}\n",
                static_cast<unsigned>(s.recv_us), static_cast<unsigned>(s.node_id), static_cast<int>(s.rssi),
                g_rx_now != nullptr ? static_cast<int>(g_rx_now->noise_floor) : 0,
                g_rx_now != nullptr ? static_cast<unsigned>(g_rx_now->sig_mode) : 0u,
                g_rx_now != nullptr ? static_cast<unsigned>(g_rx_now->rate) : 0u,
                kKind[s.kind < 4 ? s.kind : 0], static_cast<unsigned>(s.hb_seq), s.relayed ? 1 : 0);
}
#endif

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
// M5 test instrument: the last unicast WRITE this node put on the radio, byte for byte -- what an
// attacker in range would record. `POT! replay` sends it again unchanged, so a receiver's replay
// window can be shown rejecting a genuine frame. It only ever re-sends this node's own frame.
// M5.1 bench instrument (ADR-009): MACs this board pretends not to hear, by their last two bytes, so
// three boards on one desk can act out "out of range" for the relay test. Empty in normal use.
uint16_t g_deaf[4] = {0, 0, 0, 0};
bool deaf_to(const uint8_t mac[kMacLen]) {
    const uint16_t tail = static_cast<uint16_t>((mac[4] << 8) | mac[5]);
    for (uint16_t d : g_deaf) {
        if (d != 0 && d == tail) return true;
    }
    return false;
}

uint8_t g_rec_write[64];
size_t g_rec_write_len = 0;
uint8_t g_rec_write_mac[kMacLen];
// Likewise the last SAFE_STATE this node broadcast. `POT! safe` also saves it to NVS, so it survives
// the power cycle that section 8.3's replay fence is about, and `POT! replay-ss` re-sends it after.
uint8_t g_rec_ss[kHeaderSize + 80];
size_t g_rec_ss_len = 0;

int32_t hal_send(void*, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    tee_frame("tx", mac, 0, data, len);
    if (len <= sizeof(g_rec_write) && len > kHeaderSize && data[6] == kOpWrite &&
        std::memcmp(mac, kBroadcastMacAddr, kMacLen) != 0) {
        std::memcpy(g_rec_write, data, len);
        g_rec_write_len = len;
        std::memcpy(g_rec_write_mac, mac, kMacLen);
    }
    if (len <= sizeof(g_rec_ss) && len > kHeaderSize && data[6] == kOpSafeState &&
        std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0) {
        std::memcpy(g_rec_ss, data, len);
        g_rec_ss_len = len;
    }
#if CONFIG_POT_CAN
    // A CAN peer's address, or a broadcast: the bus. In a CAN build broadcasts go to CAN only --
    // v1 has no router that could keep one node reachable on two transports from appearing as two
    // peers (§7.0), so a CAN node is a CAN node. M4 builds these with the radio disabled.
    {
        uint16_t can_node = 0;
        if (can_mac_node(mac, can_node) || std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0) {
            return can_send(mac, data, len);
        }
    }
#endif
#if CONFIG_POT_SERIAL_LINK
    if (std::memcmp(mac, kHostMac, kMacLen) == 0) {
        return serial_port_send(data, len);
    }
#endif
    return espnow_send(mac, data, len);
}

bool hal_add_peer(void*, const uint8_t mac[kMacLen]) {
#if CONFIG_POT_CAN
    {
        uint16_t can_node = 0;
        if (can_mac_node(mac, can_node)) {
            return can_up();  // the bus has no peer list to join
        }
    }
#endif
#if CONFIG_POT_SERIAL_LINK
    if (std::memcmp(mac, kHostMac, kMacLen) == 0) {
        // The UART has no peer list to join; the link either exists or it does not.
        return serial_port_running();
    }
#endif
    // Channel 0: "use the current channel" (esp_now.h), so peers follow a cell that moves (M5.1).
    return espnow_add_peer(mac, 0);
}
uint32_t hal_now_ms(void*) { return now_ms_(); }
uint32_t hal_now_us(void*) { return now_us_(); }
uint32_t hal_free_dram(void*) { return free_internal_dram(); }

// M6.1: the cell's channel survives a reboot. Every board used to boot on CONFIG_POT_CHANNEL, so a
// deploy -- which reboots the whole cell -- put everyone back on the default while a router-owned
// station stayed on its router's channel, and a freshly booted node knows of no channel authority to
// sweep for. Found thinking through M6.1 step 2's retest (M0-LOG session 29). The stats task, which has
// the stack for an NVS write, saves it.
//
// Only a channel the CELL is on: one with at least one radio peer alive on it (M0-LOG session 31). It
// used to save every change, so the extender -- whose channel is its router's -- saved channel 11 while
// it sat alone there, split from a cell on 1; with CR-7 that saved value then became its preferred
// channel at the next boot, pulling the cell after it instead of keeping it where it was.
namespace chan_store {
constexpr const char* kNs = "potchan";
uint8_t load() {
    nvs_handle_t h;
    uint8_t ch = 0;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "chan", &ch) != ESP_OK) ch = 0;
    nvs_close(h);
    return ch;
}
void save_if_with_cell(uint8_t ch, size_t radio_peers_alive) {
    static uint8_t s_saved = 0;  // this boot's last write, to spare NVS a read every period
    if (ch < 1 || ch > 14 || radio_peers_alive == 0 || ch == s_saved) return;
    s_saved = ch;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t old = 0;
    if (nvs_get_u8(h, "chan", &old) != ESP_OK || old != ch) {
        nvs_set_u8(h, "chan", ch);
        nvs_commit(h);
    }
    nvs_close(h);
}
}  // namespace chan_store

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
constexpr uint32_t kUpdateMs = 1000;  // how often the colour is reassessed
constexpr uint32_t kRenderMs = 100;   // how often it is drawn, so a deployed blink can blink

// What "healthy" looks like. Built-in: steady green. A deployed Led actor (M3) replaces it; blue,
// yellow and red keep their diagnostic meaning whatever is deployed, so no deployment can make a
// failing link look healthy.
LedConfig g_healthy{0, 255, 0, 0, 1000};
bool g_custom = false;
void set_healthy(const LedConfig& c) {
    g_healthy = c;
    g_custom = true;
}

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
    // Drawn every 100 ms now; only touch the RMT when the colour actually changes.
    static int last = -1;
    const int now = (r << 16) | (g << 8) | b;
    if (now == last) return;
    last = now;
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

uint8_t scaled(uint8_t v) {
    if (v == 0) return 0;
    const uint32_t s = static_cast<uint32_t>(v) * kBrightness / 255u;
    return static_cast<uint8_t>(s == 0 ? 1 : s);
}

void render(Colour c, uint32_t now_ms) {
    switch (c) {
        case Colour::Blue: show(0, 0, kBrightness); break;
        case Colour::Green:
            if (!g_custom) {
                show(0, kBrightness, 0);
            } else if (g_healthy.blink && (now_ms % g_healthy.period_ms) >= g_healthy.period_ms / 2) {
                show(0, 0, 0);
            } else {
                show(scaled(g_healthy.r), scaled(g_healthy.g), scaled(g_healthy.b));
            }
            break;
        case Colour::Yellow: show(kBrightness, kBrightness, 0); break;
        case Colour::Red: show(kBrightness, 0, 0); break;
    }
}

}  // namespace status_led


// ---------------------------------------------------------------------------------------------
// Deploy and detach (§7.4, M3). The image is a list of built-in actors and their configuration
// (ADR-003 Tier 0); pot_deploy holds the format, the A/B state machine and the receiver.
// ---------------------------------------------------------------------------------------------
namespace deploy_rt {

constexpr uint8_t kMaxTrialBoots = CONFIG_POT_TRIAL_MAX_BOOTS;
constexpr uint32_t kTrialHeartbeats = CONFIG_POT_TRIAL_HEARTBEATS;
constexpr uint32_t kRebootDelayMs = 1500;  // long enough for the REPLY to leave first

EspSlotStore g_store;
DeployState g_state{};
BootOutcome g_outcome = BootOutcome::NoDeployment;
// One buffer, used twice: by boot() to load the active slot before any frame can arrive, then by
// the receiver. Nothing keeps pointers into it after boot -- the actors' settings are copied out.
EXT_RAM_BSS_ATTR uint8_t g_rx_buf[kMaxImageLen];  // M6: PSRAM when present; tasks only
uint8_t* const g_image = g_rx_buf;
DeployReceiver g_rx(g_store, g_rx_buf, sizeof(g_rx_buf));
bool g_trial = false;            // running a pending slot, not yet confirmed
uint32_t g_reboot_at_ms = 0;     // 0 = no reboot scheduled
uint16_t g_node_id = 0;
// M6: the image's portable actors, handed to the reconciler once the node exists.
EXT_RAM_BSS_ATTR PortableSpec g_portable[kMaxPortable];  // M8.1: any portable actor type
size_t g_portable_n = 0;

const char* slot_name(uint8_t s) { return s == kSlotA ? "A" : s == kSlotB ? "B" : "none"; }

void save() {
    if (!g_store.save_state(g_state)) {
        ESP_LOGE(kTag, "deploy: could not persist the A/B state");
    }
}

// Read the active slot, validate every actor meant for this node, and only then apply them: a
// half-applied image is worse than either whole one.
bool load_and_apply(const char** why) {
    size_t len = 0;
    if (!g_store.read_slot(g_state.active, g_image, kMaxImageLen, len)) {
        *why = "slot unreadable or fails its CRC";
        return false;
    }
    DeployImage img{};
    if (!parse_image(g_image, len, img, why)) {
        return false;
    }
    g_actor_env.trial_window_ms = kTrialHeartbeats * static_cast<uint32_t>(CONFIG_POT_HB_PERIOD_MS);
    // Every node takes every portable actor: section 7.7's "code moves at deploy time, not at failure
    // time". The reconciler decides where each one runs; the registration table says what each is.
    size_t portable_n = 0;
#if CONFIG_POT_RECONCILER
    if (!collect_portable(img, app::kActorTable, app::kActorTableLen, g_portable, kMaxPortable, portable_n, why)) {
        return false;
    }
#else
    for (uint8_t i = 0; i < img.actor_count; ++i) {
        if (img.actors[i].node_id == kPortableNode) {
            *why = "portable actor, and this build has no reconciler (CONFIG_POT_RECONCILER)";
            return false;
        }
    }
#endif
    // M8.1: this node's pinned actors, every one validated against its row in the registration table
    // before any is kept. Last, so that a refusal anywhere above leaves nothing of this image held.
    if (!g_actors.load(img, g_node_id, g_actor_env, why)) {
        return false;
    }
    g_portable_n = portable_n;
    ESP_LOGI(kTag, "deploy: image counter %u, digest %02x%02x%02x%02x..., %u actor(s) in the image",
             static_cast<unsigned>(img.rollback_counter), img.package_digest[0],
             img.package_digest[1], img.package_digest[2], img.package_digest[3],
             static_cast<unsigned>(img.actor_count));
    return true;
}

// Before any task starts. The trial counter is persisted *before* the slot runs, so a module that
// crashes the node has already been counted when it does.
void boot(uint16_t node_id) {
    g_node_id = node_id;
    if (!g_store.init()) {
        return;
    }
    g_store.load_state(g_state);
    g_outcome = decide_boot(g_state, kMaxTrialBoots);
    save();
    for (int attempt = 0; attempt < 2 && g_state.active != kSlotNone; ++attempt) {
        const char* why = "?";
        if (load_and_apply(&why)) {
            break;
        }
        if (g_state.pending != 0) {
            ESP_LOGE(kTag, "deploy: slot %s cannot run (%s) - reverting now", slot_name(g_state.active), why);
            revert_now(g_state);
            save();
            g_outcome = BootOutcome::Reverted;
            continue;
        }
        ESP_LOGE(kTag, "deploy: confirmed slot %s cannot run (%s) - built-in behaviour",
                 slot_name(g_state.active), why);
        break;
    }
    g_trial = g_state.pending != 0;
    ESP_LOGI(kTag, "deploy: boot %s, running slot %s%s, trial boot %u/%u, counter %u",
             boot_outcome_str(g_outcome), slot_name(g_state.active), g_trial ? " ON TRIAL" : "",
             static_cast<unsigned>(g_state.trial_boots), static_cast<unsigned>(kMaxTrialBoots),
             static_cast<unsigned>(g_state.counter));
}

// ---- passing a committed image on to the peers (A's half of "one artifact per system") ----------
struct Push {
    bool active = false;
    uint16_t peers[kMaxPeers];
    size_t count = 0;
    size_t idx = 0;
    uint32_t offset = 0;
    uint16_t awaiting = 0;
    uint8_t ok = 0;
    uint8_t failed = 0;
    uint16_t requester = 0;
    uint16_t requester_msg = 0;
    DeployBegin begin{};
};
Push g_push;

void schedule_reboot() { g_reboot_at_ms = now_ms_() + kRebootDelayMs; }

void push_next_peer();

void push_send(uint8_t op, const uint8_t* payload, size_t len) {
    const uint16_t msg = g_node->send_deploy(g_push.peers[g_push.idx], op, payload, static_cast<uint16_t>(len));
    if (msg == 0) {
        ESP_LOGW(kTag, "deploy: could not send to peer 0x%04x", g_push.peers[g_push.idx]);
        ++g_push.failed;
        ++g_push.idx;
        push_next_peer();
        return;
    }
    g_push.awaiting = msg;
}

void push_begin() {
    uint8_t wire[kDeployBeginLen];
    const size_t n = store_deploy_begin(g_push.begin, wire, sizeof(wire));
    g_push.offset = 0;
    push_send(kOpDeployBegin, wire, n);
}

void push_chunk() {
    const uint32_t len = g_push.begin.image_len;
    const uint16_t n = static_cast<uint16_t>(std::min<uint32_t>(kDeployChunkMax, len - g_push.offset));
    static uint8_t wire[kDeployChunkHeaderLen + kDeployChunkMax];
    const size_t w = store_deploy_chunk(g_push.offset, g_rx.image() + g_push.offset, n, wire, sizeof(wire));
    g_push.offset += n;
    push_send(kOpDeployChunk, wire, w);
}

void push_finish() {
    g_push.active = false;
    DeployReply r{};
    r.status = DeployStatus::Ok;
    r.slot = g_state.active;
    r.pending = 1;
    r.received = g_push.begin.image_len;
    r.counter = g_push.begin.rollback_counter;
    r.peers_ok = g_push.ok;
    r.peers_failed = g_push.failed;
    uint8_t wire[kDeployReplyLen];
    store_deploy_reply(r, wire, sizeof(wire));
    g_node->send_reply_raw(g_push.requester, g_push.requester_msg, wire, sizeof(wire));
    ESP_LOGI(kTag, "deploy: passed on to %u peer(s), %u failed; rebooting into slot %s",
             static_cast<unsigned>(g_push.ok), static_cast<unsigned>(g_push.failed),
             slot_name(g_state.active));
    schedule_reboot();
}

void push_next_peer() {
    if (g_push.idx >= g_push.count) {
        push_finish();
        return;
    }
    push_begin();
}

void on_result(void*, uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p,
               uint16_t len) {
    if (!g_push.active || g_push.idx >= g_push.count || peer != g_push.peers[g_push.idx] ||
        msg != g_push.awaiting) {
        return;
    }
    DeployReply r{};
    const bool ok = !timed_out && load_deploy_reply(p, len, r) && r.status == DeployStatus::Ok;
    if (!ok) {
        ESP_LOGW(kTag, "deploy: peer 0x%04x %s at step 0x%02x", peer,
                 timed_out ? "timed out" : deploy_status_str(r.status), op);
        ++g_push.failed;
        ++g_push.idx;
        push_next_peer();
        return;
    }
    if (op == kOpDeployBegin || (op == kOpDeployChunk && g_push.offset < g_push.begin.image_len)) {
        push_chunk();
    } else if (op == kOpDeployChunk) {
        uint8_t wire[kDeployCommitLen];
        push_send(kOpDeployCommit, wire, store_deploy_commit(g_push.begin.image_crc, wire, sizeof(wire)));
    } else if (op == kOpDeployCommit) {
        ESP_LOGI(kTag, "deploy: peer 0x%04x committed", peer);
        ++g_push.ok;
        ++g_push.idx;
        push_next_peer();
    }
}

// ---- the server, called from Node::on_rx with g_mutex held ---------------------------------------
size_t on_deploy(void*, uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len,
                 uint8_t* reply, size_t cap) {
    DeployReply r{};
    r.status = DeployStatus::Malformed;
    if (!g_store.ready()) {
        r.status = DeployStatus::StoreFailed;
    } else if (g_push.active || g_reboot_at_ms != 0) {
        r.status = DeployStatus::Busy;
    } else if (op == kOpDeployBegin) {
        DeployBegin b{};
        if (load_deploy_begin(p, len, b)) r = g_rx.begin(b);
    } else if (op == kOpDeployChunk) {
        DeployChunk c{};
        if (load_deploy_chunk(p, len, c)) r = g_rx.chunk(c);
    } else if (op == kOpDeployCommit) {
        uint32_t crc = 0;
        if (load_deploy_commit(p, len, crc)) r = g_rx.commit(crc);
        if (r.status == DeployStatus::Ok) {
            g_store.load_state(g_state);
            ESP_LOGI(kTag, "deploy: committed counter %u to slot %s, on trial from the next boot",
                     static_cast<unsigned>(g_rx.header().rollback_counter), slot_name(g_state.active));
            if (g_rx.distribute()) {
                g_push = Push{};
                g_push.begin = g_rx.header();
                g_push.begin.flags &= static_cast<uint8_t>(~kDeployFlagDistribute);  // one hop, no echo
                g_push.requester = from;
                g_push.requester_msg = msg_id;
                for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                    const PeerLink& q = g_node->peers().slot(i);
                    if (q.state == PeerState::Alive && q.node_id != from) {
                        g_push.peers[g_push.count++] = q.node_id;
                    }
                }
                g_push.active = true;
                ESP_LOGI(kTag, "deploy: passing it on to %u peer(s)", static_cast<unsigned>(g_push.count));
                push_next_peer();
                return 0;  // answered by push_finish(), once the peers have committed
            }
            schedule_reboot();
        }
    } else if (op == kOpDeployAbort) {
        g_rx.abort();
        r.status = DeployStatus::Ok;
    }
    return store_deploy_reply(r, reply, cap);
}

// From link_task, every pass.
void tick(uint32_t now) {
    if (g_trial && g_node->tx_tally().beacons >= kTrialHeartbeats) {
        confirm(g_state);
        save();
        g_trial = false;
        ESP_LOGI(kTag, "deploy: trial passed after %u heartbeats - slot %s CONFIRMED",
                 static_cast<unsigned>(kTrialHeartbeats), slot_name(g_state.active));
    }
    if (g_reboot_at_ms != 0 && now >= g_reboot_at_ms) {
        ESP_LOGW(kTag, "deploy: rebooting into the new slot");
        esp_restart();
    }
}

}  // namespace deploy_rt


// ---------------------------------------------------------------------------------------------
// M4's demonstration: SAFE_STATE wins arbitration against saturating telemetry (§13-M4). Both
// roles are off unless a build asks for them, and neither exists outside a CAN build.
// ---------------------------------------------------------------------------------------------
#if CONFIG_POT_CAN
namespace m4 {

uint32_t g_flood_sent = 0;
uint32_t g_safe_rx = 0;
uint32_t g_safe_rx_last = 0;
uint32_t g_safe_rx_gaps = 0;
uint32_t g_safe_rx_restarts = 0;
uint32_t g_next_safe_ms = 0;

// Lowest-priority telemetry, as fast as the transmit pool allows -- but always leaving room, so this
// board's own beacons are never starved by its own flood. Addressed to node 0x003E, whose alias
// (62) no board on the bench has: every node still ACKs each frame, as CAN requires, and then drops
// it as not-for-us, so the bus saturates without anyone doing work.
void flood() {
#if CONFIG_POT_M4_FLOOD
    while (can_tx_free() > 16) {
        EncodeSpec spec;
        spec.src = g_node->config().node_id;
        spec.dst = 0x003E;
        spec.opcode = kOpCast;
        spec.lclass = kClassL4;
        spec.priority = 0;
        uint8_t payload[8];
        std::memcpy(payload, &g_flood_sent, sizeof(g_flood_sent));
        std::memset(payload + 4, 0xA5, 4);
        uint8_t buf[32];
        size_t n = 0;
        if (encode(spec, payload, sizeof(payload), buf, sizeof(buf), n) != FrameError::Ok) return;
        uint8_t mac[kMacLen];
        can_mac_of(0x003E, mac);
        if (can_send(mac, buf, n) != 0) return;
        ++g_flood_sent;
    }
#endif
}

void on_safe_state(void*, uint16_t from, uint32_t counter, uint16_t reason) {
    ++g_safe_rx;
    // A counter that goes backwards is the sender restarting (the first M4 run reflashed C under a
    // running B and this went "negative" to 4294966709): count the restart, not a gap.
    if (g_safe_rx_last != 0 && counter <= g_safe_rx_last) {
        ++g_safe_rx_restarts;
    } else if (g_safe_rx_last != 0 && counter != g_safe_rx_last + 1) {
        g_safe_rx_gaps += counter - g_safe_rx_last - 1;
    }
    g_safe_rx_last = counter;
    ESP_LOGI(kTag, "SAFE_STATE #%u from 0x%04x, reason %u", static_cast<unsigned>(counter), from,
             static_cast<unsigned>(reason));
}

void tick(uint32_t now) {
    flood();
#if CONFIG_POT_M4_SAFE_STATE_MS > 0
    if (static_cast<int32_t>(now - g_next_safe_ms) >= 0) {
        g_next_safe_ms = now + CONFIG_POT_M4_SAFE_STATE_MS;
        g_node->send_safe_state(1);
    }
#else
    (void)now;
#endif
}

void print_stats() {
    const CanPortStats c = can_stats();
    const CanRxCounters& r = can_rx_counters();
    std::printf("{\"t\":\"can\",\"node\":%u,\"up_ms\":%u,\"tx_frames\":%u,\"tx_ok\":%u,\"tx_fail\":%u,"
                "\"tx_no_slot\":%u,\"arb_lost\":%u,\"bit_err\":%u,\"form_err\":%u,\"stuff_err\":%u,"
                "\"ack_err\":%u,\"rx_frames\":%u,\"rx_overflow\":%u,\"rx_own\":%u,\"rx_messages\":%u,"
                "\"rx_not_for_us\":%u,\"rx_unknown_alias\":%u,\"rx_out_of_order\":%u,\"rx_timeouts\":%u,"
                "\"rx_malformed\":%u,\"flood_sent\":%u,\"ss_sent\":%u,\"ss_last_us\":%u,\"ss_min_us\":%u,"
                "\"ss_max_us\":%u,\"ss_rx\":%u,\"ss_rx_gaps\":%u,\"ss_rx_restarts\":%u}\n",
                static_cast<unsigned>(g_node->config().node_id), static_cast<unsigned>(now_ms_()),
                static_cast<unsigned>(c.tx_frames), static_cast<unsigned>(c.tx_ok),
                static_cast<unsigned>(c.tx_fail), static_cast<unsigned>(c.tx_no_slot),
                static_cast<unsigned>(c.arb_lost), static_cast<unsigned>(c.bit_err),
                static_cast<unsigned>(c.form_err), static_cast<unsigned>(c.stuff_err),
                static_cast<unsigned>(c.ack_err), static_cast<unsigned>(c.rx_frames),
                static_cast<unsigned>(c.rx_overflow), static_cast<unsigned>(c.rx_own),
                static_cast<unsigned>(r.messages), static_cast<unsigned>(r.not_for_us),
                static_cast<unsigned>(r.unknown_alias), static_cast<unsigned>(r.out_of_order),
                static_cast<unsigned>(r.timeouts), static_cast<unsigned>(r.malformed),
                static_cast<unsigned>(g_flood_sent), static_cast<unsigned>(c.safe_state_sent),
                static_cast<unsigned>(c.safe_state_last_us), static_cast<unsigned>(c.safe_state_min_us),
                static_cast<unsigned>(c.safe_state_max_us), static_cast<unsigned>(g_safe_rx),
                static_cast<unsigned>(g_safe_rx_gaps), static_cast<unsigned>(g_safe_rx_restarts));
}

}  // namespace m4
#endif

// ---------------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------------

void link_task(void*) {
    g_node->start();
    if (g_rec != nullptr) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_rec->start(now_ms_());
        xSemaphoreGive(g_mutex);
    }
    uint32_t next_button_poll_ms = now_ms_();
    uint32_t next_led_ms = now_ms_() + status_led::kRenderMs;
#if CONFIG_POT_EXTENDER
    uint32_t next_hunt_poll_ms = now_ms_();
    bool was_associated = false;  // matches the window app_main chose: hunting unless already associated
#endif
    uint32_t next_assess_ms = now_ms_() + status_led::kUpdateMs;
    status_led::Colour colour = status_led::Colour::Blue;

    for (;;) {
        // Send completions first: cheap, and they carry the MAC-layer ACK an in-flight probe waits
        // for.
        TxDoneSlot done;
        while (espnow_tx_done_pop(done)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_node->on_tx_done(done.dst_mac, done.ok != 0, done.done_us);
            xSemaphoreGive(g_mutex);
        }
#if CONFIG_POT_CAN
        {
            uint8_t cmac[kMacLen];
            bool cok = false;
            uint32_t cus = 0;
            while (can_tx_done_pop(cmac, cok, cus)) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_node->on_tx_done(cmac, cok, cus);
                xSemaphoreGive(g_mutex);
            }
            static uint8_t cbuf[kCanMaxMessage];
            size_t clen = 0;
            for (int k = 0; k < 32 && can_rx_pop(cbuf, sizeof(cbuf), clen, cmac, cus); ++k) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                tee_frame("rx", cmac, 0, cbuf, clen);
                g_node->on_rx(cmac, cbuf, clen, cus, 0);
                xSemaphoreGive(g_mutex);
            }
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            m4::tick(now_ms_());
            xSemaphoreGive(g_mutex);
        }
#endif

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
#if CONFIG_POT_RSSI_TEE
            g_rx_now = &rx;
#endif
            if (!deaf_to(rx.src_mac)) g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            for (size_t drained = 1; drained < kRxRingSlots && espnow_rx_pop(rx, 0); ++drained) {
                tee_frame("rx", rx.src_mac, rx.rssi, rx.data, rx.len);
                if (!deaf_to(rx.src_mac)) g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            }
#if CONFIG_POT_RSSI_TEE
            g_rx_now = nullptr;  // anything after this (the host's serial link) carries no radio metadata
#endif
            xSemaphoreGive(g_mutex);
        }

#if CONFIG_POT_SERIAL_LINK
        // Frames from the host, handed to the same Node as radio frames. A host is not a special
        // case above the transport — §8.1 says a mode transition is a non-event, and that only holds
        // if the code does not branch on where a frame came from.
        {
            EXT_RAM_BSS_ATTR static uint8_t sbuf[kEspNowV2LinkMtu];  // M6: PSRAM when present
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

#if CONFIG_POT_EXTENDER
        // M6.1: while the station hunts for its router, its scans take the radio off the cell's channel
        // for longer than the build's death window (3.7-4.0 s measured, M0-LOG session 30), so declare a
        // window that covers one. Polled outside the lock: the status call goes into the Wi-Fi driver.
        if (extender::active() && static_cast<int32_t>(nt - next_hunt_poll_ms) >= 0) {
            next_hunt_poll_ms = nt + 200;
            const bool assoc = extender::associated();
            if (assoc != was_associated) {
                was_associated = assoc;
                const uint8_t misses = assoc ? CONFIG_POT_HB_MISS_LIMIT : CONFIG_POT_EXTENDER_HUNT_MISSES;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_node->set_heartbeat_window(CONFIG_POT_HB_PERIOD_MS, misses);
                xSemaphoreGive(g_mutex);
                ESP_LOGI(kTag, "extender: station %s; declaring %u x %u ms", assoc ? "associated" : "hunting",
                         static_cast<unsigned>(misses), static_cast<unsigned>(CONFIG_POT_HB_PERIOD_MS));
            }
        }
#endif

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_node->tick(nt);
        if (g_rec != nullptr) g_rec->tick(nt);
        g_actors.tick(nt);
        // M6.1: the router put the radio on a channel. Tell the cell (M5.1's signed announcement), and
        // declare it from now on. The radio is already there: by the time a station knows its new
        // channel it has associated on it, so the announcement goes out on the NEW channel, and members
        // still on the old one find this node by CR-1's scan rather than by hearing it.
        if (const uint8_t rch = extender::take_channel(); rch != 0 && rch != g_node->channel()) {
            if (!g_node->move_cell(rch, 0)) {
                ESP_LOGW(kTag, "extender: router is on channel %u, outside the cell's range; not announced",
                         static_cast<unsigned>(rch));
            }
        }
        xSemaphoreGive(g_mutex);

        if (static_cast<int32_t>(nt - next_assess_ms) >= 0) {
            next_assess_ms = nt + status_led::kUpdateMs;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            colour = status_led::assess(*g_node);
            xSemaphoreGive(g_mutex);
        }
        if (static_cast<int32_t>(nt - next_led_ms) >= 0) {
            next_led_ms = nt + status_led::kRenderMs;
            status_led::render(colour, nt);  // outside the lock: the RMT never holds up the node
        }

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        deploy_rt::tick(nt);
        xSemaphoreGive(g_mutex);

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
        // First, while the console is idle: the board's clock, for a host that must put several
        // boards' events on one timeline (tools/m6_bench.py). Any later line in the period waits
        // behind up to ~3 KB of output at 115200 baud, and so is a poor timestamp.
        std::printf("{\"t\":\"clk\",\"node\":%u,\"up_ms\":%u}\n", static_cast<unsigned>(node_id),
                    static_cast<unsigned>(now_ms_()));
        extender::print_status();  // M6.1: the baseline's {"t":"hs"} line, on an extender build only
        {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const uint8_t ch = g_node->channel();
            size_t radio_alive = 0;  // a host on the cable is on no channel, so it does not count
            for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                const PeerLink& q = g_node->peers().slot(i);
                if (q.state == PeerState::Alive && std::memcmp(q.mac, kHostMac, kMacLen) != 0) ++radio_alive;
            }
            xSemaphoreGive(g_mutex);
            chan_store::save_if_with_cell(ch, radio_alive);
        }
        // M8.1: each actor's own stats line (svc_client's {"t":"svc"}, ...), through the actor API.
        for (size_t i = 0; i < g_actors.running(); ++i) {
            static char line[512];  // the stats task's only; static keeps it off a 4 KB stack
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const size_t n = g_actors.actor(i)->stats_json(line, sizeof(line), now_ms_());
            xSemaphoreGive(g_mutex);
            if (n > 0) std::printf("%s\n", line);
        }

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

#if CONFIG_POT_CAN
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m4::print_stats();
        xSemaphoreGive(g_mutex);
#else
        {
            // M5: who this node has verified, and what the checking cost.
            Node::AuthCounters ac{};
            unsigned verified_peers = 0;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            ac = g_node->auth_counters();
            for (size_t i = 0; i < kMaxPeers; ++i) {
                const PeerLink& pl = g_node->peers().slot(i);
                if (pl.state == PeerState::Free) continue;
                const Node::PeerAuth* pa = g_node->peer_auth(&pl);
                if (pa != nullptr && pa->verified) ++verified_peers;
            }
            const bool required = g_node->trust_required();
            xSemaphoreGive(g_mutex);
            const unsigned link_free = g_link_handle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_link_handle)) : 0u;
            std::printf("{\"t\":\"auth\",\"node\":%u,\"up_ms\":%u,\"enrolled\":%d,\"required\":%d,"
                        "\"verified_peers\":%u,\"verified\":%u,\"cache_hits\":%u,\"refused\":%u,"
                        "\"rate_limited\":%u,\"stale_epoch\":%u,\"verify_us_max\":%u,"
                        "\"link_stack_free_b\":%u,\"crypto_stack_free_b\":%u,"
                        "\"tags_ok\":%u,\"bad_tag\":%u,\"replayed\":%u,\"untagged\":%u,\"bcast_dropped\":%u,"
                        "\"beacon_epoch_ignored\":%u,\"tag_us_max\":%u}\n",
                        static_cast<unsigned>(node_id), static_cast<unsigned>(now_ms_()),
                        g_node->trust_enrolled() ? 1 : 0, required ? 1 : 0, verified_peers,
                        static_cast<unsigned>(ac.verified), static_cast<unsigned>(ac.cache_hits),
                        static_cast<unsigned>(ac.refused), static_cast<unsigned>(ac.rate_limited),
                        static_cast<unsigned>(ac.stale_epoch), static_cast<unsigned>(ac.verify_us_max),
                        link_free, static_cast<unsigned>(g_heavy_stack_free_min),
                        static_cast<unsigned>(ac.tags_ok), static_cast<unsigned>(ac.bad_tag),
                        static_cast<unsigned>(ac.replayed), static_cast<unsigned>(ac.untagged),
                        static_cast<unsigned>(ac.bcast_dropped), static_cast<unsigned>(ac.beacon_epoch_ignored),
                        static_cast<unsigned>(ac.tag_us_max));
            Reading sp;
            unsigned served = 0;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_node->read(path_hash("act/setpoint"), sp);
            served = g_node->ns_counters().writes_served;
            xSemaphoreGive(g_mutex);
            int32_t spv = 0;
            Quality spq;
            const bool has = sp.get_i32(spv, spq);
            std::printf("{\"t\":\"act\",\"node\":%u,\"setpoint\":%d,\"has_value\":%d,\"writes_served\":%u}\n",
                        static_cast<unsigned>(node_id), static_cast<int>(spv), has ? 1 : 0, served);
            // M5.1: each peer's declared window (CR-3) and caps (CR-4), as this node holds them.
            const Node::ChannelCounters chc = g_node->channel_counters();
            std::printf("{\"t\":\"peers\",\"node\":%u,\"own_period_ms\":%u,\"own_busy\":%d,\"channel\":%u,"
                        "\"scanning\":%d,\"scans\":%u,\"hops\":%u,\"found\":%u,\"lost_to_found_ms\":%u,"
                        "\"moves_followed\":%u,\"moves_refused\":%u,\"sweeps\":%u,\"sweep_found\":%u,\"sweeping\":%d,"
                        "\"search_visits\":%u,"
                        "\"relay\":%d,\"fwd_u\":%u,\"fwd_b\":%u,"
                        "\"fwd_h\":%u,\"relay_dup\":%u,\"list\":[",
                        static_cast<unsigned>(node_id), static_cast<unsigned>(g_node->heartbeat_period_ms()),
                        g_node->busy() ? 1 : 0, static_cast<unsigned>(g_node->channel()), g_node->scanning() ? 1 : 0,
                        static_cast<unsigned>(chc.scans_started), static_cast<unsigned>(chc.scan_hops),
                        static_cast<unsigned>(chc.found_by_scan), static_cast<unsigned>(chc.last_lost_to_found_ms),
                        static_cast<unsigned>(chc.moves_followed), static_cast<unsigned>(chc.moves_refused),
                        static_cast<unsigned>(chc.authority_sweeps), static_cast<unsigned>(chc.authority_found),
                        g_node->sweeping_for_authority() ? 1 : 0,
                        static_cast<unsigned>(chc.search_visits),
                        g_node->relay() ? 1 : 0, static_cast<unsigned>(g_node->relay_counters().unicast_forwarded),
                        static_cast<unsigned>(g_node->relay_counters().broadcast_forwarded),
                        static_cast<unsigned>(g_node->relay_counters().hello_forwarded),
                        static_cast<unsigned>(g_node->relay_counters().relay_dup));
            {
                bool first_peer = true;
                for (size_t i = 0; i < kMaxPeers; ++i) {
                    xSemaphoreTake(g_mutex, portMAX_DELAY);
                    const PeerLink pl = g_node->peers().slot(i);
                    const uint32_t caps = g_node->peer_caps(&g_node->peers().slot(i));
                    const bool via = g_node->peer_relayed(&g_node->peers().slot(i));
                    xSemaphoreGive(g_mutex);
                    if (pl.state == PeerState::Free) continue;
                    std::printf("%s{\"id\":%u,\"state\":\"%s\",\"via_relay\":%d,\"period_ms\":%u,\"misses\":%u,"
                                "\"busy\":%d,\"rtt_n\":%u}",
                                first_peer ? "" : ",", static_cast<unsigned>(pl.node_id),
                                pl.state == PeerState::Alive ? "alive" : (pl.state == PeerState::Dead ? "dead" : "other"),
                                via ? 1 : 0, static_cast<unsigned>(pl.hb_period_ms), static_cast<unsigned>(pl.miss_limit),
                                (caps & kHelloCapBusy) ? 1 : 0, static_cast<unsigned>(pl.rtt_samples));
                    first_peer = false;
                }
            }
            std::printf("]}\n");
            Node::SafeStateCounters sc{};
            Node::SafeStateFloor fl[Node::kSafeStateSources];
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            sc = g_node->safe_state_counters();
            std::memcpy(fl, g_node->safe_state_floors(), sizeof(fl));
            xSemaphoreGive(g_mutex);
            std::printf("{\"t\":\"ss\",\"node\":%u,\"accepted\":%u,\"unsigned\":%u,\"unknown_sender\":%u,"
                        "\"bad_signature\":%u,\"replayed\":%u,\"rate_limited\":%u,\"verify_us_max\":%u,\"floors\":[",
                        static_cast<unsigned>(node_id), static_cast<unsigned>(sc.accepted),
                        static_cast<unsigned>(sc.unsigned_refused), static_cast<unsigned>(sc.unknown_sender),
                        static_cast<unsigned>(sc.bad_signature), static_cast<unsigned>(sc.replayed),
                        static_cast<unsigned>(sc.rate_limited), static_cast<unsigned>(sc.verify_us_max));
            bool first = true;
            for (const Node::SafeStateFloor& e : fl) {
                if (e.node_id == 0) continue;
                std::printf("%s{\"src\":%u,\"epoch\":%u,\"counter\":%u}", first ? "" : ",",
                            static_cast<unsigned>(e.node_id), static_cast<unsigned>(e.epoch),
                            static_cast<unsigned>(e.counter));
                first = false;
            }
            std::printf("]}\n");
            if (g_rec != nullptr) {
                // M6: one line per portable actor, as this node sees it.
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                const Reconciler::Counters rc = g_rec->counters();
                const bool settled = g_rec->settled();
                const size_t na = g_rec->actor_count();
                Reconciler::ActorView av[kMaxPortable];
                for (size_t i = 0; i < na; ++i) av[i] = g_rec->view(i);
                const uint32_t fenced_replies = g_node->ns_counters().replies_fenced;
                xSemaphoreGive(g_mutex);
                std::printf("{\"t\":\"rec\",\"node\":%u,\"up_ms\":%u,\"settled\":%d,\"sent\":%u,\"received\":%u,"
                            "\"malformed\":%u,\"views\":%u,\"replies_fenced\":%u,\"actors\":[",
                            static_cast<unsigned>(node_id), static_cast<unsigned>(now_ms_()), settled ? 1 : 0,
                            static_cast<unsigned>(rc.sets_sent), static_cast<unsigned>(rc.sets_received),
                            static_cast<unsigned>(rc.sets_malformed), static_cast<unsigned>(rc.view_changes),
                            static_cast<unsigned>(fenced_replies));
                for (size_t i = 0; i < na; ++i) {
                    std::printf("%s{\"key\":%u,\"running\":%d,\"term\":%u,\"owner\":%u,\"owner_term\":%u,"
                                "\"assigned\":%u,\"starts\":%u,\"fenced\":%u,\"released\":%u}",
                                i == 0 ? "" : ",", static_cast<unsigned>(av[i].key), av[i].running ? 1 : 0,
                                static_cast<unsigned>(av[i].term), static_cast<unsigned>(av[i].owner),
                                static_cast<unsigned>(av[i].owner_term), static_cast<unsigned>(av[i].assigned),
                                static_cast<unsigned>(av[i].activations), static_cast<unsigned>(av[i].fenced),
                                static_cast<unsigned>(av[i].released));
                }
                std::printf("]}\n");
            }
        }
#endif
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


// ---- Section 9.3, M5: node identity and enrolment ----
//
// Every board makes its own Ed25519 key the first time it boots this firmware, from the hardware RNG
// with the radio already running (true random, per esp_random.h), and keeps it in NVS. Enrolment is
// physical: potluck.enrol talks to the board over its own USB console, and the board installs a
// certificate only if the CA it is handed signed it for this node id and this key. What the cell
// does with an enrolled or unenrolled peer is the next step; this one only gives every node a
// provable name.
namespace trust_rt {

Identity g_id;
// What the node runs on: a copy taken once at boot. The console may enrol the board while it runs;
// that is saved and takes effect at the next boot, so the link task never reads a half-written
// identity from another core.
Identity g_id_boot;
uint16_t g_node_id = 0;
StaticTask_t g_console_tcb;
// 4 KB: an Ed25519 verify measured 2.3 KB of stack on this board (M0-LOG session 21), and every
// certificate reply reports the high-water mark, so the margin stays a measurement.
StackType_t g_console_stack[4096];

// The crypto worker. Node hands every Ed25519/X25519 operation to run_heavy(), which queues it here
// and waits: the link task's 4 KB stack overflowed on the first enrolled boot, while this task's 4 KB
// is otherwise idle and sits in section 6's crypto line. One job at a time, one submitter (the node).
struct HeavyJob {
    void (*fn)(void*);
    void* arg;
};
StaticQueue_t g_jobs_buf;
uint8_t g_jobs_storage[sizeof(HeavyJob)];
QueueHandle_t g_jobs = nullptr;
StaticSemaphore_t g_job_done_buf;
SemaphoreHandle_t g_job_done = nullptr;

void run_heavy(void*, void (*fn)(void*), void* arg) {
    // From the worker itself (a console command that sends a SAFE_STATE), queueing would wait on
    // ourselves forever: run it here, on this task's own stack, which is the one it is meant for.
    if (g_jobs == nullptr || xTaskGetCurrentTaskHandle() == reinterpret_cast<TaskHandle_t>(&g_console_tcb)) {
        fn(arg);  // only before start_console(), which app_main calls before any task uses the node
        return;
    }
    HeavyJob j{fn, arg};
    xQueueSend(g_jobs, &j, portMAX_DELAY);
    xSemaphoreTake(g_job_done, portMAX_DELAY);
}

// Section 8.3: the SAFE_STATE high-water table, in NVS, so a power cycle cannot reopen the window.
constexpr const char* kSsNs = "pot_ss";
struct PersistJob {
    const void* table;
    size_t bytes;
};
void persist_job(void* a) {
    const PersistJob& j = *static_cast<PersistJob*>(a);
    nvs_handle_t h;
    if (nvs_open(kSsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "floors", j.table, j.bytes);
    nvs_commit(h);
    nvs_close(h);
}
void persist_floors(void* ctx, const void* table, size_t bytes);  // below, once run_heavy exists
void load_floors() {
    static uint8_t buf[sizeof(Node::SafeStateFloor) * Node::kSafeStateSources];
    nvs_handle_t h;
    if (nvs_open(kSsNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "floors", buf, &len) == ESP_OK) {
        g_node->restore_safe_state_floors(buf, len);
        ESP_LOGI(kTag, "safe_state: high-water table restored from NVS");
    }
    nvs_close(h);
}
void save_recording(const char* key, const uint8_t* data, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kSsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, data, len);
    nvs_commit(h);
    nvs_close(h);
}
size_t load_recording(const char* key, uint8_t* out, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(kSsNs, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const bool ok = nvs_get_blob(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok ? len : 0;
}

void serve_jobs(TickType_t wait) {
    HeavyJob j{};
    while (xQueueReceive(g_jobs, &j, wait) == pdTRUE) {
        j.fn(j.arg);
        const uint32_t free_now = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        if (g_heavy_stack_free_min == 0 || free_now < g_heavy_stack_free_min) g_heavy_stack_free_min = free_now;
        xSemaphoreGive(g_job_done);
        wait = 0;
    }
}

// The console task takes the node mutex for its test commands. The link task may be holding it
// while it waits for this very task to run a crypto job, so waiting here plainly would deadlock the
// two: keep serving jobs while the mutex is busy.
void lock_node() {
    while (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2)) != pdTRUE) serve_jobs(0);
}

void persist_floors(void* ctx, const void* table, size_t bytes) {
    PersistJob j{table, bytes};
    run_heavy(ctx, &persist_job, &j);  // NVS work belongs on the worker's stack, like the crypto
}

// M5 step 6: an enrolled node checks every deploy itself -- a deploy-key certificate under its CA, and
// that key's signature over the image -- before writing anything. The check runs on this worker.
struct ImageJob {
    const uint8_t* img;
    size_t n;
    const uint8_t* tr;
    size_t tn;
    CertError e;
};
void image_job(void* a) {
    ImageJob& j = *static_cast<ImageJob*>(a);
    j.e = image_trailer_check(g_id_boot.ca_pub, j.img, j.n, j.tr, j.tn);
}
DeployStatus verify_image(void*, const uint8_t* img, size_t n, const uint8_t* tr, size_t tn) {
    if (tn == 0) {
        ESP_LOGW(kTag, "deploy: refused, the image is not signed");
        return DeployStatus::Unsigned;
    }
    ImageJob j{img, n, tr, tn, CertError::Ok};
    run_heavy(nullptr, &image_job, &j);
    if (j.e != CertError::Ok) {
        ESP_LOGW(kTag, "deploy: refused, image signature check failed (%s)", cert_error_name(j.e));
        return DeployStatus::BadSignature;
    }
    ESP_LOGI(kTag, "deploy: image signature verified");
    return DeployStatus::Ok;
}

void print_id() {
    char line[200];
    if (format_enrol_id(g_id, g_node_id, line, sizeof(line)) > 0) {
        std::printf("%s\n", line);
    }
}

void print_stack_margin() {
    std::printf("{\"t\":\"console\",\"stack_free_min_b\":%u,\"stack_b\":%u}\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned>(sizeof(g_console_stack)));
}

void boot(uint16_t node_id, bool radio_running) {
    g_node_id = node_id;
    if (!trust_load(g_id)) {
        uint8_t seed[kEdSeedLen];
        trust_random_seed(seed, radio_running);
        identity_from_seed(g_id, seed);
        std::memset(seed, 0, sizeof(seed));
        if (trust_save(g_id)) {
            ESP_LOGI(kTag, "identity: generated this node's key (radio %s)", radio_running ? "up" : "off");
        }
    }
    print_id();
}

// M5 test instruments, on the console only (physical access already owns the board):
//   POT! write <node hex> <int>   this node sends a WRITE of act/setpoint to that node
//   POT! replay [flip]            re-send the last recorded WRITE unchanged (or with one payload bit flipped)
bool handle_test(const char* line, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) return false;
    std::memcpy(buf, line, len);
    buf[len] = 0;
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = 0;
    unsigned node = 0;
    int value = 0;
    if (std::sscanf(buf, "POT! write %x %d", &node, &value) == 2) {
        lock_node();
        const uint16_t id = g_node->request_write(static_cast<uint16_t>(node), path_hash("act/setpoint"),
                                                  Value::of_i32(value));
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"write\",\"to\":%u,\"value\":%d,\"msg_id\":%u}\n", node, value,
                    static_cast<unsigned>(id));
        return true;
    }
    char scan_ssid[33] = {};
    if (std::sscanf(buf, "POT! scan %32s", scan_ssid) == 1) {
        uint8_t ch = 0;
        int8_t rssi = 0;
        const bool seen = espnow_scan_for(scan_ssid, ch, rssi);
        std::printf("{\"t\":\"test\",\"cmd\":\"scan\",\"ssid\":\"%s\",\"seen\":%d,\"channel\":%u,\"rssi\":%d}\n",
                    scan_ssid, seen ? 1 : 0, static_cast<unsigned>(ch), static_cast<int>(rssi));
        return true;
    }
    unsigned relay_on = 0;
    if (std::sscanf(buf, "POT! relay %u", &relay_on) == 1) {
        lock_node();
        g_node->set_relay(relay_on != 0);
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"relay\",\"relay\":%u}\n", relay_on != 0 ? 1u : 0u);
        return true;
    }
    unsigned deaf_tail = 0;
    if (std::sscanf(buf, "POT! deaf %x", &deaf_tail) == 1) {
        // POT! deaf <last 2 MAC bytes, hex>: stop hearing that board; POT! deaf 0 clears the list.
        if (deaf_tail == 0) {
            for (uint16_t& d : g_deaf) d = 0;
        } else {
            for (uint16_t& d : g_deaf) {
                if (d == 0) {
                    d = static_cast<uint16_t>(deaf_tail);
                    break;
                }
            }
        }
        std::printf("{\"t\":\"test\",\"cmd\":\"deaf\",\"list\":[%u,%u,%u,%u]}\n", g_deaf[0], g_deaf[1], g_deaf[2],
                    g_deaf[3]);
        return true;
    }
    unsigned chan = 0;
    char word[8] = {};
    const int nchan = std::sscanf(buf, "POT! channel %u %7s", &chan, word);
    if (nchan >= 1) {
        const bool quiet = (nchan == 2 && std::strcmp(word, "quiet") == 0);
        lock_node();
        bool ok = true;
        if (quiet) {
            g_node->set_channel_now(static_cast<uint8_t>(chan));  // as if a router had moved us
        } else {
            ok = g_node->move_cell(static_cast<uint8_t>(chan), 500);
        }
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"channel\",\"channel\":%u,\"quiet\":%d,\"ok\":%d}\n", chan,
                    quiet ? 1 : 0, ok ? 1 : 0);
        return true;
    }
    unsigned fixed = 0;
    if (std::sscanf(buf, "POT! fixed %u", &fixed) == 1) {
        lock_node();
        g_node->set_channel_fixed(fixed != 0);
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"fixed\",\"fixed\":%u}\n", fixed != 0 ? 1u : 0u);
        return true;
    }
    unsigned period = 0, misses = 0, flag = 0;
    if (std::sscanf(buf, "POT! window %u %u", &period, &misses) == 2) {
        lock_node();
        const bool ok = g_node->set_heartbeat_window(period, static_cast<uint8_t>(misses));
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"window\",\"period_ms\":%u,\"misses\":%u,\"ok\":%d}\n", period,
                    misses, ok ? 1 : 0);
        return true;
    }
    if (std::sscanf(buf, "POT! busy %u", &flag) == 1) {
        lock_node();
        g_node->set_busy(flag != 0);
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"busy\",\"busy\":%u}\n", flag != 0 ? 1u : 0u);
        return true;
    }
    unsigned reason = 0;
    if (std::sscanf(buf, "POT! safe %u", &reason) == 1) {
        lock_node();
        const uint32_t counter = g_node->send_safe_state(static_cast<uint16_t>(reason));
        xSemaphoreGive(g_mutex);
        if (g_rec_ss_len != 0) save_recording("rec_ss", g_rec_ss, g_rec_ss_len);
        std::printf("{\"t\":\"test\",\"cmd\":\"safe\",\"counter\":%u,\"len\":%u,\"recorded\":%d}\n",
                    static_cast<unsigned>(counter), static_cast<unsigned>(g_rec_ss_len), g_rec_ss_len != 0 ? 1 : 0);
        return true;
    }
    if (std::strcmp(buf, "POT! replay-ss") == 0) {
        uint8_t f[sizeof(g_rec_ss)];
        const size_t n = load_recording("rec_ss", f, sizeof(f));
        const int32_t err = (n != 0) ? espnow_send(kBroadcastMacAddr, f, n) : -1;
        uint32_t ep = 0, counter = 0;
        if (n >= kHeaderSize + 12) {
            std::memcpy(&counter, f + kHeaderSize, 4);
            std::memcpy(&ep, f + kHeaderSize + 8, 4);
        }
        std::printf("{\"t\":\"test\",\"cmd\":\"replay-ss\",\"len\":%u,\"epoch\":%u,\"counter\":%u,\"err\":%d}\n",
                    static_cast<unsigned>(n), static_cast<unsigned>(ep), static_cast<unsigned>(counter),
                    static_cast<int>(err));
        return true;
    }
    if (std::strncmp(buf, "POT! replay", 11) == 0) {
        const bool flip = std::strstr(buf, "flip") != nullptr;
        if (g_rec_write_len == 0) {
            std::printf("{\"t\":\"test\",\"cmd\":\"replay\",\"result\":\"nothing_recorded\"}\n");
            return true;
        }
        uint8_t f[sizeof(g_rec_write)];
        std::memcpy(f, g_rec_write, g_rec_write_len);
        if (flip) f[kHeaderSize + 6] ^= 0x01;
        const int32_t err = espnow_send(g_rec_write_mac, f, g_rec_write_len);
        std::printf("{\"t\":\"test\",\"cmd\":\"replay\",\"flip\":%d,\"len\":%u,\"seq\":%u,\"err\":%d}\n", flip ? 1 : 0,
                    static_cast<unsigned>(g_rec_write_len), static_cast<unsigned>(f[8] | (f[9] << 8)),
                    static_cast<int>(err));
        return true;
    }
    return false;
}

void handle(const char* line, size_t len) {
    if (extender::handle_console(line, len)) return;  // M6.1: credentials; prints a result, never them
    if (len > 5 && std::strncmp(line, "POT! ", 5) == 0) {
        // M8.1: the actors' own test instruments (e.g. "POT! die_temp fail 1"), offered generically.
        lock_node();
        const bool mine = g_actors.on_console(line, len);
        xSemaphoreGive(g_mutex);
        if (mine) return;
    }
    if (handle_test(line, len)) return;
    const EnrolRequest r = parse_enrol_line(line, len);
    char out[200];
    switch (r.cmd) {
        case EnrolCmd::None:
            return;
        case EnrolCmd::Id:
            print_id();
            return;
        case EnrolCmd::Cert: {
            Identity next = g_id;
            const CertError e = identity_install_cert(next, g_node_id, r.ca_pub, r.cert, kNodeCertLen);
            const char* result = cert_error_name(e);
            if (e == CertError::Ok) {
                if (trust_save(next)) {
                    g_id = next;
                    ESP_LOGI(kTag, "identity: enrolled; takes effect at the next boot");
                } else {
                    result = "save_failed";
                }
            } else {
                ESP_LOGW(kTag, "identity: certificate refused (%s)", result);
            }
            if (format_enrol_result(result, out, sizeof(out)) > 0) std::printf("%s\n", out);
            print_stack_margin();  // only the console task reaches here
            return;
        }
        case EnrolCmd::Bad:
            if (format_enrol_result("bad_command", out, sizeof(out)) > 0) std::printf("%s\n", out);
            return;
    }
}

// Reads the console UART a byte at a time and hands each line to handle(). Lines that are not
// "POT! " commands are ignored, so typing into a monitor does nothing.
void console_task(void*) {
    const int uart = CONFIG_ESP_CONSOLE_UART_NUM;
    if (uart_driver_install(static_cast<uart_port_t>(uart), 1024, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGW(kTag, "console commands unavailable: UART%d driver did not install", uart);
        for (;;) serve_jobs(portMAX_DELAY);  // still the crypto worker: the node depends on it
    }
    uart_vfs_dev_use_driver(uart);  // printf now goes through the same driver, so the two never race
    static char line[400];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c = 0;
        serve_jobs(0);
        // 10 ms, so a queued crypto job waits at most that long behind an idle console.
        if (uart_read_bytes(static_cast<uart_port_t>(uart), &c, 1, pdMS_TO_TICKS(10)) != 1) continue;
        if (c == '\n') {
            if (!overflow) handle(line, n);
            std::memset(line, 0, n);  // M6.1: a line may have carried a Wi-Fi password; keep no copy
            n = 0;
            overflow = false;
        } else if (n < sizeof(line)) {
            line[n++] = static_cast<char>(c);
        } else {
            overflow = true;  // longer than any command: drop the whole line, never a prefix of it
        }
    }
}

void start_console(BaseType_t core) {
    g_jobs = xQueueCreateStatic(1, sizeof(HeavyJob), g_jobs_storage, &g_jobs_buf);
    g_job_done = xSemaphoreCreateBinaryStatic(&g_job_done_buf);
    xTaskCreateStaticPinnedToCore(console_task, "pot_console", sizeof(g_console_stack) / sizeof(StackType_t),
                                  nullptr, 2, g_console_stack, &g_console_tcb, core);
}

}  // namespace trust_rt

}  // namespace

namespace board {
void set_led_healthy(const LedConfig& c) { status_led::set_healthy(c); }
}  // namespace board

}  // namespace pot

#if CONFIG_POT_SELFTEST
extern "C" int pot_selftest_run(void);
#endif
#if CONFIG_POT_CRYPTO_BENCH || CONFIG_POT_MEM_BENCH
#include "crypto_bench.hpp"
#endif

extern "C" void app_main(void) {
    using namespace pot;

#if CONFIG_POT_CRYPTO_BENCH
    // M5's measurement, alone on the machine for the same reason as the self-test below.
    const int bench_failed = pot_crypto_bench_run();
    ESP_LOGI(kTag, "crypto bench finished with %d failure(s)", bench_failed);
    return;
#endif

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
    // M6.1: boot on the channel the cell was last on, not the build's default.
    nvs_init_once();
    const uint8_t saved_channel = chan_store::load();
    if (saved_channel >= 1 && saved_channel <= 11) {
        ecfg.channel = saved_channel;
        ESP_LOGI(kTag, "channel %u from the last boot (build default %u)", static_cast<unsigned>(saved_channel),
                 static_cast<unsigned>(kChannel));
    }
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
#if CONFIG_POT_EXTENDER
    // M6.1: the hotspot owns Wi-Fi when it has credentials, and ESP-NOW only attaches to it. Its
    // credentials live in NVS, so NVS comes up first.
    nvs_init_once();
    ecfg.attach = extender::start(saved_channel);
#endif
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
    cfg.channel = ecfg.channel;
    if (extender::active()) {
        // M6.1: the router owns this radio's channel. Declare what it is on now (0 until the station
        // first associates), never hop, never follow an announcement -- CR-1's station case.
        const uint8_t now_ch = espnow_current_channel();
        if (now_ch >= cfg.channel_lo && now_ch <= cfg.channel_hi) cfg.channel = now_ch;
        cfg.channel_fixed = true;
#if CONFIG_POT_EXTENDER
        // At boot the station is usually still hunting; the link loop tightens this once it associates.
        if (!extender::associated()) cfg.hb_miss_limit = CONFIG_POT_EXTENDER_HUNT_MISSES;
#endif
    }
#if CONFIG_POT_CAN
    cfg.admit_on_beacon = true;  // a CAN bus carries no HELLO (§5.3.1)
#endif
#if CONFIG_POT_SERIAL_LINK
    // M5: the host reaches this board over a cable plugged into it, so its HELLO is admitted
    // unsigned. Deploys over that cable still need a signed package.
    cfg.has_trusted_mac = true;
    std::memcpy(cfg.trusted_mac, kHostMac, kMacLen);
#endif
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
#if CONFIG_POT_RSSI_TEE
    hal.on_rx_sample = &rssi_tee;
#endif
    hal.run_heavy = &trust_rt::run_heavy;
#if !CONFIG_POT_RADIO_DISABLE && !CONFIG_POT_CAN
    hal.set_channel = [](void*, uint8_t ch) { espnow_set_channel(ch); };
    if (extender::active()) hal.set_channel = nullptr;  // M6.1: nothing here may retune the router's radio
#endif
    hal.persist_safe_state_floors = &trust_rt::persist_floors;

    // Placement new into static storage: the node owns the peer table and the histograms, which §6
    // budgets statically, and there is no heap allocation anywhere on this path.
    g_node = new (g_node_storage) Node(cfg, hal);

    // §7.2: declare what this node owns. Six built-ins every board has, so a fresh fleet has
    // something real to read across a link on the day it is switched on.
    const size_t declared = declare_sys_resources(g_node->ns(), cfg.node_id);
    {
        // M5's stand-in actuator: a writable setpoint. Nothing physical moves; every applied value is
        // reported on the {"t":"act"} line, which is what the replay demonstration watches.
        NsDecl d;
        d.path_hash = path_hash("act/setpoint");
        d.owner_node = cfg.node_id;
        d.type = ValueType::I32;
        d.access = Access::ReadWrite;
        d.latency_class = kClassL3;
        g_node->ns().declare(d);
    }
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

    // §7.4: decide which slot this boot runs, persist the trial count, apply the actors -- all
    // before any task starts, so the first heartbeat already comes from the deployed behaviour.
    deploy_rt::boot(cfg.node_id);
#if CONFIG_POT_RECONCILER
    if (deploy_rt::g_portable_n > 0) {
        g_rec = new (g_rec_storage) Reconciler(*g_node);
        if (!g_rec->load(deploy_rt::g_portable, deploy_rt::g_portable_n)) {
            ESP_LOGE(kTag, "reconciler: could not declare the portable actors' resources");
        }
        g_node->set_call_handler(
            [](void*, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
                return g_rec->on_cast(from, path, args, len);
            },
            nullptr);
        ESP_LOGI(kTag, "reconciler: %u portable actor(s)", static_cast<unsigned>(g_rec->actor_count()));
    }
#endif
    // M8.1: start this node's pinned actors, now that the node they publish through exists.
    g_actor_env.node = g_node;
    {
        const char* failed = nullptr;
        g_actors.start(g_actor_env, now_ms_(), &failed);
        if (failed != nullptr) ESP_LOGE(kTag, "actors: %s could not start; dropped", failed);
        for (size_t i = 0; i < g_actors.running(); ++i) ESP_LOGI(kTag, "actors: %s running", g_actors.name(i));
    }
    if (g_actors.running() > 0) {
        g_node->set_call_result(
            [](void*, uint16_t from, uint16_t msg_id, uint32_t path, Node::CallOutcome o, const Value& v) {
                g_actors.on_call_result(from, msg_id, path, o, v);
            },
            nullptr);
    }
#if CONFIG_POT_MEM_BENCH
    pot_mem_bench_run();  // with the radio already up, so internal RAM is what the node really has
#endif
    trust_rt::boot(cfg.node_id, espnow_up());
    trust_rt::g_id_boot = trust_rt::g_id;
    trust_rt::load_floors();
    if (trust_rt::g_id_boot.enrolled) {
        deploy_rt::g_rx.set_verifier(&trust_rt::verify_image, nullptr);
    }
#if !CONFIG_POT_CAN
#if CONFIG_POT_REQUIRE_AUTH
    g_node->set_trust(&trust_rt::g_id_boot, true);
#else
    g_node->set_trust(&trust_rt::g_id_boot, false);
#endif
    ESP_LOGI(kTag, "auth: %s, %s", trust_rt::g_id_boot.enrolled ? "enrolled" : "NOT enrolled",
             g_node->trust_required() ? "required" : "not required");
#endif
#if CONFIG_POT_CAN
    {
        CanPortConfig cc;
        cc.tx_gpio = CONFIG_POT_CAN_TX_GPIO;
        cc.rx_gpio = CONFIG_POT_CAN_RX_GPIO;
        cc.bitrate = CONFIG_POT_CAN_BITRATE;
#ifdef CONFIG_POT_CAN_LOOPBACK
        cc.loopback = true;  // an unset Kconfig bool is undefined in C, not 0
#endif
        cc.node_id = cfg.node_id;
        if (!can_start(cc)) {
            ESP_LOGE(kTag, "CAN transport failed to start");
        }
        g_node->set_safe_state_handler(&m4::on_safe_state, nullptr);
    }
#endif
    if (extender::active()) {
        // M6.1 / CR-6 item 4: the garden node is busy by nature (it forwards phones' traffic) and the
        // cell's relay by construction (ADR-009): it is the member out where the others cannot reach.
        g_node->set_busy(true);
        g_node->set_relay(true);
        ESP_LOGI(kTag, "extender: declared BUSY and RELAY; channel %u from the router", static_cast<unsigned>(g_node->channel()));
    }
    g_node->set_deploy_server(&deploy_rt::on_deploy, nullptr);
    g_node->set_deploy_result(&deploy_rt::on_result, nullptr);

    // The broadcast address needs a peer entry before anything can be broadcast, and it consumes
    // one of §3's twenty slots — leaving nineteen for unicast.
    if (espnow_up()) {
        if (!espnow_add_peer(kBroadcastMacAddr, 0)) {
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

    trust_rt::start_console(kStatsCore);  // first: the crypto worker must exist before the node runs
    g_link_handle = xTaskCreateStaticPinnedToCore(link_task, "pot_link",
                                  sizeof(g_link_stack) / sizeof(StackType_t), nullptr, 6,
                                  g_link_stack, &g_link_tcb, 0);
    xTaskCreateStaticPinnedToCore(stats_task, "pot_stats",
                                  sizeof(g_stats_stack) / sizeof(StackType_t), nullptr, 3,
                                  g_stats_stack, &g_stats_tcb, kStatsCore);
}
