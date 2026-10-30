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
#include "pot/wifi.hpp"
#include "uplink.hpp"
#include "pot/boot_epoch.hpp"
#if CONFIG_POT_CAN
#include "pot/can_port.hpp"
#include "pot/frame.hpp"
#endif
#include "pot/deploy.hpp"
#include "pot/deploy_esp.hpp"
#include "pot/dram_probe.hpp"
#include "pot/fw.hpp"
#include "pot/fw_esp.hpp"
#include "pot/mc.hpp"  // every image: M22's telemetry names the job and the lender (a build without WASM failed, 2026-10-29)
#if CONFIG_POT_WASM
#include "pot/guest.hpp"
#include "pot/wasm_modules.h"
#include "pot/wasm_sandbox.hpp"
#endif
#include "pot/espnow_port.hpp"
#include "pot/node.hpp"
#include "pot/lease.hpp"
#include "pot/package_agent.hpp"
#include "pot/telemetry.hpp"
#include "pot/node_status.hpp"
#include "pot/opcodes.hpp"
#include "pot/reconcile.hpp"
#include "pot/actor.hpp"
#include "pot/serial_port.hpp"
#include "pot/stats_json.hpp"
#include "pot/led_slots.hpp"
#include "pot/split_probe.hpp"
#include "pot/sys_resources.hpp"
#include "pot/trust.hpp"
#include "pot/trust_store.hpp"
#include "pot/present.hpp"
#include "pot/pocket.hpp"
#include "pot/pocket_auth.hpp"
#include "nvs.h"
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
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
// M13.1, defined with the board services below: the pocket page's snapshot, and the cell id it shows.
const pocket::Snapshot& pocket_snapshot(uint32_t now);
const char* pocket_apply_write(const pocket::PendingWrite& w, uint32_t now);
const char* cell_id(void*);

#if CONFIG_POT_RECONCILER
// M6: section 7.7's reconciler, created only when the running image has portable actors. Touched by
// the link task and, for its stats line, the stats task -- both under g_mutex, never from an ISR.
EXT_RAM_BSS_ATTR alignas(Reconciler) uint8_t g_rec_storage[sizeof(Reconciler)];
#endif
Reconciler* g_rec = nullptr;

// M8.1: the image's actors pinned to this node, built from the registration table (actor_table.cpp)
// and driven through the actor API -- tick, CALL results, stats -- with no type known here. PSRAM when
// present: only tasks touch it.
EXT_RAM_BSS_ATTR ActorRuntime g_actors(nullptr, 0);  // its table is merged at boot (app::actor_table)
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
StackType_t g_link_stack[6656 / sizeof(StackType_t)];  // M12: +1.5 KB, a live package change runs here
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
void rx_sample_hook(void* ctx, const RxSample& s);  // below: the RSSI tee, then the actors (PS-2)
// M8.2: true only while the link task hands the node frames that came off the ESP-NOW radio. A sample's
// RSSI is a radio measurement; a frame from the host's cable or a CAN bus carries none, and on board A
// the cabled host became a third "link" in rf_link (found by passive-sensor in the smoke capture).
bool g_rx_from_radio = false;

#if CONFIG_POT_RSSI_TEE
// M8.2 (PS-0): the radio metadata of the frame on_rx is handling now. The drain loop sets it around
// each on_rx call; the node's sample hook, called synchronously inside, adds what only the node knows.
const RxSlot* g_rx_now = nullptr;
// The line is handed to a message buffer and printed by a low-priority task, never by the link task:
// ESP-IDF's console blocks until a line is out (~10 ms for these 111 bytes at 115200), and printing
// here made board A's heartbeat round trips 3-4x slower (M0-LOG session 32, the PS-0 dry run). A
// line the buffer has no room for is dropped and counted (tee_dropped), never waited for.
constexpr size_t kTeeBufBytes = 16384;
EXT_RAM_BSS_ATTR uint8_t g_tee_storage[kTeeBufBytes];
StaticMessageBuffer_t g_tee_mb_ctl;
MessageBufferHandle_t g_tee_mb = nullptr;
std::atomic<uint32_t> g_tee_dropped{0};
// In PSRAM (2026-10-22: in internal RAM it put the core 1,649 B over its 64 KB cap). Allowed for a task that never
// touches flash -- this one only prints (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY, on in the PSRAM build).
EXT_RAM_BSS_ATTR StackType_t g_tee_stack[3072 / sizeof(StackType_t)];
StaticTask_t g_tee_tcb;
// Held by the statistics task for its whole period's output, and by the tee for each line. Several
// statistics lines (peers, ss, rec) are printed in pieces, and a tee line written between two pieces
// broke them: on board A the night run of M0-LOG session 32 lost every `peers` line and most `ss` and
// `rec` lines to it. Not stdout's own lock: the statistics task takes the node mutex while printing,
// and the link task prints events under that mutex. The tee takes no other lock, so this cannot
// deadlock; while it waits, the message buffer holds ~3.7 s of lines against a ~0.3 s period block.
StaticSemaphore_t g_print_gate_ctl;
SemaphoreHandle_t g_print_gate = nullptr;
void tee_task(void*) {
    static char out[160];
    for (;;) {
        const size_t n = xMessageBufferReceive(g_tee_mb, out, sizeof(out), portMAX_DELAY);
        if (n == 0) continue;
        xSemaphoreTake(g_print_gate, portMAX_DELAY);
        std::fwrite(out, 1, n, stdout);  // one whole line per write
        xSemaphoreGive(g_print_gate);
    }
}
void tee_start() {
    g_print_gate = xSemaphoreCreateMutexStatic(&g_print_gate_ctl);
    g_tee_mb = xMessageBufferCreateStatic(sizeof(g_tee_storage), g_tee_storage, &g_tee_mb_ctl);
    xTaskCreateStaticPinnedToCore(tee_task, "pot_tee", sizeof(g_tee_stack) / sizeof(StackType_t), nullptr, 1,
                                  g_tee_stack, &g_tee_tcb, tskNO_AFFINITY);
}
void rssi_tee(void*, const RxSample& s) {
    static const char* const kKind[] = {"?", "beacon", "unicast", "bcast"};
    char line[160];
    const int n = std::snprintf(line, sizeof(line),
                "{\"t\":\"rssi\",\"us\":%u,\"peer\":%u,\"rssi\":%d,\"nf\":%d,\"sig\":%u,\"rate\":%u,"
                "\"kind\":\"%s\",\"seq\":%u,\"rel\":%d,\"ch\":%u,\"away\":%d}\n",
                static_cast<unsigned>(s.recv_us), static_cast<unsigned>(s.node_id), static_cast<int>(s.rssi),
                g_rx_now != nullptr ? static_cast<int>(g_rx_now->noise_floor) : 0,
                g_rx_now != nullptr ? static_cast<unsigned>(g_rx_now->sig_mode) : 0u,
                g_rx_now != nullptr ? static_cast<unsigned>(g_rx_now->rate) : 0u,
                kKind[s.kind < 4 ? s.kind : 0], static_cast<unsigned>(s.hb_seq), s.relayed ? 1 : 0,
                static_cast<unsigned>(s.channel), s.off_home ? 1 : 0);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || g_tee_mb == nullptr ||
        xMessageBufferSend(g_tee_mb, line, static_cast<size_t>(n), 0) != static_cast<size_t>(n)) {
        g_tee_dropped.fetch_add(1);
    }
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

#if CONFIG_POT_SERIAL_LINK
// M10: when the host was last heard on the cable. A host that is a full member -- pot_hostnode, which
// keeps membership from beacons like any node -- needs the cell's broadcasts: this board's own beacons,
// and, when it relays (ADR-009), the ones it re-broadcasts for the others. So while a host has been
// heard in the last kHostQuietMs, every broadcast also goes down the cable. ~30 small frames a second at
// 921,600 baud; an M2/M8 bridge that ignores them is unaffected.
std::atomic<uint32_t> g_host_heard_ms{0};
constexpr uint32_t kHostQuietMs = 2000;
#endif

// M13 (found on the bench, M0-LOG session 41): a radio that refuses every send while it still receives. C ran so
// for minutes after 5.6 h up -- every esp_now_send NO_MEM, 201 KB of heap free -- and kept its portable actors, which
// the others, hearing nothing, ran as well: a double run nobody can fence, since fencing needs to be heard. So
// the radio's sends are watched, and a board whose every send has failed for CONFIG_POT_TX_DEAD_RESTART_S stops:
// a restart is a death the cell knows how to handle (M6), a mute member is not.
std::atomic<uint32_t> g_radio_ok_ms{0};
std::atomic<uint32_t> g_radio_fail_since_ms{0};
// What the radio said (M0-LOG session 42: the stall's cause is open, so its record carries the evidence): the
// first refusal's error code and the internal heap then, the latest code, and how many sends were refused.
std::atomic<int32_t> g_radio_first_err{0};
std::atomic<int32_t> g_radio_last_err{0};
std::atomic<uint32_t> g_radio_first_err_heap{0};
std::atomic<uint32_t> g_radio_refused{0};

int32_t radio_send(const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    const int32_t err = espnow_send(mac, data, len);
    // A radio that never came up (a failed bring-up, a radio-less build) is not a radio refusing: such a node is meant
    // to keep running, diagnosable over its serial link (found by review, M0-LOG session 41).
    if (!espnow_up()) return err;
    const uint32_t now = now_ms_();
    if (err == 0) {
        g_radio_ok_ms.store(now);
        g_radio_fail_since_ms.store(0);
        g_radio_refused.store(0);
    } else {
        g_radio_last_err.store(err);
        g_radio_refused.fetch_add(1);
        if (g_radio_fail_since_ms.load() == 0) {
            g_radio_fail_since_ms.store(now == 0 ? 1 : now);
            g_radio_first_err.store(err);
            g_radio_first_err_heap.store(static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        }
    }
    return err;
}

#if CONFIG_POT_SERIAL_LINK
// M19: the cable's family of addresses -- the PC's node (kHostMac) and the stub nodes the PC hosts for a tree's tests,
// 02:00:00:00:<id>, each a peer of its own. Ids in kStubFirst..kStubLast get their own; every other frame from the
// cable is the PC's node, as before.
constexpr uint16_t kStubFirst = 0xF000;
constexpr uint16_t kStubLast = 0xFDFF;
inline bool cable_mac(const uint8_t mac[kMacLen]) { return std::memcmp(mac, kHostMac, 4) == 0; }
#endif
// 2026-10-30: nodes the hub bridges from the house network (a tablet) come down the same cable, each under its own
// 02:00:00:01:<id> -- outside the trusted 02:00:00:00 family, so such a node signs its HELLO and tags its frames as a
// radio peer does, and the hub that forwards them is trusted with nothing. Ids kBridgedFirst..kBridgedLast (hosts').
constexpr uint8_t kBridgedPrefix[4] = {0x02, 0x00, 0x00, 0x01};
constexpr uint16_t kBridgedFirst = 0x00F0;
constexpr uint16_t kBridgedLast = 0x00FD;
inline bool bridged_mac(const uint8_t mac[kMacLen]) { return std::memcmp(mac, kBridgedPrefix, 4) == 0; }
// Not a radio peer: the PC's node at the cable's end, or a node the hub bridges -- neither is on any channel.
inline bool off_radio(const uint8_t mac[kMacLen]) {
    return std::memcmp(mac, kHostMac, kMacLen) == 0 || bridged_mac(mac);
}

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
    if (cable_mac(mac) || bridged_mac(mac)) {
        return serial_port_send(data, len);  // the PC's node, a stub it hosts (M19), a node the hub bridges
    }
    if (std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0) {
        const uint32_t heard = g_host_heard_ms.load();
        if (heard != 0 && now_ms_() - heard < kHostQuietMs) serial_port_send(data, len);
    }
#endif
    return radio_send(mac, data, len);
}


// M19: a reclaimed peer's radio entry. The bus and the cable keep no list.
bool hal_del_peer(void*, const uint8_t mac[kMacLen]) {
#if CONFIG_POT_SERIAL_LINK
    if (cable_mac(mac) || bridged_mac(mac)) return true;
#endif
#if CONFIG_POT_CAN
    uint16_t can_node = 0;
    if (can_mac_node(mac, can_node)) return true;
#endif
    return espnow_del_peer(mac);
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
    if (cable_mac(mac) || bridged_mac(mac)) {
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
// M12: the relay setting (ADR-009) outlives a reboot: `POT! relay 1` once, and the board keeps relaying.
uint8_t relay_load() {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "relay", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}
void relay_save(bool on) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "relay", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}
}  // namespace chan_store

// M12: portable actors' checkpoints in NVS, so a whole-cell power cut does not lose them. The reconciler
// asks at most once per interval per actor, and only for a checkpoint that changed; the write itself is
// queued here and done by the stats task, because NVS can take milliseconds and the reconciler runs on
// the link task. Blob per actor, key "k<out_hash>": term u32, seq u32, then the checkpoint bytes.
namespace ck_persist {
constexpr const char* kNs = "pot_ck";
struct Pending {
    bool used = false;
    uint32_t key = 0, term = 0, seq = 0;
    uint8_t len = 0;
    uint8_t data[kMaxCheckpoint];
};
EXT_RAM_BSS_ATTR Pending g_q[kMaxPortable];  // under g_mutex
bool g_retain_due = false;
EXT_RAM_BSS_ATTR uint32_t g_retain[kMaxPortable];
size_t g_retain_n = 0;
uint32_t g_writes = 0;       // NVS writes done this boot (the wear figure)
uint32_t g_write_errors = 0;
int g_last_error = 0;        // the last esp_err_t a write or commit returned (0 = none)
uint32_t g_deferred = 0;     // writes held back by the board's wear budget (each stays queued, the newest kept)

// The board's wear budget, in NVS entries a minute. A checkpoint costs 2 + ceil((8 + len) / 32) entries (a
// blob's header, its data, its index; ledger rows on the NVS layout): 3 for a small one, 7 for a full 128 B.
// The interval above holds one actor to one write a minute, but eight portable actors on a board each writing
// a full changing checkpoint wear its six NVS pages to the flash's rated 100,000 cycles in ~2.2-2.6 years
// (WROOM-1 datasheet v1.8, Table 6-8; computed 2026-10-15). Seven entries a minute -- one full checkpoint --
// keeps a board at ~18-20 years whatever its actors do; the actors share it in turn, so with many busy ones
// each checkpoint is older when the power goes, never lost.
constexpr uint32_t kWearEntriesPerMin = 7;
constexpr uint32_t kWearBurst = 14;
uint32_t g_wear_tokens = kWearBurst;
uint32_t g_wear_refill_ms = 0;
size_t g_wear_cursor = 0;
uint32_t entries_of(size_t len) { return 2u + static_cast<uint32_t>((8u + len + 31u) / 32u); }

void key_name(char* out, uint32_t key) { std::snprintf(out, 12, "k%08x", static_cast<unsigned>(key)); }

bool write(void*, uint32_t key, uint32_t term, uint32_t seq, const uint8_t* data, size_t len) {
    if (len > kMaxCheckpoint) return false;
    Pending* free_slot = nullptr;
    for (Pending& p : g_q) {
        if (p.used && p.key == key) { free_slot = &p; break; }
        if (!p.used && free_slot == nullptr) free_slot = &p;
    }
    if (free_slot == nullptr) return false;
    free_slot->used = true;
    free_slot->key = key;
    free_slot->term = term;
    free_slot->seq = seq;
    free_slot->len = static_cast<uint8_t>(len);
    std::memcpy(free_slot->data, data, len);
    return true;
}

// Read back at boot only (the reconciler's first load, on app_main's stack): an actor a live change adds
// is new to the package, and NVS on the link task's stack is not worth the bytes.
bool g_boot_reads = true;

bool read(void*, uint32_t key, uint32_t* term, uint32_t* seq, uint8_t* out, size_t cap, size_t* len) {
    if (!g_boot_reads) return false;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
    char name[12];
    key_name(name, key);
    uint8_t buf[8 + kMaxCheckpoint];
    size_t n = sizeof(buf);
    const bool ok = nvs_get_blob(h, name, buf, &n) == ESP_OK && n >= 8 && n - 8 <= cap;
    nvs_close(h);
    if (!ok) return false;
    std::memcpy(term, buf, 4);
    std::memcpy(seq, buf + 4, 4);
    std::memcpy(out, buf + 8, n - 8);
    *len = n - 8;
    std::printf("{\"t\":\"ck_restored\",\"key\":%u,\"term\":%u,\"seq\":%u,\"len\":%u}\n", static_cast<unsigned>(key),
                static_cast<unsigned>(*term), static_cast<unsigned>(*seq), static_cast<unsigned>(*len));
    return true;
}

void retain(void*, const uint32_t* keys, size_t n) {
    g_retain_n = std::min(n, kMaxPortable);
    std::memcpy(g_retain, keys, sizeof(uint32_t) * g_retain_n);
    g_retain_due = true;
    for (Pending& p : g_q) {  // a queued write for an actor that is gone is dropped with it
        bool kept = false;
        for (size_t i = 0; i < g_retain_n && !kept; ++i) kept = g_retain[i] == p.key;
        if (!kept) p.used = false;
    }
}

const CheckpointPersist g_hook{nullptr, &write, &read, &retain, 60000};

// From the stats task: take the queue under the mutex, then write without it.
void flush() {
    EXT_RAM_BSS_ATTR static Pending s_out[kMaxPortable];  // M19: 16 actors -- PSRAM, never internal RAM
    EXT_RAM_BSS_ATTR static uint32_t s_keep[kMaxPortable];
    size_t n_out = 0, n_keep = 0;
    bool retain_due = false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const uint32_t now = now_ms_();
    if (g_wear_refill_ms == 0) g_wear_refill_ms = now;
    const uint32_t per_entry_ms = 60000u / kWearEntriesPerMin;
    while (now - g_wear_refill_ms >= per_entry_ms) {
        g_wear_refill_ms += per_entry_ms;
        if (g_wear_tokens < kWearBurst) ++g_wear_tokens;
    }
    for (size_t k = 0; k < kMaxPortable; ++k) {  // in turn from where the last flush stopped, so none starves
        Pending& p = g_q[(g_wear_cursor + k) % kMaxPortable];
        if (!p.used) continue;
        const uint32_t cost = entries_of(p.len);
        if (cost > g_wear_tokens) {
            ++g_deferred;
            continue;
        }
        g_wear_tokens -= cost;
        s_out[n_out++] = p;
        p.used = false;
    }
    g_wear_cursor = (g_wear_cursor + 1) % kMaxPortable;
    if (g_retain_due) {
        retain_due = true;
        g_retain_due = false;
        n_keep = g_retain_n;
        std::memcpy(s_keep, g_retain, sizeof(uint32_t) * n_keep);
    }
    xSemaphoreGive(g_mutex);
    if (n_out == 0 && !retain_due) return;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
        ++g_write_errors;
        return;
    }
    if (retain_due) {
        nvs_iterator_t it = nullptr;
        EXT_RAM_BSS_ATTR static char gone[kMaxPortable][16];  // the stats task's only
        size_t n_gone = 0;
        for (esp_err_t e = nvs_entry_find("nvs", kNs, NVS_TYPE_BLOB, &it); e == ESP_OK && it != nullptr;
             e = nvs_entry_next(&it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            bool keep = false;
            for (size_t i = 0; i < n_keep && !keep; ++i) {
                char name[12];
                key_name(name, s_keep[i]);
                keep = std::strcmp(name, info.key) == 0;
            }
            if (!keep && n_gone < kMaxPortable) {
                std::strncpy(gone[n_gone], info.key, sizeof(gone[0]) - 1);
                gone[n_gone][sizeof(gone[0]) - 1] = 0;
                ++n_gone;
            }
        }
        nvs_release_iterator(it);
        for (size_t i = 0; i < n_gone; ++i) nvs_erase_key(h, gone[i]);
    }
    for (size_t i = 0; i < n_out; ++i) {
        uint8_t buf[8 + kMaxCheckpoint];
        std::memcpy(buf, &s_out[i].term, 4);
        std::memcpy(buf + 4, &s_out[i].seq, 4);
        std::memcpy(buf + 8, s_out[i].data, s_out[i].len);
        char name[12];
        key_name(name, s_out[i].key);
        const esp_err_t e = nvs_set_blob(h, name, buf, 8u + s_out[i].len);
        if (e == ESP_OK) {
            ++g_writes;
        } else {
            ++g_write_errors;
            g_last_error = e;
        }
    }
    const esp_err_t ce = nvs_commit(h);
    if (ce != ESP_OK) {
        ++g_write_errors;
        g_last_error = ce;
    }
    nvs_close(h);
}
}  // namespace ck_persist

void deploy_package_soon();  // M12, in deploy_rt below

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
            deploy_package_soon();  // M12: it may have missed a package
            break;
        case EventKind::PeerRebooted:
            ESP_LOGW(kTag, "peer 0x%04x rebooted, epoch now %u", e.node_id,
                     static_cast<unsigned>(e.detail_a));
            deploy_package_soon();
            break;
        case EventKind::PeerDiscovered:
            ESP_LOGI(kTag, "discovered peer 0x%04x", e.node_id);
            deploy_package_soon();
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
// The owner's scheme (2026-10-14; the sweep's blue/green/yellow/red by loss is gone):
//   purple  updating: firmware or a package arriving, or running on trial
//   yellow  searching: no peer alive, or hunting for the cell's channel
//   red     a peer dead within CONFIG_POT_STATUS_LED_DEAD_RED_S, a peer losing over 50 %, or this
//           board's own sends failing
//   green   on the Potluck network (an app's blue or cyan may show in its place)
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

// The owner's scheme (2026-10-14): purple updating, yellow searching, red disconnected / dead /
// stalled, green on the Potluck network. Healthy is always green: a deployed Led actor (M3) used to
// recolour it, and A's blinking purple then read as "updating", so its colour is kept but no longer
// shown. Only an application's state (below) may replace green, and purple, yellow and red always win.
LedConfig g_healthy{0, 255, 0, 0, 1000};
// M8.2 (PS-5): applications' states, shown in place of the healthy colour only (see BoardServices); one slot per
// app since 2026-10-19 (pot/led_slots.hpp: led_app is slot 0, led_claim names one).
LedSlots g_apps;
const char* g_last_shown = "yellow";  // what render() drew last, for the stats line
bool g_last_app = false;
void set_healthy(const LedConfig& c) { g_healthy = c; }

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

enum class Colour : uint8_t { Yellow, Green, Red, Purple };

// What the board itself is doing, gathered by the link task for assess().
struct Signals {
    bool updating;  // receiving firmware or a package, or running one on trial
    bool scanning;  // hunting for the cell's channel
    bool stalled;   // this board's own radio sends failing
};

// Per peer slot: the (broadcast received, heartbeats lost) counters one and two seconds ago.
struct Snap {
    uint32_t bcast;
    uint32_t lost;
};
Snap g_hist[kMaxPeers][2];

// How long a dead peer keeps the LED red, from the last frame heard from it. A peer that left for
// good -- the PC's placement node when the PC is switched off -- otherwise kept every board red for
// as long as they ran (2026-10-14: B and C red for hours after the M14 host node stopped). The hub,
// the dashboard and Home Assistant keep showing the dead peer; 0 keeps the LED red for ever.
constexpr uint32_t kDeadRedMs = static_cast<uint32_t>(CONFIG_POT_STATUS_LED_DEAD_RED_S) * 1000u;

// Called with g_mutex held: reads the peer table, touches nothing in it.
Colour assess(const Node& n, uint32_t now_ms, const Signals& sig) {
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
            if (kDeadRedMs != 0 && now_ms - p.last_rx_ms > kDeadRedMs) continue;  // gone: not this LED's news
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
    if (sig.updating) return Colour::Purple;
    if (sig.stalled) return Colour::Red;
    if (!any || sig.scanning) return Colour::Yellow;
    if (dead || worst_ppm > 500000) return Colour::Red;
    return Colour::Green;  // a link losing 10-50 % stays green here; the dashboard shows the loss
}

uint8_t scaled(uint8_t v) {
    if (v == 0) return 0;
    const uint32_t s = static_cast<uint32_t>(v) * kBrightness / 255u;
    return static_cast<uint8_t>(s == 0 ? 1 : s);
}

// M20: "light up this board" -- the setup wizard's rooms step and the dashboard's identify. While it runs, the LED
// blinks blue fast whatever the status is: the owner asked for it, and the board must be findable even while searching.
uint32_t g_identify_until_ms = 0;

void render(Colour c, uint32_t now_ms) {
    if (g_identify_until_ms != 0 && static_cast<int32_t>(g_identify_until_ms - now_ms) > 0) {
        if ((now_ms % 400u) < 200u) {
            show(0, 0, kBrightness);
        } else {
            show(0, 0, 0);
        }
        g_last_shown = "identify";
        g_last_app = false;
        return;
    }
    g_identify_until_ms = 0;
    switch (c) {
        case Colour::Purple:
            show(kBrightness, 0, kBrightness);
            g_last_shown = "purple";
            g_last_app = false;
            break;
        case Colour::Green: {
            g_last_shown = "green";
            const LedSlots::State* app = g_apps.current(now_ms);
            g_last_app = app != nullptr;
            if (g_last_app) {
                if (app->period_ms != 0 && (now_ms % app->period_ms) >= app->period_ms / 2u) {
                    show(0, 0, 0);
                } else {
                    show(scaled(app->r), scaled(app->g), scaled(app->b));
                }
            } else {
                show(0, kBrightness, 0);
            }
            break;
        }
        case Colour::Yellow: show(kBrightness, kBrightness, 0); g_last_shown = "yellow"; g_last_app = false; break;
        case Colour::Red: show(kBrightness, 0, 0); g_last_shown = "red"; g_last_app = false; break;
    }
}

}  // namespace status_led


// ---------------------------------------------------------------------------------------------
// Deploy and detach (§7.4, M3). The image is a list of built-in actors and their configuration
// (ADR-003 Tier 0); pot_deploy holds the format, the A/B state machine and the receiver.
// ---------------------------------------------------------------------------------------------
namespace fw_rt {  // M11, below trust_rt: deploy_rt routes the FW_* opcodes to it
size_t on_fw(uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len, uint8_t* reply,
             size_t cap);
void on_result(uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p, uint16_t len);
bool busy();
void tick(uint32_t now);
void catchup_tick(uint32_t now);  // M19: the firmware catch-up's announces and a push in progress
bool on_cast(uint16_t from, uint32_t path, const uint8_t* args, uint16_t len, uint32_t now);  // M19
bool receiving();    // an image arriving (the status LED's "updating")
uint32_t received();  // its bytes so far
bool on_trial();
}  // namespace fw_rt

extern const BoardServices g_board_services;  // below, with the background workers it names

namespace deploy_rt {

constexpr uint8_t kMaxTrialBoots = CONFIG_POT_TRIAL_MAX_BOOTS;
constexpr uint32_t kTrialHeartbeats = CONFIG_POT_TRIAL_HEARTBEATS;

EspSlotStore g_store;
DeployState g_state{};
BootOutcome g_outcome = BootOutcome::NoDeployment;
// One buffer, used twice: by boot() to load the active slot before any frame can arrive, then by
// the receiver. Nothing keeps pointers into it after boot -- the actors' settings are copied out, and
// M7's guests into their own pool.
#if CONFIG_POT_WASM
constexpr size_t kImageBufLen = kMaxImageLen;  // M7: guests carry their code
EXT_RAM_BSS_ATTR uint8_t g_guest_pool[kMaxImageLen];  // PSRAM; the guest task reads the modules here
#elif CONFIG_SPIRAM
constexpr size_t kImageBufLen = kMaxImageLen;  // M19/M20: a cell of twenty placed boards, a tree's package (PSRAM)
#elif CONFIG_POT_PRESENT
constexpr size_t kImageBufLen = 2048;  // M13: the outside section's names and the broker's CA (deploy.py's limit)
#else
constexpr size_t kImageBufLen = 512;  // no guest section can run here, so images stay as small as before M7
#endif
EXT_RAM_BSS_ATTR uint8_t g_rx_buf[kImageBufLen];  // M6: PSRAM when present; tasks only
uint8_t* const g_image = g_rx_buf;
DeployReceiver g_rx(g_store, g_rx_buf, sizeof(g_rx_buf));
bool g_trial = false;            // running a pending slot, not yet confirmed
uint32_t g_trial_base = 0;       // M12: heartbeats sent when the trial began (a live change starts one mid-boot)
uint16_t g_node_id = 0;
// The deploy server, the push to the peers and catch-up (pot/package_agent.hpp), made in boot() once the node exists.
// It also holds what runs here -- the package's counter and digest, for the inventory (0 = built-in behaviour) -- and
// the reboot a deploy schedules. PSRAM when present, like the buffer it works in: tasks only.
EXT_RAM_BSS_ATTR alignas(PackageAgent) uint8_t g_agent_storage[sizeof(PackageAgent)];
PackageAgent* g_agent = nullptr;
// M6: the image's portable actors, handed to the reconciler once the node exists.
EXT_RAM_BSS_ATTR PortableSpec g_portable[kMaxPortable];  // M8.1: any portable actor type
size_t g_portable_n = 0;
uint32_t g_live_applies = 0;     // packages taken without a reboot, this boot

const char* slot_name(uint8_t s) { return s == kSlotA ? "A" : s == kSlotB ? "B" : "none"; }

void save() {
    if (!g_store.save_state(g_state)) {
        ESP_LOGE(kTag, "deploy: could not persist the A/B state");
    }
}

// M13: the running package's outside section, names included, for the presenter (BoardServices::outside_at). A
// copy, since the image buffer it came from is reused by the next deploy. PSRAM: only tasks read it.
struct OutsideName {
    uint32_t hash;
    uint8_t flags;
    uint8_t len;
    char name[kMaxOutsideName + 1];
};
EXT_RAM_BSS_ATTR OutsideName g_outside[kMaxOutside];
size_t g_outside_n = 0;
EXT_RAM_BSS_ATTR uint8_t g_broker_ca[kMaxBrokerCa];
size_t g_broker_ca_len = 0;

// The cell's other boards, as the running package names them: every board an actor is pinned to. A split station
// probes for one of these gone missing (pot/split_probe.hpp); a package of portable actors only names none.
uint16_t g_members[kMaxPeers];
size_t g_members_n = 0;
SplitProbe g_split;  // the link loop's; the stats task only reads its counters

void keep_members(const DeployImage& img) {
    g_members_n = 0;
    for (uint8_t i = 0; i < img.actor_count; ++i) {
        const uint16_t n = img.actors[i].node_id;
        // Not a host (below kAssignedNodeIdMin): a node the hub bridges comes and goes with the PC, and its absence
        // says nothing about the radio -- counted, a tablet switched off would send boards probing other channels.
        if (n == 0 || n == g_node_id || n >= kPortableNode || n < kAssignedNodeIdMin ||
            (n >= kStubFirst && n <= kStubLast)) continue;
        bool have = false;
        for (size_t k = 0; k < g_members_n; ++k) have = have || g_members[k] == n;
        if (!have && g_members_n < kMaxPeers) g_members[g_members_n++] = n;
    }
}

void keep_outside(const DeployImage& img) {
    keep_members(img);
    g_outside_n = 0;
    for (size_t i = 0; i < img.outside_count && i < kMaxOutside; ++i) {
        OutsideName& o = g_outside[g_outside_n++];
        const char* name = nullptr;
        size_t len = 0;
        outside_rule(img, i, o.hash, o.flags, &name, &len);
        o.len = static_cast<uint8_t>(len > kMaxOutsideName ? kMaxOutsideName : len);
        std::memcpy(o.name, name, o.len);
        o.name[o.len] = '\0';
    }
    // The broker's CA (version 4), copied for the same reason.
    g_broker_ca_len = img.broker_ca_len <= kMaxBrokerCa ? img.broker_ca_len : 0;
    if (g_broker_ca_len != 0) std::memcpy(g_broker_ca, img.broker_ca, g_broker_ca_len);
}

// Read the active slot, validate every actor meant for this node, and only then apply them: a
// half-applied image is worse than either whole one.
bool load_and_apply(const char** why) {
    size_t len = 0;
    if (!g_store.read_slot(g_state.active, g_image, kImageBufLen, len)) {
        *why = "slot unreadable or fails its CRC";
        return false;
    }
    // M12: a slot holds the whole signed stream (so it can be passed on to a board that was off); the
    // image is its first part. A slot written before M12 holds the image alone, and its extent is all of it.
    const size_t extent = image_extent(g_image, len);
    if (extent != 0 && extent <= len) len = extent;
    EXT_RAM_BSS_ATTR static DeployImage img;  // M19: 64 declarations, off the boot stack and internal RAM
    img = DeployImage{};
    if (!parse_image(g_image, len, img, why, g_node_id)) {  // M19: this board's cells only
        return false;
    }
    g_actor_env.trial_window_ms = kTrialHeartbeats * static_cast<uint32_t>(CONFIG_POT_HB_PERIOD_MS);
#if CONFIG_POT_WASM
    // M7: the guests first, so the guest rows below can check their declarations against them. Their
    // authors' signatures were checked when the image arrived and are checked again before each runs.
    if (!guest_library().load(img, g_guest_pool, sizeof(g_guest_pool), why)) {
        return false;
    }
#else
    if (img.guest_count > 0) {
        *why = "image carries guests, and this build has no sandbox (CONFIG_POT_WASM)";
        return false;
    }
#endif
    // Every node takes every portable actor: section 7.7's "code moves at deploy time, not at failure
    // time". The reconciler decides where each one runs; the registration table says what each is.
    size_t portable_n = 0;
#if CONFIG_POT_RECONCILER
    size_t n_kinds = 0;
    const ActorKind* kinds = app::actor_table(&n_kinds);
    if (!collect_portable(img, kinds, n_kinds, g_portable, kMaxPortable, portable_n, why)) {
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
    // M13: what the package lets the outside do. parse_image judged every rule, so this cannot refuse.
    apply_outside(img, g_node->ns());
    keep_outside(img);
    g_agent->set_running(img.rollback_counter, img.package_digest);
    ESP_LOGI(kTag, "deploy: image counter %u, digest %02x%02x%02x%02x..., %u actor(s) in the image",
             static_cast<unsigned>(img.rollback_counter), img.package_digest[0],
             img.package_digest[1], img.package_digest[2], img.package_digest[3],
             static_cast<unsigned>(img.actor_count));
    return true;
}

// Before any task starts. The trial counter is persisted *before* the slot runs, so a module that
// crashes the node has already been counted when it does.
bool apply_live(DeployStatus* refused);  // below

void boot(uint16_t node_id) {
    g_node_id = node_id;
    PackageAgentHooks hooks;
    hooks.committed = [](void*, DeployStatus* refused) { return apply_live(refused); };
    hooks.busy = [](void*) { return fw_rt::busy(); };
    hooks.ready = [](void*) { return g_store.ready(); };
    hooks.log = [](void*, const char* line) { ESP_LOGI(kTag, "%s", line); };
    g_agent = new (g_agent_storage) PackageAgent(*g_node, g_store, g_rx, g_rx_buf, sizeof(g_rx_buf), g_state, hooks,
                                                 kHostMac);
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

// ---- M12: a committed package taken without a reboot ---------------------------------------------
// The slot is written and on trial exactly as before (pending, the fallback kept); what changes is only
// that the running actors move to it in place: identical ones are kept (their state, their outputs), the
// rest stop and their outputs leave the namespace, new ones start. The live run counts as the trial's
// first boot, so a package that crashes the board gets no more boots than one that arrived by reboot,
// and the trial still ends only after kTrialHeartbeats heartbeats from the moment it began.
// Returns false, changing nothing, when the change cannot be live (a guest's code differs: the running
// guests read it from the pool) -- the caller then reboots into the slot as before.
EXT_RAM_BSS_ATTR PortableSpec g_portable_stage[kMaxPortable];

size_t cell_nodes(uint16_t* ids) {
    size_t n = 0;
    ids[n++] = g_node_id;
    for (size_t i = 0; i < PeerTable::capacity() && n < kMaxPeers + 1; ++i) {
        const PeerLink& q = g_node->peers().slot(i);
        if (q.state != PeerState::Free && std::memcmp(q.mac, kHostMac, kMacLen) != 0) ids[n++] = q.node_id;
    }
    return n;
}

void rec_create();  // after the board services exist, below

bool apply_live(DeployStatus* refused) {
    EXT_RAM_BSS_ATTR static DeployImage img;  // off the link task's stack, which this runs on, and internal RAM
    *refused = DeployStatus::Ok;
    const char* why = "?";
    const size_t extent = image_extent(g_rx.image(), g_rx.image_len());
    if (extent == 0 || !parse_image(g_rx.image(), extent, img, &why, g_node_id)) return false;
#if CONFIG_POT_WASM
    if (!guest_library().same_guests(img)) {
        ESP_LOGI(kTag, "deploy: the package changes a guest's code - rebooting into it");
        return false;
    }
#else
    if (img.guest_count > 0) return false;  // the reboot refuses it, the way it always has
#endif
    size_t portable_n = 0;
#if CONFIG_POT_RECONCILER
    size_t n_kinds = 0;
    const ActorKind* kinds = app::actor_table(&n_kinds);
    if (!collect_portable(img, kinds, n_kinds, g_portable_stage, kMaxPortable, portable_n, &why)) {
        *refused = DeployStatus::BadImage;
    }
#else
    for (uint8_t i = 0; i < img.actor_count; ++i) {
        if (img.actors[i].node_id == kPortableNode) {
            why = "portable actor, and this build has no reconciler (CONFIG_POT_RECONCILER)";
            *refused = DeployStatus::BadImage;
        }
    }
#endif
    uint16_t ids[kMaxPeers + 1];
    const size_t n_ids = cell_nodes(ids);
    ActorRuntime::ReloadStats as;
    if (*refused == DeployStatus::Ok &&
        !g_actors.reload(img, g_node_id, g_actor_env, now_ms_(), ids, n_ids, &why, &as)) {
        *refused = DeployStatus::BadImage;
    }
    if (*refused != DeployStatus::Ok) {
        // Nothing changed; the slot just written is not what runs, so it must not be what boots either.
        ESP_LOGE(kTag, "deploy: package counter %u refused (%s) - the running package stays",
                 static_cast<unsigned>(img.rollback_counter), why);
        revert_now(g_state);
        save();
        return false;
    }
    if (as.failed != nullptr) ESP_LOGE(kTag, "actors: %s could not start; dropped", as.failed);
    apply_outside(img, g_node->ns());  // M13: before the reconciler re-declares, so its entries take the flags
    keep_outside(img);
#if CONFIG_POT_RECONCILER
    std::memcpy(g_portable, g_portable_stage, sizeof(PortableSpec) * portable_n);
    g_portable_n = portable_n;
    Reconciler::ReloadStats rs;
    if (g_rec == nullptr) {
        if (portable_n > 0) {
            ck_persist::g_boot_reads = false;
            rec_create();
            g_rec->start(now_ms_());
        }
        rs.added = portable_n;
    } else {
        g_rec->reload(g_portable, g_portable_n, now_ms_(), &rs);
    }
#endif
    g_state.trial_boots = 1;  // this run is the trial's first boot
    save();
    g_trial = true;
    g_trial_base = g_node->tx_tally().beacons;
    g_agent->set_running(img.rollback_counter, img.package_digest);
    ++g_live_applies;
    std::printf("{\"t\":\"package_live\",\"node\":%u,\"counter\":%u,\"kept\":%u,\"stopped\":%u,\"started\":%u,"
                "\"outputs_removed\":%u"
#if CONFIG_POT_RECONCILER
                ",\"portable_kept\":%u,\"portable_stopped\":%u,\"portable_added\":%u,\"portable_outputs_removed\":%u"
#endif
                "}\n",
                static_cast<unsigned>(g_node_id), static_cast<unsigned>(img.rollback_counter),
                static_cast<unsigned>(as.kept), static_cast<unsigned>(as.stopped), static_cast<unsigned>(as.started),
                static_cast<unsigned>(as.outputs_removed)
#if CONFIG_POT_RECONCILER
                ,
                static_cast<unsigned>(rs.kept), static_cast<unsigned>(rs.stopped), static_cast<unsigned>(rs.added),
                static_cast<unsigned>(rs.outputs_removed)
#endif
    );
    return true;
}

// ---- passing a committed package on to the peers, and catch-up: pot/package_agent.hpp (since 2026-10-14) ----
void schedule_reboot() { g_agent->schedule_reboot(now_ms_()); }

// ---- M12: the inventory (NODE_STATUS) ------------------------------------------------------------
// Payload: u16 target (0 = whoever answers). Another node's is passed on over the radio, one at a time,
// the way FW_STATUS is; a reply of {0, target} means it could not be reached.
size_t build_node_status(uint8_t* out, size_t cap, bool cabled);  // below, where every part is known

struct StatusProxy {
    uint16_t awaiting = 0, target = 0, requester = 0, requester_msg = 0;
};
StatusProxy g_status_proxy;

void status_unreachable(uint16_t to, uint16_t msg, uint16_t target) {
    const uint8_t wire[3] = {0, static_cast<uint8_t>(target), static_cast<uint8_t>(target >> 8)};
    g_node->send_reply_raw(to, msg, wire, sizeof(wire));
}

void on_node_status(uint16_t from, uint16_t msg_id, const uint8_t* p, uint16_t len) {
    const uint16_t target = (p != nullptr && len >= 2) ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : 0;
    if (target != 0 && target != g_node_id) {
        const uint16_t m = g_node->send_deploy(target, kOpNodeStatus, p, len);
        if (m == 0) {
            status_unreachable(from, msg_id, target);
            return;
        }
        if (g_status_proxy.awaiting != 0) status_unreachable(g_status_proxy.requester, g_status_proxy.requester_msg, g_status_proxy.target);
        g_status_proxy = StatusProxy{m, target, from, msg_id};
        return;
    }
    const PeerLink* q = g_node->peers().find_by_node_id(from);
    const bool cabled = q != nullptr && std::memcmp(q->mac, kHostMac, kMacLen) == 0;
    static uint8_t wire[kNodeStatusMaxLen];  // off the link task's stack
    const size_t n = build_node_status(wire, sizeof(wire), cabled);
    if (n > 0) g_node->send_reply_raw(from, msg_id, wire, static_cast<uint16_t>(n));
}

// ---- M14: the cell's leases (LEASE) ---------------------------------------------------------------
// Every board keeps a table; the lowest-id live board is the one asked. A request with target 0 is for the arbiter
// this board picks; it goes on over the radio with that board's id in it, so the arbiter answers it as its own.
LeaseTable g_leases;  // under g_mutex

struct LeaseProxy {
    uint16_t awaiting = 0, target = 0, requester = 0, requester_msg = 0;
    uint8_t kind = 0;
};
LeaseProxy g_lease_proxy;

void lease_unreachable(uint16_t to, uint16_t msg, uint16_t arbiter, uint8_t kind) {
    LeaseReply r;
    r.status = LeaseStatus::Unreachable;
    r.kind = kind;
    r.arbiter = arbiter;
    uint8_t wire[kLeaseReplyMaxLen];
    const size_t n = store_lease_reply(r, wire, sizeof(wire));
    g_node->send_reply_raw(to, msg, wire, static_cast<uint16_t>(n));
}

void on_lease(uint16_t from, uint16_t msg_id, const uint8_t* p, uint16_t len) {
    LeaseRequest req;
    if (!load_lease_request(p, len, req)) {
        LeaseReply r;  // Malformed
        uint8_t wire[kLeaseReplyMaxLen];
        r.arbiter = g_node_id;
        g_node->send_reply_raw(from, msg_id, wire, static_cast<uint16_t>(store_lease_reply(r, wire, sizeof(wire))));
        return;
    }
    uint16_t target = req.target;
    if (target == 0) target = lease_arbiter_of(g_node->peers(), g_node_id, kHostMac);
    if (target != g_node_id) {
        req.target = target;
        uint8_t wire[kLeaseRequestMaxLen];
        const size_t wl = store_lease_request(req, wire, sizeof(wire));
        const uint16_t m = wl > 0 ? g_node->send_deploy(target, kOpLease, wire, static_cast<uint16_t>(wl)) : 0;
        if (m == 0) {
            lease_unreachable(from, msg_id, target, req.kind);
            return;
        }
        if (g_lease_proxy.awaiting != 0) {
            lease_unreachable(g_lease_proxy.requester, g_lease_proxy.requester_msg, g_lease_proxy.target,
                              g_lease_proxy.kind);
        }
        g_lease_proxy = LeaseProxy{m, target, from, msg_id, req.kind};
        return;
    }
    const LeaseReply r = g_leases.handle(req, now_ms_(), g_node_id);
    if (r.status != LeaseStatus::Ok || req.op != kLeaseQuery) {
        ESP_LOGI(kTag, "lease %s from 0x%04x: %s (holder now: %s)", lease_kind_str(req.kind), from,
                 lease_status_str(r.status), r.name_len ? r.name : "none");
    }
    uint8_t wire[kLeaseReplyMaxLen];
    g_node->send_reply_raw(from, msg_id, wire, static_cast<uint16_t>(store_lease_reply(r, wire, sizeof(wire))));
}

// ---- M15: telemetry (TELEMETRY) -----------------------------------------------------------------------
// What the console's stats lines say, in pot/telemetry.hpp's binary sections, for the hub; another board's is passed
// on over the radio, one at a time, the way NODE_STATUS is. Answered under g_mutex (the deploy server's caller).
EXT_RAM_BSS_ATTR TelemetryEventLog g_tel_events;  // under g_mutex; filled where the stats task drains the event ring

struct TelemetryProxy {
    uint16_t awaiting = 0, target = 0, requester = 0, requester_msg = 0;
};
TelemetryProxy g_tel_proxy;

void telemetry_unreachable(uint16_t to, uint16_t msg, uint16_t target) {
    // A reply with no section: version, 0, the node that could not be reached.
    const uint8_t wire[4] = {kTelemetryVersion, 0, static_cast<uint8_t>(target), static_cast<uint8_t>(target >> 8)};
    g_node->send_reply_raw(to, msg, wire, sizeof(wire));
}

size_t build_telemetry(const TelemetryRequest& rq, uint8_t* out, size_t cap) {
    if (rq.section == kTelemetryHealth) {
        const NodeCounters& c = g_node->counters();
        const auto& ns = g_node->ns_counters();
        const EspNowQueueStats q = espnow_queue_stats();
        TelemetryHealth h;
        h.uptime_s = now_ms_() / 1000;
        h.boot_epoch = g_node->config().boot_epoch;
        h.channel = g_node->channel();
        const uint32_t caps = g_node->caps();
        h.flags = static_cast<uint8_t>((g_no_radio ? kTelHealthNoRadio : 0) | ((caps & kHelloCapBusy) ? kTelHealthBusy : 0) |
                                       ((caps & kHelloCapConnected) ? kTelHealthConnected : 0) |
                                       (g_node->trust_enrolled() ? kTelHealthEnrolled : 0) |
                                       (g_node->heartbeat_period_ms() * g_node->config().hb_miss_limit >
                                                kHbPeriodWirelessMs * kHbMissLimitWireless
                                            ? kTelHealthLongWindow
                                            : 0));
        h.hb_period_ms = static_cast<uint16_t>(g_node->heartbeat_period_ms());
        h.int_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        h.int_min = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        h.int_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        h.int_total_kib = static_cast<uint16_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
        h.ext_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        h.ext_min = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        h.peers_alive = static_cast<uint8_t>(g_node->peers().count_in_state(PeerState::Alive));
        h.peers_dead = static_cast<uint8_t>(g_node->peers().count_in_state(PeerState::Dead));
        h.actors_pinned = static_cast<uint8_t>(g_actors.running());
#if CONFIG_POT_RECONCILER
        for (size_t i = 0; g_rec != nullptr && i < g_rec->actor_count(); ++i) {
            if (g_rec->view(i).running) ++h.actors_portable_here;
        }
#endif
        h.deaths = c.deaths_declared;
        h.revivals = c.revivals;
        h.reboots_seen = c.reboots_seen;
        h.rx_total = c.rx_total;
        h.rx_rejected = c.rx_bad_frame + c.rx_short_payload + c.rx_unknown_opcode;
        h.rx_queue_dropped = q.rx_dropped_queue_full + q.rx_dropped_oversize;
        h.tx_total = c.tx_total;
        h.tx_enqueue_err = c.tx_enqueue_err;
        h.tx_done_dropped = q.tx_done_dropped;
        h.events_dropped = g_node->events().dropped();
        h.writes_served = ns.writes_served;
        h.writes_rejected = ns.writes_rejected;
        h.link_stack_free = g_link_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_link_handle)) : 0;
        return store_telemetry_health(g_node_id, h, out, cap);
    }
    if (rq.section == kTelemetryLinks) {
        TelemetryLink links[kTelemetryLinksPerReply];
        size_t total = 0, n = 0;
        for (size_t i = 0; i < kMaxPeers; ++i) {
            const PeerLink& pl = g_node->peers().slot(i);
            if (pl.state == PeerState::Free) continue;
            if (total++ < rq.start || n >= kTelemetryLinksPerReply) continue;
            const Node::PeerAuth* pa = g_node->peer_auth(&pl);
            const uint8_t flags = static_cast<uint8_t>((std::memcmp(pl.mac, kHostMac, kMacLen) == 0 ? kTelLinkCable : 0) |
                                                       (pa != nullptr && pa->verified ? kTelLinkVerified : 0));
            links[n++] = telemetry_link(pl, g_node->peers().histogram(i), flags);
        }
        return store_telemetry_links(g_node_id, static_cast<uint8_t>(total), rq.start, links, n, out, cap);
    }
    if (rq.section == kTelemetryActors) {
        TelemetryActor acts[kTelemetryActorsPerReply];
        size_t total = 0, n = 0;
#if CONFIG_POT_RECONCILER
        total = g_rec != nullptr ? g_rec->actor_count() : 0;
        for (size_t i = rq.start; i < total && n < kTelemetryActorsPerReply; ++i) {
            const Reconciler::ActorView v = g_rec->view(i);
            TelemetryActor& a = acts[n++];
            a.key = v.key;
            a.flags = v.running ? kTelActorRunningHere : 0;
            a.owner = v.owner;
            a.assigned = v.assigned;
            a.activations = v.activations;
            a.fenced = static_cast<uint16_t>(v.fenced > 0xFFFF ? 0xFFFF : v.fenced);
            a.released = static_cast<uint16_t>(v.released > 0xFFFF ? 0xFFFF : v.released);
        }
#endif
        return store_telemetry_actors(g_node_id, static_cast<uint8_t>(total), rq.start, acts, n, out, cap);
    }
    if (rq.section == kTelemetryChip) {
        // M21: what this board is, read now from the chip itself -- never esptool's check, which restarts the board.
        esp_chip_info_t ci{};
        esp_chip_info(&ci);
        uint32_t flash_bytes = 0;
        if (esp_flash_get_size(nullptr, &flash_bytes) != ESP_OK) flash_bytes = 0;
        TelemetryChip c;
        c.model = static_cast<uint8_t>(ci.model);
        c.cores = ci.cores;
        c.revision = ci.revision;
        c.flash_mb = static_cast<uint16_t>(flash_bytes / (1024u * 1024u));
#if CONFIG_SPIRAM
        c.psram_mb = static_cast<uint16_t>(esp_psram_get_size() / (1024u * 1024u));
#endif
        c.features = ci.features;
        return store_telemetry_chip(g_node_id, c, out, cap);
    }
    if (rq.section == kTelemetryCompute) {
        // M22: M9's borrowed cores -- the lender and the job running here, found by their kind's name (no RTTI).
        TelemetryCompute c;
        const uint32_t now = now_ms_();
        for (size_t i = 0; i < g_actors.running(); ++i) {
            const char* kind = g_actors.name(i);
            if ((c.flags & kTelComputeLender) == 0 && std::strcmp(kind, kMcLenderKind.name) == 0) {
                const McLenderActor::Stats& s = static_cast<McLenderActor*>(g_actors.actor(i))->stats();
                c.flags |= kTelComputeLender;
                c.lender_accepted = s.accepted;
                c.lender_refused = s.refused;
                c.lender_served = s.served;
                c.lender_unit_ms = s.unit_ms;
            } else if ((c.flags & kTelComputeJob) == 0 && std::strcmp(kind, kMcJobKind.name) == 0) {
                const McJobActor::View v = static_cast<McJobActor*>(g_actors.actor(i))->view(now);
                c.flags |= kTelComputeJob | (v.running ? kTelComputeRunning : 0) | (v.stopped ? kTelComputeStopped : 0) |
                           (v.lend ? kTelComputeLend : 0);
                c.run = v.run;
                c.units = v.units;
                c.done = v.done;
                c.local_units = v.local_units;
                c.lent_units = v.lent_units;
                c.refused = v.refused;
                c.lost = v.lost;
                c.overdue = v.overdue;
                c.elapsed_ms = v.elapsed_ms;
                c.local_ms = v.local_ms;
                c.pi = v.pi;
                for (size_t k = 0; k < McJobActor::kMaxLenders && c.n < 4; ++k) {
                    if (v.lender_node[k] == 0) continue;
                    c.lender_node[c.n] = v.lender_node[k];
                    c.lender_answered[c.n] = v.lender_answered[k];
                    ++c.n;
                }
            }
        }
        return store_telemetry_compute(g_node_id, c, out, cap);
    }
    if (rq.section == kTelemetryAuth) {
        const Node::AuthCounters& c = g_node->auth_counters();
        TelemetryAuth a;
        a.verified = c.verified;
        a.cache_hits = c.cache_hits;
        a.refused = c.refused;
        a.rate_limited = c.rate_limited;
        a.stale_epoch = c.stale_epoch;
        a.tags_ok = c.tags_ok;
        a.bad_tag = c.bad_tag;
        a.replayed = c.replayed;
        a.untagged = c.untagged;
        a.bcast_dropped = c.bcast_dropped;
        a.beacon_epoch_ignored = c.beacon_epoch_ignored;
        a.group_refusals = g_node->group_refusals();
        return store_telemetry_auth(g_node_id, a, out, cap);
    }
    if (rq.section == kTelemetryEvents) {
        TelemetryEvent ev[kTelemetryEventsPerReply];
        const size_t n = g_tel_events.since(rq.since, ev, kTelemetryEventsPerReply);
        return store_telemetry_events(g_node_id, g_tel_events.next(), ev, n, out, cap);
    }
    return 0;
}

void on_telemetry(uint16_t from, uint16_t msg_id, const uint8_t* p, uint16_t len) {
    TelemetryRequest rq;
    if (!load_telemetry_request(p, len, rq)) {
        telemetry_unreachable(from, msg_id, g_node_id);
        return;
    }
    if (rq.target != 0 && rq.target != g_node_id) {
        const uint16_t m = g_node->send_deploy(rq.target, kOpTelemetry, p, len);
        if (m == 0) {
            telemetry_unreachable(from, msg_id, rq.target);
            return;
        }
        if (g_tel_proxy.awaiting != 0) telemetry_unreachable(g_tel_proxy.requester, g_tel_proxy.requester_msg, g_tel_proxy.target);
        g_tel_proxy = TelemetryProxy{m, rq.target, from, msg_id};
        return;
    }
    static uint8_t wire[kTelemetryReplyMax];  // off the link task's stack; under g_mutex
    const size_t n = build_telemetry(rq, wire, sizeof(wire));
    if (n > 0) {
        g_node->send_reply_raw(from, msg_id, wire, static_cast<uint16_t>(n));
    } else {
        telemetry_unreachable(from, msg_id, g_node_id);  // a section this build does not know
    }
}

void on_result(void*, uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p,
               uint16_t len) {
    if (op == kOpTelemetry) {  // M15: another board's telemetry, passed back
        if (g_tel_proxy.awaiting == 0 || msg != g_tel_proxy.awaiting || peer != g_tel_proxy.target) return;
        const TelemetryProxy tp = g_tel_proxy;
        g_tel_proxy = TelemetryProxy{};
        if (timed_out || p == nullptr || len == 0) {
            telemetry_unreachable(tp.requester, tp.requester_msg, tp.target);
        } else {
            g_node->send_reply_raw(tp.requester, tp.requester_msg, p, len);
        }
        return;
    }
    if (op == kOpLease) {  // M14: the arbiter's answer to a request passed on
        if (g_lease_proxy.awaiting == 0 || msg != g_lease_proxy.awaiting || peer != g_lease_proxy.target) return;
        const LeaseProxy lp = g_lease_proxy;
        g_lease_proxy = LeaseProxy{};
        if (timed_out || p == nullptr || len == 0) {
            lease_unreachable(lp.requester, lp.requester_msg, lp.target, lp.kind);
        } else {
            g_node->send_reply_raw(lp.requester, lp.requester_msg, p, len);
        }
        return;
    }
    if (is_fw_opcode(op)) {
        fw_rt::on_result(peer, msg, op, timed_out, p, len);
        return;
    }
    if (op == kOpNodeStatus) {  // M12: a proxied inventory reply
        if (g_status_proxy.awaiting == 0 || msg != g_status_proxy.awaiting || peer != g_status_proxy.target) return;
        const StatusProxy sp = g_status_proxy;
        g_status_proxy = StatusProxy{};
        if (timed_out || p == nullptr || len == 0) {
            status_unreachable(sp.requester, sp.requester_msg, sp.target);
        } else {
            g_node->send_reply_raw(sp.requester, sp.requester_msg, p, len);
        }
        return;
    }
    g_agent->on_result(peer, msg, op, timed_out, p, len, now_ms_());  // the push's own steps
}

// ---- the server, called from Node::on_rx with g_mutex held ---------------------------------------
size_t on_deploy(void*, uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len,
                 uint8_t* reply, size_t cap) {
    if (is_fw_opcode(op)) return fw_rt::on_fw(from, msg_id, op, p, len, reply, cap);  // M11
    if (op == kOpNodeStatus) {  // M12: answered whatever else is going on
        on_node_status(from, msg_id, p, len);
        return 0;
    }
    if (op == kOpLease) {  // M14: likewise -- a lease is asked for while a deploy runs
        on_lease(from, msg_id, p, len);
        return 0;
    }
    if (op == kOpTelemetry) {  // M15: and telemetry
        on_telemetry(from, msg_id, p, len);
        return 0;
    }
    return g_agent->serve(from, msg_id, op, p, len, reply, cap, now_ms_());
}

// From link_task, every pass.
void tick(uint32_t now) {
    g_agent->tick(now);  // catch-up, and a receive nobody finished
    if (g_trial && g_node->tx_tally().beacons - g_trial_base >= kTrialHeartbeats) {
        confirm(g_state);
        save();
        g_trial = false;
        ESP_LOGI(kTag, "deploy: trial passed after %u heartbeats - slot %s CONFIRMED",
                 static_cast<unsigned>(kTrialHeartbeats), slot_name(g_state.active));
    }
    if (g_agent->reboot_due(now)) {
        ESP_LOGW(kTag, "deploy: rebooting into the new slot");
        esp_restart();
    }
}

}  // namespace deploy_rt

void deploy_package_soon() {
    if (deploy_rt::g_agent != nullptr) deploy_rt::g_agent->soon(now_ms_());
}


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
                "\"ss_max_us\":%u,\"ss_rx\":%u,\"ss_rx_gaps\":%u,\"ss_rx_restarts\":%u,\"bus_off\":%u,"
                "\"recoveries\":%u,\"tx_bus_off\":%u,\"tx_abandoned\":%u}\n",
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
                static_cast<unsigned>(g_safe_rx_gaps), static_cast<unsigned>(g_safe_rx_restarts),
                static_cast<unsigned>(c.bus_off), static_cast<unsigned>(c.recoveries),
                static_cast<unsigned>(c.tx_bus_off), static_cast<unsigned>(c.tx_abandoned));
}

}  // namespace m4
#endif

// ---------------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------------

void link_task(void*) {
    // Both receive paths wake this task (a task notification) when they queue a frame, and it sleeps on that rather
    // than on the radio's queue alone: frames from the PC's cable used to wait up to 20 ms (2026-10-26).
    espnow_set_rx_notify(xTaskGetCurrentTaskHandle());
#if CONFIG_POT_SERIAL_LINK
    serial_port_set_rx_notify(xTaskGetCurrentTaskHandle());
#endif
    g_node->start();
    if (g_rec != nullptr) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_rec->start(now_ms_());
        xSemaphoreGive(g_mutex);
    }
    uint32_t next_button_poll_ms = now_ms_();
    uint32_t next_led_ms = now_ms_() + status_led::kRenderMs;
#if CONFIG_POT_EXTENDER || CONFIG_POT_WIFI
    uint32_t next_hunt_poll_ms = now_ms_();
    uint32_t next_split_ms = now_ms_();
    bool was_associated = false;  // matches the window app_main chose: hunting unless already associated
    bool ip_seen = false;         // M13: the connected bit's hysteresis
    uint32_t ip_since_ms = now_ms_();
#endif
    uint32_t next_assess_ms = now_ms_() + status_led::kUpdateMs;
#if CONFIG_POT_POCKET
    uint32_t next_pocket_ms = now_ms_();
#endif
    status_led::Colour colour = status_led::Colour::Yellow;
    // "Updating" counts a transfer only while its bytes still arrive: one whose sender went away would
    // otherwise leave the LED purple until the next transfer began.
    uint32_t upd_bytes = 0;
    uint32_t upd_seen_ms = 0;

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

        // This loop's only sleep was once inside espnow_rx_pop(), and on a node whose radio never came up
        // that function returns *immediately* rather than blocking — so the loop became a hard busy
        // spin at priority 6. Nothing crashed and nothing logged; the statistics task at priority 3
        // simply never ran again, which presents as a node that boots perfectly and then goes silent
        // for ever. Found under emulation, but §8.1 makes this a hardware path too: a node whose
        // radio fails to start is meant to keep running *and stay diagnosable*, and starving the one
        // task that reports anything is the opposite of that.
        //
        // So the sleep is now the loop's own responsibility rather than a side effect of whichever
        // transport happens to exist: a task-notification wait that every receive path ends (2026-10-26).
        const bool radio_can_block = espnow_up();

        RxSlot rx;
        bool got = espnow_rx_pop(rx, 0);
        if (!got) {
            // Sleep until the radio or the PC's cable queues a frame (each notifies this task) or the next deadline.
            // Waiting on the radio's queue alone left the cable's frames queued up to 20 ms, and a burst from the hub
            // overflowed them: ~1 in 120 of the PC's reads unanswered (2026-10-26). A notification given since the
            // last pass makes this return at once, so a frame queued just before it is not slept past.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
            got = espnow_rx_pop(rx, 0);
        }
        if (got) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            tee_frame("rx", rx.src_mac, rx.rssi, rx.data, rx.len);
#if CONFIG_POT_RSSI_TEE
            g_rx_now = &rx;
#endif
            g_rx_from_radio = true;
            if (!deaf_to(rx.src_mac)) g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            for (size_t drained = 1; drained < kRxRingSlots && espnow_rx_pop(rx, 0); ++drained) {
                tee_frame("rx", rx.src_mac, rx.rssi, rx.data, rx.len);
                if (!deaf_to(rx.src_mac)) g_node->on_rx(rx.src_mac, rx.data, rx.len, rx.recv_us, rx.rssi);
            }
#if CONFIG_POT_RSSI_TEE
            g_rx_now = nullptr;  // anything after this (the host's serial link) carries no radio metadata
#endif
            g_rx_from_radio = false;
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
                uint8_t cmac[kMacLen];
                std::memcpy(cmac, kHostMac, kMacLen);
                Frame cf{};
                if (parse(sbuf, slen, cf) == FrameError::Ok && cf.hdr.src >= kStubFirst && cf.hdr.src <= kStubLast) {
                    cmac[4] = static_cast<uint8_t>(cf.hdr.src >> 8);  // M19: a stub node the PC hosts
                    cmac[5] = static_cast<uint8_t>(cf.hdr.src & 0xFF);
                } else if (cf.hdr.src >= kBridgedFirst && cf.hdr.src <= kBridgedLast) {
                    std::memcpy(cmac, kBridgedPrefix, sizeof(kBridgedPrefix));  // a node the hub bridges: untrusted
                    cmac[4] = static_cast<uint8_t>(cf.hdr.src >> 8);
                    cmac[5] = static_cast<uint8_t>(cf.hdr.src & 0xFF);
                }
                tee_frame("rx", cmac, 0, sbuf, slen);
                g_host_heard_ms.store(now_ms_() == 0 ? 1 : now_ms_());  // M10: broadcasts follow it down the cable
                g_node->on_rx(cmac, sbuf, slen, srecv, 0);
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

#if CONFIG_POT_EXTENDER || CONFIG_POT_WIFI
        // M6.1: while the station hunts for its router, its scans take the radio off the cell's channel
        // for longer than the build's death window (3.7-4.0 s measured, M0-LOG session 30), so declare a
        // window that covers one. Polled outside the lock: the status call goes into the Wi-Fi driver.
        if (uplink::active() && static_cast<int32_t>(nt - next_hunt_poll_ms) >= 0) {
            next_hunt_poll_ms = nt + 200;
            const bool assoc = uplink::associated();
            const uint8_t radio_ch = espnow_current_channel();
            if (assoc != was_associated) {
                was_associated = assoc;
                const uint8_t misses = assoc ? CONFIG_POT_HB_MISS_LIMIT : uplink::hunt_misses();
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_node->set_heartbeat_window(CONFIG_POT_HB_PERIOD_MS, misses);
                xSemaphoreGive(g_mutex);
                ESP_LOGI(kTag, "%s: station %s; declaring %u x %u ms", uplink::name(), assoc ? "associated" : "hunting",
                         static_cast<unsigned>(misses), static_cast<unsigned>(CONFIG_POT_HB_PERIOD_MS));
            }
            // Declare a channel only while it is the router's: associated, and the node already moved to
            // where the radio is (the router's channel reaches the node through take_channel below, a
            // pass later). Hunting, or between the two, the HELLO says 0 and nobody is sent anywhere.
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const bool known = assoc && radio_ch == g_node->channel();
            if (known != g_node->channel_known()) g_node->set_channel_known(known);
            // M13: on a network outside the cell, with hysteresis -- an address held 3 s before we say so, gone 5 s
            // before we unsay it -- so a flapping station does not drag the presenting role back and forth.
            {
                const bool ip = uplink::has_ip();
                if (ip != ip_seen) {
                    ip_seen = ip;
                    ip_since_ms = nt;
                }
                const bool want = ip_seen ? (nt - ip_since_ms >= 3000 || g_node->connected())
                                          : (nt - ip_since_ms < 5000 && g_node->connected());
                if (want != g_node->connected()) {
                    g_node->set_connected(want);
                    ESP_LOGI(kTag, "%s: %s the house network", uplink::name(), want ? "on" : "off");
                }
            }
            // M13: a lower-id station is the cell's channel authority; if it is on another channel than ours,
            // Potluck's station picks again preferring it (at most every 30 s), and announces nothing meanwhile.
            const uint8_t auth = g_node->outranking_channel();
            const uint8_t cell_ch = g_node->channel();
            xSemaphoreGive(g_mutex);
            // The channel the station prefers at its next pick: the authority's if one outranks it, else the cell's
            // (found by review: it was set only at boot, so after a move every reconnect preferred a stale channel).
            uplink::prefer(auth != 0 ? auth : cell_ch);
            // Split by a mesh's channels from boards the package names (2026-10-18): probe the network's other
            // channels for them, at most one probe in flight, paced by the probe itself.
            if (static_cast<int32_t>(nt - next_split_ms) >= 0) {
                next_split_ms = nt + 1000;
                uint8_t missing = 0, alive = 0;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                for (size_t k = 0; k < deploy_rt::g_members_n; ++k) {
                    const PeerLink* q = g_node->peers().find_by_node_id(deploy_rt::g_members[k]);
                    if (q == nullptr || q->state != PeerState::Alive) ++missing;
                }
                for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                    const PeerLink& q = g_node->peers().slot(i);
                    if (q.state == PeerState::Alive && !off_radio(q.mac)) ++alive;
                }
                const bool outranked = auth != 0 && auth == radio_ch;
                xSemaphoreGive(g_mutex);
                const SplitProbe::Act act = deploy_rt::g_split.tick(SplitProbe::Inputs{
                    nt, assoc, radio_ch, uplink::seen_channels(), missing, alive, outranked});
                if (act != SplitProbe::Act::None && uplink::join_channel(deploy_rt::g_split.target())) {
                    std::printf("{\"t\":\"split\",\"act\":\"%s\",\"from\":%u,\"to\":%u,\"missing\":%u,\"alive\":%u}\n",
                                act == SplitProbe::Act::Probe ? "probe" : "return", static_cast<unsigned>(radio_ch),
                                static_cast<unsigned>(deploy_rt::g_split.target()), static_cast<unsigned>(missing),
                                static_cast<unsigned>(alive));
                }
            }
            if (assoc && auth != 0 && auth != radio_ch && uplink::defer_to(auth)) {
                ESP_LOGI(kTag, "%s: the cell's channel authority is on channel %u, this station on %u; picking again",
                         uplink::name(), static_cast<unsigned>(auth), static_cast<unsigned>(radio_ch));
            }
        }
#endif

#if CONFIG_POT_TX_DEAD_SOFT_S > 0
        {
            // The experiment: a driver restart first (Kconfig help, M0-LOG session 42). Once per stall.
            static bool soft_tried = false;
            static uint32_t soft_at = 0;
            const uint32_t since = g_radio_fail_since_ms.load();
            if (since == 0 && soft_tried) {
                std::printf("{\"t\":\"tx_soft_ok\",\"node\":%u,\"after_ms\":%lu}\n",
                            static_cast<unsigned>(g_node->config().node_id), static_cast<unsigned long>(nt - soft_at));
                soft_tried = false;
            } else if (since != 0 && !soft_tried && uplink::kind() != uplink::Kind::Extender &&
                       nt - since > static_cast<uint32_t>(CONFIG_POT_TX_DEAD_SOFT_S) * 1000u) {
                // Potluck owns the driver: no station, or its own `wifi` module's (ESP-NOW attaches to that one too,
                // so espnow_attached() is no test -- found on the bench: the first try skipped C, the board it was for).
                soft_tried = true;
                soft_at = nt;
                uint8_t macs[kMaxPeers][kMacLen];
                size_t n = 0;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                const uint8_t ch = g_node->channel();
                for (size_t i = 0; i < kMaxPeers; ++i) {
                    const PeerLink& q = g_node->peers().slot(i);
                    if (q.state != PeerState::Free && !off_radio(q.mac)) {
                        std::memcpy(macs[n++], q.mac, kMacLen);
                    }
                }
                xSemaphoreGive(g_mutex);
                const char* failed_at = nullptr;
                int32_t reset_err = 0;
                const bool ok = espnow_reset_radio(ch, uplink::kind() == uplink::Kind::Wifi, &failed_at, &reset_err);
                size_t readded = 0;
                for (size_t i = 0; ok && i < n; ++i) readded += espnow_add_peer(macs[i], 0) ? 1 : 0;
                std::printf("{\"t\":\"tx_soft\",\"node\":%u,\"failing_ms\":%lu,\"first_err\":\"0x%lx\",\"ok\":%d,"
                            "\"failed_at\":\"%s\",\"reset_err\":\"0x%lx\","
                            "\"peers\":%u,\"readded\":%u,\"int_free\":%lu}\n",
                            static_cast<unsigned>(g_node->config().node_id), static_cast<unsigned long>(nt - since),
                            static_cast<unsigned long>(static_cast<uint32_t>(g_radio_first_err.load())), ok ? 1 : 0,
                            failed_at != nullptr ? failed_at : "",
                            static_cast<unsigned long>(static_cast<uint32_t>(reset_err)),
                            static_cast<unsigned>(n), static_cast<unsigned>(readded),
                            static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            }
        }
#endif
#if CONFIG_POT_TX_DEAD_RESTART_S > 0
        if (const uint32_t since = g_radio_fail_since_ms.load();
            since != 0 && nt - since > static_cast<uint32_t>(CONFIG_POT_TX_DEAD_RESTART_S) * 1000u) {
            std::printf("{\"t\":\"tx_dead\",\"node\":%u,\"failing_ms\":%lu,\"last_ok_ms\":%lu,\"first_err\":\"0x%lx\","
                        "\"last_err\":\"0x%lx\",\"refused\":%lu,\"int_free_at_first\":%lu,\"int_free\":%lu,"
                        "\"action\":\"restart\"}\n",
                        static_cast<unsigned>(g_node->config().node_id), static_cast<unsigned long>(nt - since),
                        static_cast<unsigned long>(g_radio_ok_ms.load()),
                        static_cast<unsigned long>(static_cast<uint32_t>(g_radio_first_err.load())),
                        static_cast<unsigned long>(static_cast<uint32_t>(g_radio_last_err.load())),
                        static_cast<unsigned long>(g_radio_refused.load()),
                        static_cast<unsigned long>(g_radio_first_err_heap.load()),
                        static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            std::fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
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
        if (const uint8_t rch = uplink::take_channel(); rch != 0 && rch != g_node->channel()) {
            const uint8_t auth = g_node->outranking_channel();
            if (auth != 0 && auth != rch) {
                // M13: outranked -- the cell follows the lowest-id station, never this one's router.
                ESP_LOGW(kTag, "%s: router is on channel %u, the cell's authority (0x%04x) on %u; not announced",
                         uplink::name(), static_cast<unsigned>(rch), static_cast<unsigned>(g_node->channel_authority()),
                         static_cast<unsigned>(auth));
            } else if (!g_node->move_cell(rch, 0)) {
                ESP_LOGW(kTag, "%s: router is on channel %u, outside the cell's range; not announced", uplink::name(),
                         static_cast<unsigned>(rch));
            }
        }
        xSemaphoreGive(g_mutex);

#if CONFIG_POT_POCKET
        if (pocket::active() && static_cast<int32_t>(nt - next_pocket_ms) >= 0) {
            next_pocket_ms = nt + 1000;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const pocket::Snapshot& snap = pocket_snapshot(nt);
            xSemaphoreGive(g_mutex);
            pocket::publish(snap);  // only this task writes it
        }
        if (pocket::active()) {
            // M26: a logged-in phone's writes, queued by the page's server -- applied here, the node's own task
            pocket::PendingWrite pw;
            while (pocket::take_write(&pw)) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                const char* result = pocket_apply_write(pw, nt);
                xSemaphoreGive(g_mutex);
                pocket::write_result(pw.name, result);
            }
        }
#endif
        if (static_cast<int32_t>(nt - next_assess_ms) >= 0) {
            next_assess_ms = nt + status_led::kUpdateMs;
            const bool receiving = fw_rt::receiving() || deploy_rt::g_rx.in_progress();
            const uint32_t bytes = fw_rt::received() +
                                   static_cast<uint32_t>(deploy_rt::g_rx.image_len());
            if (receiving && bytes != upd_bytes) upd_seen_ms = nt;
            upd_bytes = bytes;
            const uint32_t failing_since = g_radio_fail_since_ms.load();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const status_led::Signals sig{
                (receiving && nt - upd_seen_ms < 30000u) || deploy_rt::g_trial || fw_rt::on_trial(),
                g_node->scanning(),
                failing_since != 0 && nt - failing_since > 3000u,
            };
            colour = status_led::assess(*g_node, nt, sig);
            xSemaphoreGive(g_mutex);
        }
        if (static_cast<int32_t>(nt - next_led_ms) >= 0) {
            next_led_ms = nt + status_led::kRenderMs;
            status_led::render(colour, nt);  // outside the lock: the RMT never holds up the node
        }

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        deploy_rt::tick(nt);
        fw_rt::tick(nt);
        fw_rt::catchup_tick(nt);  // M19
        xSemaphoreGive(g_mutex);

        // The notification wait above is the loop's sleep, radio or none (it no longer returns at once on a node whose
        // radio never started). Only a deadline already passed made it not sleep: then one tick, so this task can
        // never spin on a radio-less node, whatever the transports below it are doing.
        if (!radio_can_block && wait_ms == 0) {
            vTaskDelay(1);
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
    g_node->publish(sys_resource_hash(me, SysResource::ResetReason),
                    Value::of_u32(static_cast<uint32_t>(esp_reset_reason())));
    // An extender board: its router and hotspot figures (declared at boot only where the extender started).
    bool assoc = false;
    int8_t router_rssi = 0;
    uint8_t clients = 0;
    uint32_t reconnects = 0;
    if (extender::figures(&assoc, &router_rssi, &clients, &reconnects)) {
        if (assoc) {  // no association, no measurement: the last reading ages into STALE rather than lie
            g_node->publish(extender_resource_hash(me, ExtenderResource::RouterRssi),
                            Value::of_i32(static_cast<int32_t>(router_rssi)));
        }
        g_node->publish(extender_resource_hash(me, ExtenderResource::Clients), Value::of_u32(clients));
        g_node->publish(extender_resource_hash(me, ExtenderResource::Reconnects), Value::of_u32(reconnects));
    }
    // A board whose pocket page is up: phones on its access point, pages served, requests refused (declared only there).
    uint32_t phones = 0, pages = 0, refused = 0;
    if (pocket::figures(&phones, &pages, &refused)) {
        g_node->publish(pocket_resource_hash(me, PocketResource::Phones), Value::of_u32(phones));
        g_node->publish(pocket_resource_hash(me, PocketResource::Pages), Value::of_u32(pages));
        g_node->publish(pocket_resource_hash(me, PocketResource::Refused), Value::of_u32(refused));
    }
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
        const NsError st = g_node->read(e.path_hash, r, nullptr, true);  // a look, not a use: keeps eviction honest
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
        ck_persist::flush();  // M12: the checkpoints the reconciler asked to keep, off the link task
#if CONFIG_POT_RSSI_TEE
        if (g_print_gate != nullptr) xSemaphoreTake(g_print_gate, portMAX_DELAY);  // whole lines (above)
#endif
        const uint16_t node_id = g_node->config().node_id;
        // First, while the console is idle: the board's clock, for a host that must put several
        // boards' events on one timeline (tools/m6_bench.py). Any later line in the period waits
        // behind up to ~3 KB of output at 115200 baud, and so is a poor timestamp.
        std::printf("{\"t\":\"clk\",\"node\":%u,\"up_ms\":%u}\n", static_cast<unsigned>(node_id),
                    static_cast<unsigned>(now_ms_()));
        extender::print_status();  // M6.1: the baseline's {"t":"hs"} line, on an extender build only
        wifi::print_status();      // M13: {"t":"wifi"}, when Potluck's own station is up
        if (uplink::active()) {    // 2026-10-18: a split station's probes for the rest of its cell
            const SplitProbe::Counters& sc = deploy_rt::g_split.counters();
            std::printf("{\"t\":\"split_probe\",\"members\":%u,\"seen\":%u,\"probes\":%lu,\"stayed\":%lu,\"returned\":%lu,\"probing\":%d}\n",
                        static_cast<unsigned>(deploy_rt::g_members_n), static_cast<unsigned>(uplink::seen_channels()),
                        static_cast<unsigned long>(sc.probes), static_cast<unsigned long>(sc.stayed),
                        static_cast<unsigned long>(sc.returned), deploy_rt::g_split.probing() ? 1 : 0);
        }
#if CONFIG_POT_PRESENT
        present::print_status();   // M13: {"t":"present"}, the presenting module's figures
#endif
        pocket::print_status();    // M13.1: {"t":"pocket"}, when the pocket page is up
        // M8.2 (PS-5): what the LED shows, since nobody may be looking at it.
        std::printf("{\"t\":\"led\",\"node\":%u,\"shows\":\"%s\",\"app\":%d}\n", static_cast<unsigned>(node_id),
                    status_led::g_last_shown, status_led::g_last_app ? 1 : 0);
#if CONFIG_POT_RSSI_TEE
        std::printf("{\"t\":\"tee\",\"node\":%u,\"dropped\":%u}\n", static_cast<unsigned>(node_id),
                    static_cast<unsigned>(g_tee_dropped.load()));
#endif
        {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const uint8_t ch = g_node->channel();
            size_t radio_alive = 0;  // a host on the cable is on no channel, so it does not count
            for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                const PeerLink& q = g_node->peers().slot(i);
                if (q.state == PeerState::Alive && !off_radio(q.mac)) ++radio_alive;
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
#if CONFIG_POT_RECONCILER
        // M8.2: the portable actors running here print theirs too (passive-sensor's rf_fusion).
        for (size_t i = 0; g_rec != nullptr && i < g_rec->actor_count(); ++i) {
            EXT_RAM_BSS_ATTR static char pline[512];  // the stats task's only
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            Actor* inst = g_rec->instance(i);
            const size_t n = inst != nullptr ? inst->stats_json(pline, sizeof(pline), now_ms_()) : 0;
            xSemaphoreGive(g_mutex);
            if (n > 0) std::printf("%s\n", pline);
        }
#endif

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        refresh_sys_resources();
        xSemaphoreGive(g_mutex);

        // Events first, so one that preceded a statistics line appears before it.
        for (;;) {
            Event e{};
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const bool got = g_node->events().pop(e);
            if (got) deploy_rt::g_tel_events.push(e.at_ms, static_cast<uint8_t>(e.kind), e.node_id, e.detail_a, e.detail_b);  // M15
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
#if CONFIG_POT_SERIAL_LINK
            // The PC's cable: a frame dropped or a bad CRC here is a request the PC never had answered (2026-10-26: ~1 in
            // 120 reads lost to a full 4-slot queue, counted here and printed nowhere).
            const SerialPortStats ss = serial_port_stats();
            std::printf("{\"t\":\"serial\",\"node\":%u,\"frames_ok\":%u,\"bad_crc\":%u,\"cobs_invalid\":%u,"
                        "\"too_long\":%u,\"rx_queue_dropped\":%u,\"tx_frames\":%u,\"tx_errors\":%u}\n",
                        static_cast<unsigned>(node_id), static_cast<unsigned>(ss.framing.frames_ok),
                        static_cast<unsigned>(ss.framing.bad_crc), static_cast<unsigned>(ss.framing.cobs_invalid),
                        static_cast<unsigned>(ss.framing.too_long), static_cast<unsigned>(ss.rx_queue_dropped),
                        static_cast<unsigned>(ss.tx_frames), static_cast<unsigned>(ss.tx_errors));
#endif
            Reading sp;
            unsigned served = 0;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_node->read(path_hash("act/setpoint"), sp);
            served = g_node->ns_counters().writes_served;
            const uint32_t w_out = g_node->ns_counters().writes_outside;
            const uint32_t w_out_refused = g_node->ns_counters().writes_outside_refused;
            const unsigned ns_used = static_cast<unsigned>(g_node->ns().count());
            const unsigned ns_full = static_cast<unsigned>(g_node->ns().full_refusals());
            xSemaphoreGive(g_mutex);
            int32_t spv = 0;
            Quality spq;
            const bool has = sp.get_i32(spv, spq);
            std::printf("{\"t\":\"act\",\"node\":%u,\"setpoint\":%d,\"has_value\":%d,\"writes_served\":%u,"
                        "\"writes_outside\":%u,\"writes_outside_refused\":%u,\"ns_used\":%u,\"ns_cap\":%u,"
                        "\"ns_full_refusals\":%u}\n",
                        static_cast<unsigned>(node_id), static_cast<int>(spv), has ? 1 : 0, served,
                        static_cast<unsigned>(w_out), static_cast<unsigned>(w_out_refused), ns_used,
                        static_cast<unsigned>(kNamespaceEntries), ns_full);
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
                nvs_stats_t nvs_st{};
                nvs_get_stats(nullptr, &nvs_st);  // M12: how full the store the checkpoints share is
                std::printf("{\"t\":\"rec\",\"node\":%u,\"up_ms\":%u,\"settled\":%d,\"sent\":%u,\"received\":%u,"
                            "\"malformed\":%u,\"views\":%u,\"replies_fenced\":%u,\"ck_persisted\":%u,"
                            "\"ck_restored\":%u,\"ck_nvs_writes\":%u,\"ck_nvs_deferred\":%u,\"ck_nvs_errors\":%u,\"ck_nvs_last_err\":%d,"
                            "\"nvs_used\":%u,\"nvs_available\":%u,\"actors\":[",
                            static_cast<unsigned>(node_id), static_cast<unsigned>(now_ms_()), settled ? 1 : 0,
                            static_cast<unsigned>(rc.sets_sent), static_cast<unsigned>(rc.sets_received),
                            static_cast<unsigned>(rc.sets_malformed), static_cast<unsigned>(rc.view_changes),
                            static_cast<unsigned>(fenced_replies), static_cast<unsigned>(rc.checkpoints_persisted),
                            static_cast<unsigned>(rc.checkpoints_restored), static_cast<unsigned>(ck_persist::g_writes),
                            static_cast<unsigned>(ck_persist::g_deferred), static_cast<unsigned>(ck_persist::g_write_errors), ck_persist::g_last_error,
                            static_cast<unsigned>(nvs_st.used_entries), static_cast<unsigned>(nvs_st.available_entries));
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
#if CONFIG_POT_RSSI_TEE
        if (g_print_gate != nullptr) xSemaphoreGive(g_print_gate);
#endif
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
#if CONFIG_POT_WASM
// ---------------------------------------------------------------------------------------------
// M7's experiment, in a running node: M9's kernel in the sandbox, on a task of its own at the lowest
// priority on core 1, while the node keeps its duties. The cost to those duties is read off the link
// records meanwhile (tools/m7_bench.py). One guest at a time: the sandbox's fuel counter is global.
// ---------------------------------------------------------------------------------------------
namespace wasm_rt {

std::atomic<bool> g_busy{false};
uint32_t g_samples = 0;
uint32_t g_runs = 0;

void task(void*) {
    const uint32_t args[3] = {0x1234u, 7u, g_samples};
    const int64_t n0 = esp_timer_get_time();
    const uint32_t native = mc_hits(args[0], args[1], args[2], nullptr, nullptr);
    const int64_t n1 = esp_timer_get_time();
    WasmSandbox sb;
    const char* why = sb.load(wasm_modules::k_mc, sizeof(wasm_modules::k_mc), WasmLimits{0, 8 * 1024});
    std::printf("{\"t\":\"wasm_run\",\"phase\":\"start\",\"samples\":%u,\"runs\":%u,\"native_us\":%lld,\"load\":\"%s\"}\n",
                static_cast<unsigned>(g_samples), static_cast<unsigned>(g_runs), static_cast<long long>(n1 - n0),
                why == nullptr ? "ok" : why);
    for (uint32_t i = 0; why == nullptr && i < g_runs; ++i) {
        const int64_t t0 = esp_timer_get_time();
        const WasmCallResult r = sb.call("mc_hits", args, 3, 4000000000ull, 12 * 1024);
        const int64_t t1 = esp_timer_get_time();
        std::printf("{\"t\":\"wasm_run\",\"run\":%u,\"us\":%lld,\"same\":%s,\"error\":\"%s\"}\n",
                    static_cast<unsigned>(i), static_cast<long long>(t1 - t0), r.value == native ? "true" : "false",
                    r.error == nullptr ? "none" : r.error);
    }
    std::printf("{\"t\":\"wasm_run\",\"phase\":\"done\",\"stack_free_min_b\":%u}\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    sb.unload();
    g_busy.store(false);
    vTaskDelete(nullptr);
}

bool start(uint32_t samples, uint32_t runs) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    g_samples = samples;
    g_runs = runs;
    if (xTaskCreatePinnedToCore(&task, "pot_wasm", 16384, nullptr, 1, nullptr, portNUM_PROCESSORS > 1 ? 1 : 0) != pdPASS) {
        g_busy.store(false);
        return false;
    }
    return true;
}

}  // namespace wasm_rt

// ---------------------------------------------------------------------------------------------
// M7: the guest task -- where every guest actor's code runs (pot/guest.hpp). One task, so one guest at a
// time (the sandbox's fuel counter is global), at the lowest priority on core 1 as the experiment
// measured, with a 16 KB stack: each interpreted op nests a C frame on Xtensa. The actors on the link
// task hand it work and wake it; it never touches the node.
// ---------------------------------------------------------------------------------------------
namespace guest_rt {

TaskHandle_t g_task = nullptr;

void wake(void*) {
    if (g_task != nullptr) xTaskNotifyGive(g_task);
}

void task(void*) {
    for (;;) {
        // A missed wake costs at most 100 ms: service() also finds work on its own.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        while (guest_runtime().service()) {
        }
    }
}

void start(const uint8_t* ca_pub) {
    guest_runtime().set_ca(ca_pub);
    // Let the idle task in every ~50,000 units of fuel (~16 ms measured): the task watchdog watches it.
    WasmYield y;
    y.fn = [](void*) { vTaskDelay(1); };
    wasm_set_yield(y);
    if (xTaskCreatePinnedToCore(&task, "pot_guest", 16384, nullptr, 1, &g_task, portNUM_PROCESSORS > 1 ? 1 : 0) !=
        pdPASS) {
        ESP_LOGE(kTag, "guest: no task, so no guest will run");
        return;
    }
    guest_runtime().set_waker(&wake, nullptr);
    // M7's [MEASURE]: each run's time, and the heap a fresh instance takes -- all of it, PSRAM included,
    // since wasm3 asks malloc for the guest's 64 KiB page and malloc puts large blocks there.
    guest_runtime().set_meter([](void*) { return static_cast<uint32_t>(esp_timer_get_time()); },
                              [](void*) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }, nullptr);
    ESP_LOGI(kTag, "guest: %u guest(s) in the image, author checks %s", static_cast<unsigned>(guest_library().count()),
             ca_pub != nullptr ? "under the cluster CA" : "impossible (not enrolled): no guest will run");
}

}  // namespace guest_rt
#endif

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
#if CONFIG_POT_WASM
    // M7: and every guest's author signature, so an image carrying a guest nobody certified is refused
    // whole, before it is written. Static: this worker's 4 KB stack also holds the Ed25519 check.
    EXT_RAM_BSS_ATTR static DeployImage img;
    const char* why = nullptr;
    if (j.e == CertError::Ok && parse_image(j.img, j.n, img, &why, kKeepNone) && img.guest_count > 0) {
        uint8_t which = 0;
        j.e = guest_image_check(img, g_id_boot.ca_pub, &which);
        if (j.e != CertError::Ok) ESP_LOGW(kTag, "deploy: guest %u's author signature failed", static_cast<unsigned>(which));
    }
#endif
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
//   POT! write <node hex> <int> outside   ... marked as from outside the cell (M13): the owner applies the package's
//                                 outside rules, whatever this node thinks -- the presenter's check bypassed
//   POT! replay [flip]            re-send the last recorded WRITE unchanged (or with one payload bit flipped)
bool handle_test(const char* line, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) return false;
    std::memcpy(buf, line, len);
    buf[len] = 0;
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = 0;
    unsigned node = 0;
    int value = 0;
    unsigned secs = 0;
    if (std::sscanf(buf, "POT! identify %u", &secs) == 1) {
        // M20: blink blue for `secs` seconds (at most 300; 0 stops it) so a person can find this board.
        if (secs > 300) secs = 300;
        status_led::g_identify_until_ms = secs == 0 ? 0 : now_ms_() + secs * 1000u;
        std::printf("{\"t\":\"test\",\"cmd\":\"identify\",\"seconds\":%u}\n", secs);
        return true;
    }
    if (std::sscanf(buf, "POT! txpower %d", &value) == 1) {
        // 2026-10-22 bench instrument (an orientation experiment): the radio's maximum transmit power in 0.25 dBm,
        // 8..84 (2-20 dBm). Not saved: a restart returns to the driver's default. Prints what the driver took.
        int8_t actual = 0;
        const bool ok = value >= 8 && value <= 84 && espnow_set_tx_power(static_cast<int8_t>(value), actual);
        std::printf("{\"t\":\"test\",\"cmd\":\"txpower\",\"asked_qdbm\":%d,\"ok\":%d,\"qdbm\":%d}\n", value, ok ? 1 : 0,
                    static_cast<int>(actual));
        return true;
    }
    if (std::sscanf(buf, "POT! write %x %d", &node, &value) == 2) {
        const bool outside = std::strstr(buf, " outside") != nullptr;
        lock_node();
        const uint16_t id = g_node->request_write(static_cast<uint16_t>(node), path_hash("act/setpoint"),
                                                  Value::of_i32(value), outside);
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"write\",\"to\":%u,\"value\":%d,\"outside\":%d,\"msg_id\":%u}\n", node,
                    value, outside ? 1 : 0, static_cast<unsigned>(id));
        return true;
    }
    // M10 test instrument: "POT! read <path hash>" -- what this board holds for a path (its owner, quality,
    // value, age), and, when someone else owns it, a fresh READ from that owner, so the next read shows
    // the owner's value (refresh on read). Read twice to see where a portable actor's values come from.
    unsigned long read_hash = 0;
    if (std::sscanf(buf, "POT! read %lu", &read_hash) == 1) {
        lock_node();
        const uint32_t h = static_cast<uint32_t>(read_hash);
        const NsEntry* e = g_node->ns().find(h);
        const uint16_t owner = e != nullptr ? e->owner_node : 0;
        Reading r;
        g_node->read(h, r);
        uint16_t asked = 0;
        if (owner != 0 && owner != g_node->config().node_id) asked = g_node->request_read(owner, h);
        xSemaphoreGive(g_mutex);
        uint32_t u = 0;
        float fv = 0;
        const bool has = r.value.as_u32(u);
        const bool hasf = r.value.as_f32(fv);  // M19: a summary's max/min/sum/avg is an f32
        std::printf("{\"t\":\"test\",\"cmd\":\"read\",\"hash\":%u,\"owner\":%u,\"quality\":\"%s\",\"value\":%s%u,"
                    "\"f32\":%.3f,\"unit\":%u,\"age_ms\":%u,\"ts\":%u,\"asked\":%u}\n",
                    static_cast<unsigned>(h), static_cast<unsigned>(owner), quality_str(r.quality), has ? "" : "-",
                    has ? static_cast<unsigned>(u) : 0u, hasf ? static_cast<double>(fv) : 0.0,
                    static_cast<unsigned>(r.unit), static_cast<unsigned>(r.age_ms),
                    static_cast<unsigned>(r.timestamp_ms), static_cast<unsigned>(asked));
        return true;
    }
    // M19 test instrument: "POT! refer <via, hex> <path hash>" -- a referral (Node::request_referral): the path lives
    // in another cell, and <via> (its foreman, in this board's upper cell) answers for it. Then POT! read shows it.
    unsigned long ref_via = 0, ref_hash = 0;
    if (std::sscanf(buf, "POT! refer %lx %lu", &ref_via, &ref_hash) == 2) {
        lock_node();
        const uint16_t id = g_node->request_referral(static_cast<uint16_t>(ref_via), static_cast<uint32_t>(ref_hash));
        xSemaphoreGive(g_mutex);
        std::printf("{\"t\":\"test\",\"cmd\":\"refer\",\"via\":%u,\"hash\":%u,\"msg_id\":%u}\n",
                    static_cast<unsigned>(ref_via), static_cast<unsigned>(ref_hash), static_cast<unsigned>(id));
        return true;
    }
#if CONFIG_POT_WASM
    // M7 experiment: "POT! wasm <samples> <runs>" -- M9's kernel in the sandbox, <runs> times, while the
    // node runs (wasm_rt above). Answers {"t":"wasm_run",...} lines as it goes.
    unsigned long w_samples = 0, w_runs = 0;
    if (std::sscanf(buf, "POT! wasm %lu %lu", &w_samples, &w_runs) == 2) {
        const bool ok = wasm_rt::start(static_cast<uint32_t>(w_samples), static_cast<uint32_t>(w_runs));
        std::printf("{\"t\":\"test\",\"cmd\":\"wasm\",\"started\":%d}\n", ok ? 1 : 0);
        return true;
    }
#endif
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
        chan_store::relay_save(relay_on != 0);  // M12: kept across reboots
        std::printf("{\"t\":\"test\",\"cmd\":\"relay\",\"relay\":%u,\"saved\":1}\n", relay_on != 0 ? 1u : 0u);
        return true;
    }
    unsigned hog_ms = 0;
    if (std::sscanf(buf, "POT! hog %u", &hog_ms) == 1) {
        // POT! hog <ms>: spin on core 0 above every task but Wi-Fi's own, never yielding -- what C's wedged
        // Wi-Fi task did for 6 hours (M0-LOG session 46). Past CONFIG_ESP_TASK_WDT_TIMEOUT_S the task
        // watchdog resets the chip (CONFIG_ESP_TASK_WDT_PANIC); the next boot's "fw: running" line says so.
        static uint32_t s_hog_ms;
        s_hog_ms = hog_ms;
        std::printf("{\"t\":\"test\",\"cmd\":\"hog\",\"ms\":%u,\"core\":0}\n", hog_ms);
        std::fflush(stdout);
        xTaskCreatePinnedToCore(
            [](void*) {
                const int64_t end = esp_timer_get_time() + static_cast<int64_t>(s_hog_ms) * 1000;
                while (esp_timer_get_time() < end) {
                }
                std::printf("{\"t\":\"test\",\"cmd\":\"hog\",\"done\":1}\n");
                vTaskDelete(nullptr);
            },
            "pot_hog", 2048, nullptr, 20, nullptr, 0);
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
    if (wifi::handle_console(line, len)) return;      // M13: the same rule
#if CONFIG_POT_PRESENT
    if (present::handle_console(line, len)) return;   // M13: the broker's login, the same rule
#endif
    if (pocket::handle_console(line, len)) return;    // M13.1: the access point's name and password, the same rule
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
    // The text the console reads, a line at a time; with M20's frame link on this UART, only what is not a frame.
    struct Lines {
        char line[400];
        size_t n;
        bool overflow;
    };
    static Lines lines{};
    auto on_text = [](void* ctx, const uint8_t* t, size_t len) {
        auto* l = static_cast<Lines*>(ctx);
        for (size_t i = 0; i < len; ++i) {
            const uint8_t c = t[i];
            if (c == '\n') {
                if (!l->overflow) handle(l->line, l->n);
                std::memset(l->line, 0, l->n);  // M6.1: a line may have carried a Wi-Fi password; keep no copy
                l->n = 0;
                l->overflow = false;
            } else if (l->n < sizeof(l->line)) {
                l->line[l->n++] = static_cast<char>(c);
            } else {
                l->overflow = true;  // longer than any command: drop the whole line, never a prefix of it
            }
        }
    };
#if CONFIG_POT_SERIAL_LINK
    serial_console_attach(uart);  // M20: frames over the board's own USB cable, beside the logs
#endif
    static uint8_t buf[64];
    for (;;) {
        serve_jobs(0);
        // 10 ms, so a queued crypto job waits at most that long behind an idle console. The first byte, then what is
        // already buffered: uart_read_bytes waits for its whole length (see serial_port.cpp's reader, 2026-10-25).
        int got = uart_read_bytes(static_cast<uart_port_t>(uart), buf, 1, pdMS_TO_TICKS(10));
        if (got <= 0) continue;
        size_t more = 0;
        if (uart_get_buffered_data_len(static_cast<uart_port_t>(uart), &more) == ESP_OK && more > 0) {
            const int m = uart_read_bytes(static_cast<uart_port_t>(uart), buf + 1, more < sizeof(buf) - 1 ? more : sizeof(buf) - 1, 0);
            if (m > 0) got += m;
        }
#if CONFIG_POT_SERIAL_LINK
        serial_console_feed(buf, static_cast<size_t>(got), on_text, &lines);
#else
        on_text(&lines, buf, static_cast<size_t>(got));
#endif
    }
}

void start_console(BaseType_t core) {
    g_jobs = xQueueCreateStatic(1, sizeof(HeavyJob), g_jobs_storage, &g_jobs_buf);
    g_job_done = xSemaphoreCreateBinaryStatic(&g_job_done_buf);
    xTaskCreateStaticPinnedToCore(console_task, "pot_console", sizeof(g_console_stack) / sizeof(StackType_t),
                                  nullptr, 2, g_console_stack, &g_console_tcb, core);
}

}  // namespace trust_rt

// ---------------------------------------------------------------------------------------------
// M11: firmware over the cell. A signed native image streams into the app slot not booted from
// (pot_deploy's FwReceiver and EspOtaSink), is checked -- the deploy key's signature over (counter,
// length, SHA-512), then ESP-IDF's own image check -- and boots on trial. ESP-IDF's bootloader
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) boots it PENDING_VERIFY; this node confirms it only once it
// has sent the trial's heartbeats with at least one radio peer alive, and rolls it back itself if that
// has not happened by CONFIG_POT_FW_TRIAL_TIMEOUT_MS. A crash before then is a reset, and the
// bootloader rolls back an unconfirmed image at the next reset by itself.
//
// A request for another node is passed on to it unchanged, and its REPLY passed back: the board on the
// cable is a proxy, so the image is never stored on the way and the host drives the rollout.
// ---------------------------------------------------------------------------------------------
namespace fw_rt {

constexpr const char* kNs = "pot_fw";  // floor: highest confirmed; run: counter running; pend: on trial
constexpr uint32_t kTrialTimeoutMs = CONFIG_POT_FW_TRIAL_TIMEOUT_MS;

EspOtaSink g_sink;
bool g_sink_ok = false;
FwState g_state = FwState::Factory;
uint32_t g_running = 0;
uint32_t g_floor = 0;
uint32_t g_pending = 0;
bool g_committed = false;  // an image is activated and the reboot into it is scheduled

bool verify(void*, uint32_t counter, uint32_t len, const uint8_t sha[64], const uint8_t* tr) {
    // Called from commit(), which runs on the crypto worker: the Ed25519 check belongs there.
    const CertError e = fw_trailer_check(trust_rt::g_id_boot.ca_pub, counter, len, sha, tr, kFwTrailerLen);
    if (e != CertError::Ok) {
        ESP_LOGW(kTag, "fw: refused, signature check failed (%s)", cert_error_name(e));
        return false;
    }
    return true;
}
FwReceiver g_rx(g_sink, &verify, nullptr);

uint32_t nvs_u32(const char* key) {
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}
void nvs_put(uint32_t floor, uint32_t run, uint32_t pend) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(kTag, "fw: could not persist the firmware state");
        return;
    }
    nvs_set_u32(h, "floor", floor);
    nvs_set_u32(h, "run", run);
    nvs_set_u32(h, "pend", pend);
    nvs_commit(h);
    nvs_close(h);
}

// M19: firmware catch-up. A board keeps, with each image it takes, that image's length and the owner's signed trailer
// ("p..." while on trial, "r..." once confirmed), so it can pass the image on: a board of a cell the PC's cable does
// not reach gets the firmware the way it gets the package -- a confirmed board announces its counter to its peers, a
// peer with an older one asks, and the holder sends its own running image (FW_BEGIN / CHUNK / COMMIT, read back from
// its partition). The receiver checks the owner's signature and its anti-downgrade floor exactly as for an image from
// the PC, and runs the same trial. Every board runs one image (M12), so any confirmed board's image is the cell's.
uint8_t g_run_trailer[kFwTrailerLen] = {};
uint32_t g_run_len = 0;
bool g_run_have = false;  // the running image's trailer and length are known (it came over the cell since M19)

void save_image(const char* which, uint32_t counter, uint32_t len, const uint8_t* trailer) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    char k[8];
    std::snprintf(k, sizeof k, "%sctr", which);
    nvs_set_u32(h, k, counter);
    std::snprintf(k, sizeof k, "%slen", which);
    nvs_set_u32(h, k, len);
    std::snprintf(k, sizeof k, "%str", which);
    nvs_set_blob(h, k, trailer, kFwTrailerLen);
    nvs_commit(h);
    nvs_close(h);
}
bool load_image(const char* which, uint32_t want_counter, uint32_t& len, uint8_t* trailer) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
    char k[8];
    uint32_t ctr = 0;
    size_t n = kFwTrailerLen;
    std::snprintf(k, sizeof k, "%sctr", which);
    bool ok = nvs_get_u32(h, k, &ctr) == ESP_OK && ctr == want_counter;
    std::snprintf(k, sizeof k, "%slen", which);
    ok = ok && nvs_get_u32(h, k, &len) == ESP_OK;
    std::snprintf(k, sizeof k, "%str", which);
    ok = ok && nvs_get_blob(h, k, trailer, &n) == ESP_OK && n == kFwTrailerLen;
    nvs_close(h);
    return ok;
}

// Why the chip last started: a task watchdog here is a wedge the watchdog cleared (M0-LOG session 46).
const char* reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        default: return "other";
    }
}

const char* state_str(FwState s) {
    return s == FwState::Confirmed ? "confirmed" : s == FwState::OnTrial ? "ON TRIAL" : "factory";
}

struct Push {  // M19: one firmware catch-up being sent, to one peer
    uint16_t peer = 0;      // 0 = none
    uint16_t awaiting = 0;  // the request's msg_id
    uint8_t step = 0;       // 1 BEGIN, 2 CHUNK, 3 COMMIT
    uint8_t tries = 0;
    uint32_t offset = 0;
    uint32_t started_ms = 0;
};
Push g_push;
// M19: when the last BEGIN or CHUNK of a receive arrived. A transfer cut off half-way -- the sender out of reach, as on
// the bench when a board dropped out of the cable's cells -- kept the receiver busy until a reboot, and with it every
// catch-up: dropped after kFwRxStaleMs, as the package agent drops a stale receive.
constexpr uint32_t kFwRxStaleMs = 15000;
uint32_t g_rx_last_ms = 0;
bool busy() { return g_state == FwState::OnTrial || g_committed || g_push.peer != 0; }
bool receiving() { return g_rx.in_progress(); }
uint32_t received() { return g_rx.received(); }
bool on_trial() { return g_state == FwState::OnTrial; }

// Before any task starts.
void boot() {
    g_floor = nvs_u32("floor");
    const uint32_t run = nvs_u32("run");
    g_pending = nvs_u32("pend");
    const esp_partition_t* part = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    const bool has_state = part != nullptr && esp_ota_get_state_partition(part, &st) == ESP_OK;
    if (part != nullptr && part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        g_state = FwState::Factory;
        g_running = 0;
    } else if (has_state && st == ESP_OTA_IMG_PENDING_VERIFY) {
        g_state = FwState::OnTrial;
        g_running = g_pending;
    } else {
        g_state = FwState::Confirmed;
        g_running = run;
    }
    if (g_state != FwState::OnTrial && g_pending != 0) {
        ESP_LOGW(kTag, "fw: image counter %u did not pass its trial - ROLLED BACK",
                 static_cast<unsigned>(g_pending));
        g_pending = 0;
        nvs_put(g_floor, g_running, 0);
    }
    g_sink_ok = g_sink.init();
    if (g_state == FwState::Confirmed && g_running != 0) {
        g_run_have = load_image("r", g_running, g_run_len, g_run_trailer);  // M19
    }
    g_rx.set_floor(g_floor);
    g_rx.set_busy(busy());
    ESP_LOGI(kTag, "fw: running %s (%s), counter %u, floor %u, %s; spare slot %s, %u KB; reset: %s",
             part != nullptr ? part->label : "?", esp_app_get_description()->version,
             static_cast<unsigned>(g_running), static_cast<unsigned>(g_floor), state_str(g_state),
             g_sink_ok ? g_sink.target()->label : "none", static_cast<unsigned>(g_sink.capacity() / 1024),
             reset_name(esp_reset_reason()));
}

size_t reply_with(FwStatus status, uint16_t node, uint8_t* reply, size_t cap) {
    FwReply r{};
    r.status = status;
    r.node = node;
    r.received = g_rx.received();
    r.running = g_running;
    r.floor = g_floor;
    r.state = g_state;
    std::strncpy(r.version, esp_app_get_description()->version, kFwVersionLen - 1);
    return store_fw_reply(r, reply, cap);
}

struct CommitJob {
    FwStatus status;
};
void commit_job(void* a) {
    CommitJob& j = *static_cast<CommitJob*>(a);
    const uint32_t t0 = now_ms_();
    j.status = g_rx.commit();
    if (j.status == FwStatus::Ok) {
        g_pending = g_rx.counter();
        nvs_put(g_floor, g_running, g_pending);
        save_image("p", g_pending, g_rx.image_len(), g_rx.trailer());  // M19: to pass it on once it is confirmed
    }
    ESP_LOGI(kTag, "fw: commit of counter %u: %s (%u ms)", static_cast<unsigned>(g_rx.counter()),
             fw_status_str(j.status), static_cast<unsigned>(now_ms_() - t0));
}

// ---- the proxy: one request passed on at a time --------------------------------------------------
struct Proxy {
    uint16_t awaiting = 0;  // msg_id of the request sent on; 0 = none
    uint16_t target = 0;
    uint16_t requester = 0;
    uint16_t requester_msg = 0;
};
Proxy g_proxy;

size_t on_fw(uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len, uint8_t* reply,
             size_t cap) {
    const uint16_t self = g_node->config().node_id;
    if (p == nullptr || len < kFwTargetLen) return reply_with(FwStatus::Malformed, self, reply, cap);
    const uint16_t target = static_cast<uint16_t>(p[0] | (p[1] << 8));
    if (target != self) {
        // Passed on unchanged. A newer request replaces one still waiting: the host has given up on it.
        const uint16_t m = g_node->send_deploy(target, op, p, len);
        if (m == 0) return reply_with(FwStatus::Unreachable, target, reply, cap);
        g_proxy = Proxy{m, target, from, msg_id};
        return 0;  // answered in on_result()
    }
    if (!g_sink_ok) return reply_with(FwStatus::StoreFailed, self, reply, cap);
    FwStatus s = FwStatus::Malformed;
    if (op == kOpFwStatus) {
        s = FwStatus::Ok;
    } else if (op == kOpFwBegin) {
        FwBegin b{};
        if (load_fw_begin(p, len, b)) {
            g_rx.set_busy(busy() || deploy_rt::g_agent->pushing() || deploy_rt::g_agent->reboot_scheduled());
            s = g_rx.begin(b);
            g_rx_last_ms = now_ms_();  // M19
            ESP_LOGI(kTag, "fw: begin counter %u, %u B: %s", static_cast<unsigned>(b.counter),
                     static_cast<unsigned>(b.image_len), fw_status_str(s));
        }
    } else if (op == kOpFwChunk) {
        FwChunk c{};
        if (load_fw_chunk(p, len, c)) {
            s = g_rx.chunk(c);  // here: one flash write and a hash update
            g_rx_last_ms = now_ms_();  // M19
        }
    } else if (op == kOpFwCommit) {
        CommitJob j{FwStatus::NotStarted};
        trust_rt::run_heavy(nullptr, &commit_job, &j);  // the signature, then esp_ota_end's image check
        s = j.status;
        if (s == FwStatus::Ok) {
            g_committed = true;
            deploy_rt::schedule_reboot();
        }
    }
    return reply_with(s, self, reply, cap);
}

// ---- M19: the catch-up's push --------------------------------------------------------------------
constexpr uint32_t kFwPath = path_hash("potluck:sys/firmware/v1");  // announce: 1, counter u32; ask: 2, counter u32
constexpr uint32_t kFwFirstAnnounceMs = 45000;
constexpr uint32_t kFwAnnounceMs = 60000;
constexpr uint32_t kFwAskCooldownMs = 180000;  // a transfer and a trial take two or three minutes
constexpr uint32_t kFwPushMaxMs = 600000;
uint32_t g_next_announce_ms = kFwFirstAnnounceMs;
uint32_t g_asked_ms = 0;
uint32_t g_pushes = 0, g_asks = 0;

void push_send() {
    EXT_RAM_BSS_ATTR static uint8_t buf[kFwBeginLen + kFwChunkMax + 16];  // the link task's: PSRAM, off its stack
    size_t n = 0;
    uint8_t op = 0;
    if (g_push.step == 1) {
        FwBegin b{g_push.peer, g_run_len, g_running, g_run_trailer};
        n = store_fw_begin(b, buf, sizeof buf);
        op = kOpFwBegin;
    } else if (g_push.step == 2) {
        EXT_RAM_BSS_ATTR static uint8_t data[kFwChunkMax];
        const uint32_t left = g_run_len - g_push.offset;
        const uint16_t len = static_cast<uint16_t>(left < kFwChunkMax ? left : kFwChunkMax);
        const esp_partition_t* part = esp_ota_get_running_partition();
        if (part == nullptr || esp_partition_read(part, g_push.offset, data, len) != ESP_OK) {
            ESP_LOGW(kTag, "fw: catch-up to 0x%04x abandoned: cannot read the running image", g_push.peer);
            g_push = Push{};
            return;
        }
        FwChunk c{g_push.peer, g_push.offset, len, data};
        n = store_fw_chunk(c, buf, sizeof buf);
        op = kOpFwChunk;
    } else {
        n = store_fw_target(g_push.peer, buf, sizeof buf);
        op = kOpFwCommit;
    }
    g_push.awaiting = n != 0 ? g_node->send_deploy(g_push.peer, op, buf, static_cast<uint16_t>(n)) : 0;
    if (g_push.awaiting == 0) {
        ESP_LOGW(kTag, "fw: catch-up to 0x%04x abandoned: the peer is not reachable", g_push.peer);
        g_push = Push{};
    }
}

void push_result(bool timed_out, const uint8_t* p, uint16_t len) {
    FwReply r{};
    const bool answered = !timed_out && p != nullptr && load_fw_reply(p, len, r);
    if (!answered) {
        if (g_push.step == 3) {
            // The commit's checks (the signature, then the image's own) outlast a node's request timeout: the peer
            // is very likely restarting into the image now. Its trial and its next announce tell the rest.
            ESP_LOGI(kTag, "fw: catch-up to 0x%04x sent; its commit outlasted the request timeout", g_push.peer);
            g_push = Push{};
            return;
        }
        if (++g_push.tries > 3) {
            ESP_LOGW(kTag, "fw: catch-up to 0x%04x abandoned at %u B: no answer", g_push.peer,
                     static_cast<unsigned>(g_push.offset));
            g_push = Push{};
            return;
        }
        push_send();  // the same step again
        return;
    }
    if (r.status != FwStatus::Ok) {
        ESP_LOGW(kTag, "fw: catch-up to 0x%04x refused at step %u: %s", g_push.peer, static_cast<unsigned>(g_push.step),
                 fw_status_str(r.status));
        g_push = Push{};
        return;
    }
    g_push.tries = 0;
    if (g_push.step == 1) {
        g_push.step = 2;
    } else if (g_push.step == 2) {
        const uint32_t left = g_run_len - g_push.offset;
        g_push.offset += left < kFwChunkMax ? left : kFwChunkMax;
        if (g_push.offset >= g_run_len) g_push.step = 3;
    } else {
        ESP_LOGI(kTag, "fw: catch-up to 0x%04x committed: %u B, counter %u, in %u ms", g_push.peer,
                 static_cast<unsigned>(g_run_len), static_cast<unsigned>(g_running),
                 static_cast<unsigned>(now_ms_() - g_push.started_ms));
        g_push = Push{};
        return;
    }
    push_send();
}

bool on_cast(uint16_t from, uint32_t path, const uint8_t* a, uint16_t len, uint32_t now) {
    if (path != kFwPath) return false;
    if (a == nullptr || len < 5) return true;
    const uint32_t c = static_cast<uint32_t>(a[1]) | static_cast<uint32_t>(a[2]) << 8 |
                       static_cast<uint32_t>(a[3]) << 16 | static_cast<uint32_t>(a[4]) << 24;
    // The board on the PC's cable is the PC's to update (last, as it carries everyone else's transfer): taking an image
    // by catch-up mid-rollout restarted it under the PC's push to the next board (found on the bench).
    const uint32_t heard = g_host_heard_ms.load();
    const bool pc_cable = heard != 0 && now - heard < 300000u;
    if (a[0] == 1) {
        // A newer confirmed image on a peer: ask for it, unless this board is busy with firmware or a package.
        if (c > g_running && c > g_floor && g_sink_ok && !busy() && !g_rx.in_progress() && !pc_cable &&
            !deploy_rt::g_agent->pushing() && !deploy_rt::g_agent->reboot_scheduled() &&
            (g_asked_ms == 0 || now - g_asked_ms >= kFwAskCooldownMs)) {
            const uint8_t ask[5] = {2, a[1], a[2], a[3], a[4]};
            if (g_node->cast(from, kFwPath, ask, sizeof ask)) {
                g_asked_ms = now == 0 ? 1 : now;
                ++g_asks;
                ESP_LOGI(kTag, "fw: 0x%04x runs counter %u, this board %u: asked for it", from, static_cast<unsigned>(c),
                         static_cast<unsigned>(g_running));
            }
        }
    } else if (a[0] == 2) {
        if (c == g_running && g_run_have && g_state == FwState::Confirmed && g_push.peer == 0 && !g_committed &&
            !deploy_rt::g_agent->pushing()) {
            g_push = Push{from, 0, 1, 0, 0, now};
            ++g_pushes;
            ESP_LOGI(kTag, "fw: catch-up to 0x%04x: counter %u, %u B", from, static_cast<unsigned>(g_running),
                     static_cast<unsigned>(g_run_len));
            push_send();
        }
    }
    return true;
}

void catchup_tick(uint32_t now) {
    if (g_rx.in_progress() && now - g_rx_last_ms > kFwRxStaleMs) {
        ESP_LOGW(kTag, "fw: a receive of counter %u stopped at %u B - abandoned", static_cast<unsigned>(g_rx.counter()),
                 static_cast<unsigned>(g_rx.received()));
        g_rx.abort();
    }
    if (g_push.peer != 0 && now - g_push.started_ms > kFwPushMaxMs) {
        ESP_LOGW(kTag, "fw: catch-up to 0x%04x abandoned: too slow", g_push.peer);
        g_push = Push{};
    }
    if (static_cast<int32_t>(now - g_next_announce_ms) < 0) return;
    g_next_announce_ms = now + kFwAnnounceMs;
    if (!g_run_have || g_state != FwState::Confirmed || g_committed) return;
    const uint8_t ann[5] = {1, static_cast<uint8_t>(g_running), static_cast<uint8_t>(g_running >> 8),
                            static_cast<uint8_t>(g_running >> 16), static_cast<uint8_t>(g_running >> 24)};
    const NodeConfig& cfg = g_node->config();
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& q = g_node->peers().slot(i);
        if (q.state != PeerState::Alive) continue;
        if (cfg.has_trusted_mac && std::memcmp(q.mac, cfg.trusted_mac, cfg.trusted_mac_len) == 0) continue;  // the PC
        g_node->cast(q.node_id, kFwPath, ann, sizeof ann);
    }
}

void on_result(uint16_t peer, uint16_t msg, uint8_t, bool timed_out, const uint8_t* p, uint16_t len) {
    if (g_push.peer != 0 && msg == g_push.awaiting && peer == g_push.peer) {
        push_result(timed_out, p, len);  // M19
        return;
    }
    if (g_proxy.awaiting == 0 || msg != g_proxy.awaiting || peer != g_proxy.target) return;
    const Proxy q = g_proxy;
    g_proxy = Proxy{};
    if (timed_out || p == nullptr || len != kFwReplyLen) {
        uint8_t wire[kFwReplyLen];
        reply_with(FwStatus::Unreachable, q.target, wire, sizeof(wire));
        g_node->send_reply_raw(q.requester, q.requester_msg, wire, sizeof(wire));
        return;
    }
    g_node->send_reply_raw(q.requester, q.requester_msg, p, len);
}

// From link_task, every pass, with the node mutex held.
void tick(uint32_t now) {
    if (g_state != FwState::OnTrial) return;
    const NodeConfig& cfg = g_node->config();
    size_t radio_peers = 0;
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& q = g_node->peers().slot(i);
        if (q.state != PeerState::Alive) continue;
        if (cfg.has_trusted_mac && std::memcmp(q.mac, cfg.trusted_mac, cfg.trusted_mac_len) == 0) continue;  // the cable
        ++radio_peers;
    }
    if (radio_peers > 0 && g_node->tx_tally().beacons >= deploy_rt::kTrialHeartbeats) {
        const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e != ESP_OK) {
            ESP_LOGE(kTag, "fw: could not confirm the image: %s", esp_err_to_name(e));
            return;
        }
        g_running = g_pending;
        if (g_running > g_floor) g_floor = g_running;
        g_pending = 0;
        nvs_put(g_floor, g_running, 0);
        // M19: the image just confirmed is now the one this board can pass on
        g_run_have = load_image("p", g_running, g_run_len, g_run_trailer);
        if (g_run_have) save_image("r", g_running, g_run_len, g_run_trailer);
        g_state = FwState::Confirmed;
        g_rx.set_floor(g_floor);
        g_rx.set_busy(false);
        ESP_LOGI(kTag, "fw: trial passed after %u heartbeats with %u radio peer(s) - counter %u CONFIRMED",
                 static_cast<unsigned>(deploy_rt::kTrialHeartbeats), static_cast<unsigned>(radio_peers),
                 static_cast<unsigned>(g_running));
        return;
    }
    if (now >= kTrialTimeoutMs) {
        ESP_LOGE(kTag, "fw: trial not passed in %u ms (%u radio peer(s)) - rolling back",
                 static_cast<unsigned>(kTrialTimeoutMs), static_cast<unsigned>(radio_peers));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

}  // namespace fw_rt

}  // namespace

namespace {
// M8.2 (PS-2): every frame the node accepted from a known peer. On the link task, lock held.
void rx_sample_hook(void* ctx, const RxSample& s) {
    if (!g_rx_from_radio) return;  // the host's cable, a CAN bus: no radio, no sample
#if CONFIG_POT_RSSI_TEE
    rssi_tee(ctx, s);
#else
    (void)ctx;
#endif
    g_actors.on_rx_sample(s);
}
}  // namespace

namespace board {
void set_led_healthy(const LedConfig& c) { status_led::set_healthy(c); }
}  // namespace board

namespace {
// M8.2 (PS-5): the board services every actor gets. Called from actor ticks, on the link task, the
// same task that renders the LED.
// The four status colours are the board's own (purple updating, yellow searching, red a problem, green
// healthy). An app may show blue or cyan; any colour with its red channel lit (at half the brightest or
// more: purple, yellow, orange, red, and white, which at this LED's brightness looks light purple --
// seen on the bench 2026-10-14) or plain green is drawn blue instead, so no app can look like an
// update or a fault.
bool reserved_hue(uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t top = std::max(r, std::max(g, b));
    if (top == 0) return false;
    const bool R = r * 2 >= top, G = g * 2 >= top, B = b * 2 >= top;
    return R || (G && !B);
}

void led_claim(void*, uint8_t slot, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms) {
    if (reserved_hue(r, g, b)) {
        r = 0;
        g = 0;
        b = 255;
    }
    status_led::g_apps.set(slot, on, r, g, b, period_ms, now_ms_());
}

void led_app(void* ctx, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms) {
    led_claim(ctx, 0, on, r, g, b, period_ms);  // slot 0: every app before led_claim
}

// M9: the background workers -- one task per core at priority 1, above idle (0) and below every task
// the node runs (stats 3, console 2, link 6, Wi-Fi's own far above), so a borrowed core only ever runs
// what nothing else wanted. They take jobs from one queue; a job is refused when every worker is
// already busy or promised, never queued behind one, so a lender's BUSY is immediate. The work must
// call bg_yield every few tens of ms: the task watchdog watches the idle task on both cores
// (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1, 5 s), and a worker that never blocked would starve it.
namespace bg {
struct Job {
    void (*work)(void*);
    void* arg;
};
constexpr UBaseType_t kPriority = 1;
constexpr size_t kStackBytes = 3072;
constexpr uint8_t kWorkers = portNUM_PROCESSORS;
StaticQueue_t g_q_ctl;
uint8_t g_q_storage[kWorkers * sizeof(Job)];
QueueHandle_t g_q = nullptr;
std::atomic<uint8_t> g_taken{0};  // workers busy or promised a job
StackType_t g_stack[kWorkers][kStackBytes / sizeof(StackType_t)];
StaticTask_t g_tcb[kWorkers];

void worker(void*) {
    for (;;) {
        Job j{};
        if (xQueueReceive(g_q, &j, portMAX_DELAY) != pdTRUE) continue;
        j.work(j.arg);
        g_taken.fetch_sub(1);
    }
}
bool submit(void*, void (*work)(void*), void* arg) {
    if (g_q == nullptr) return false;
    uint8_t t = g_taken.load();
    do {
        if (t >= kWorkers) return false;
    } while (!g_taken.compare_exchange_weak(t, static_cast<uint8_t>(t + 1)));
    const Job j{work, arg};
    if (xQueueSend(g_q, &j, 0) != pdTRUE) {
        g_taken.fetch_sub(1);
        return false;
    }
    return true;
}
void yield(void*) { vTaskDelay(1); }
void start() {
    g_q = xQueueCreateStatic(kWorkers, sizeof(Job), g_q_storage, &g_q_ctl);
    for (uint8_t i = 0; i < kWorkers; ++i) {
        xTaskCreateStaticPinnedToCore(&worker, "pot_bg", sizeof(g_stack[i]) / sizeof(StackType_t), nullptr, kPriority,
                                      g_stack[i], &g_tcb[i], static_cast<BaseType_t>(i));
    }
}
}  // namespace bg

// M13.1: what the pocket page shows, built on the link task with the node's lock held, once a second.
EXT_RAM_BSS_ATTR pocket::Snapshot g_pocket;
const pocket::Snapshot& pocket_snapshot(uint32_t now) {
    pocket::Snapshot& s = g_pocket;
    std::snprintf(s.system, sizeof(s.system), "%s", sys_system());
    std::snprintf(s.cell_id, sizeof(s.cell_id), "%s", cell_id(nullptr));
    s.self = g_node->config().node_id;
    s.uptime_s = now / 1000;
    s.channel = g_node->channel();
    s.package = deploy_rt::g_state.active != kSlotNone ? deploy_rt::g_agent->running_counter() : 0;
    s.other_cell_frames = g_node->auth_counters().refused;
    size_t at = 0;
    s.actors[0] = '\0';
    auto add = [&](const char* name) {
        const size_t n = std::strlen(name);
        if (n == 0 || at + (at ? 1 : 0) + n >= sizeof(s.actors)) return;
        if (at) s.actors[at++] = ',';
        std::memcpy(s.actors + at, name, n);
        at += n;
        s.actors[at] = '\0';
    };
    for (size_t i = 0; i < g_actors.running(); ++i) add(g_actors.name(i));
#if CONFIG_POT_RECONCILER
    if (g_rec != nullptr) {
        for (size_t i = 0; i < g_rec->actor_count(); ++i) {
            if (g_rec->kind_name(i)[0] != 0 && g_rec->view(i).running) add(g_rec->kind_name(i));
        }
    }
#endif
    s.n_boards = 0;
    s.boards[s.n_boards++] = pocket::Board{s.self, true, true, 0, 0};
    for (size_t i = 0; i < PeerTable::capacity() && s.n_boards < pocket::kMaxBoards; ++i) {
        const PeerLink& p = g_node->peers().slot(i);
        if (p.state == PeerState::Free || p.node_id == 0 || off_radio(p.mac)) continue;
        const bool alive = p.state == PeerState::Alive;
        s.boards[s.n_boards++] =
            pocket::Board{p.node_id, false, alive, alive ? 0u : (now - p.last_rx_ms) / 1000u, p.last_rssi};
    }
    s.n_items = 0;
    for (size_t i = 0; i < deploy_rt::g_outside_n && s.n_items < pocket::kMaxValues; ++i) {
        const deploy_rt::OutsideName& o = deploy_rt::g_outside[i];
        if ((o.flags & kOutsideExposed) == 0) continue;  // what the package shows outside, and nothing else
        Reading r;
        bool local = true;
        if (g_node->read(o.hash, r, &local) != NsError::Ok) continue;
        // Nobody else may be polling it (on the boat no presenter can be placed): refresh the replica from its owner
        // every other pass, for the next snapshot (found by review, M0-LOG session 41).
        if (!local && (now / 1000) % 2 == 0) {
            const NsEntry* e = g_node->ns().find(o.hash);
            if (e != nullptr && e->owner_node != 0 && e->owner_node != s.self) g_node->request_read(e->owner_node, o.hash);
        }
        pocket::Item& it = s.items[s.n_items++];
        std::snprintf(it.name, sizeof(it.name), "%s", o.name);
        it.value[0] = '\0';
        if (r.usable()) r.value.format(it.value, sizeof(it.value));
        std::snprintf(it.quality, sizeof(it.quality), "%s", quality_str(r.quality));
        const char* u = unit_str(r.unit);
        std::snprintf(it.unit, sizeof(it.unit), "%s", u != nullptr ? u : "");
        it.age_ms = r.age_ms;
        it.writable = (o.flags & kOutsideWritable) != 0 && (o.flags & kOutsideSafety) == 0;  // M26
    }
    return s;
}

// M26: a pocket page write, on the link task with the node's lock held: the outside section must let it be written
// (and it is not safety-relevant); this board's own resource is written here, another board's is passed to its owner as
// an outside write -- which that board checks again by its own package. The result, for the page.
const char* pocket_apply_write(const pocket::PendingWrite& w, uint32_t now) {
    const deploy_rt::OutsideName* rule = nullptr;
    for (size_t i = 0; i < deploy_rt::g_outside_n; ++i) {
        const deploy_rt::OutsideName& o = deploy_rt::g_outside[i];
        if (std::strlen(w.name) == o.len && std::memcmp(w.name, o.name, o.len) == 0) rule = &o;
    }
    if (rule == nullptr || (rule->flags & kOutsideWritable) == 0 || (rule->flags & kOutsideSafety) != 0) {
        return "refused: not writable from outside";
    }
    const NsEntry* e = g_node->ns().find(rule->hash);
    if (e == nullptr) return "refused: not on this board";
    // Its type from its value: one never read has none, and is refused rather than guessed (security review
    // 2026-10-26: "1" became a bool for a whole-number resource).
    if (e->value.type == ValueType::None) return "refused: no value yet to know its type";
    Value v;
    if (!pocket::parse_value(e->value.type, w.value, v)) return "refused: not a value of its type";
    const uint16_t self = g_node->config().node_id;
    if (e->is_local(self)) {
        const NsError st = g_node->ns().write_outside(rule->hash, v, now);
        return st == NsError::Ok ? "applied" : ns_error_str(st);
    }
    return g_node->request_write(e->owner_node, rule->hash, v, /*from_outside=*/true) != 0 ? "sent to its board"
                                                                                       : "its board is not reachable";
}

// M13: what the presenting role asks the board (pot/actor.hpp).
bool outside_at(void*, size_t i, uint32_t* hash, uint8_t* flags, const char** name, size_t* len) {
    if (i >= deploy_rt::g_outside_n) return false;
    const deploy_rt::OutsideName& o = deploy_rt::g_outside[i];
    *hash = o.hash;
    *flags = o.flags;
    *name = o.name;
    *len = o.len;
    return true;
}
const char* cell_id(void*) {
    static char id[9] = {};
    if (id[0] == 0 && trust_rt::g_id_boot.enrolled) {
        uint8_t fp[kCaFpLen];
        ca_fingerprint(trust_rt::g_id_boot.ca_pub, fp);
        std::snprintf(id, sizeof(id), "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
    }
    return id[0] != 0 ? id : "00000000";
}
const char* system_name(void*) { return sys_system(); }
bool broker_ca(void*, const uint8_t** der, size_t* len) {
    if (deploy_rt::g_broker_ca_len == 0) return false;
    *der = deploy_rt::g_broker_ca;
    *len = deploy_rt::g_broker_ca_len;
    return true;
}
uint64_t unix_ms(void*) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // Before SNTP answers, the clock counts from 1970 at boot: anything before 2025 is not the network's time.
    if (tv.tv_sec < 1735689600) return 0;
    return static_cast<uint64_t>(tv.tv_sec) * 1000u + static_cast<uint64_t>(tv.tv_usec / 1000);
}

bool receiving_image(void*) { return fw_rt::receiving() || deploy_rt::g_rx.in_progress(); }

const BoardServices g_board_services{nullptr,     &led_app, &bg::submit,  &bg::yield, bg::kWorkers,
                                     &outside_at, &cell_id, &system_name, &unix_ms, &broker_ca, &led_claim,
                                     &receiving_image};

// M12: the inventory record (pot/node_status.hpp). With g_mutex held (on_deploy).
size_t deploy_rt::build_node_status(uint8_t* out, size_t cap, bool cabled) {
    NodeStatus st;
    st.node = g_node->config().node_id;
    st.fw_running = fw_rt::g_running;
    st.fw_floor = fw_rt::g_floor;
    st.fw_state = fw_rt::g_state == FwState::Confirmed ? 1 : fw_rt::g_state == FwState::OnTrial ? 2 : 0;
    const esp_app_desc_t* app = esp_app_get_description();
    std::strncpy(st.fw_version, app->version, kNodeStatusVersionLen);
    std::memcpy(st.elf_sha, app->app_elf_sha256, sizeof(st.elf_sha));
    st.slot = deploy_rt::g_state.active;
    st.pending = deploy_rt::g_state.pending;
    st.trial_boots = deploy_rt::g_state.trial_boots;
    st.pkg_counter = deploy_rt::g_state.active != kSlotNone ? deploy_rt::g_agent->running_counter() : 0;
    st.pkg_floor = deploy_rt::g_state.counter;
    std::memcpy(st.pkg_digest, deploy_rt::g_agent->running_digest(), sizeof(st.pkg_digest));
#if CONFIG_POT_EXTENDER
    st.roles |= kRoleExtenderBuild;
#endif
    if (extender::active()) st.roles |= kRoleExtenderActive;
#if CONFIG_POT_WASM
    st.roles |= kRoleSandbox;
#endif
#if CONFIG_POT_RECONCILER
    st.roles |= kRoleReconciler;
#endif
    if (g_node->relay()) st.roles |= kRoleRelay;
    if (trust_rt::g_id_boot.enrolled) st.roles |= kRoleEnrolled;
    if (cabled) st.roles |= kRoleCabled;
    st.live_applies = deploy_rt::g_live_applies;
    st.ck_writes = ck_persist::g_writes;
    st.uptime_s = now_ms_() / 1000;
    std::strncpy(st.cluster, sys_system(), kNodeStatusMaxCluster);
    size_t at = 0;
    auto add = [&](const char* name) {
        const size_t n = std::strlen(name);
        if (n == 0 || at + (at ? 1 : 0) + n > kNodeStatusMaxActors) return;
        if (at) st.actors[at++] = ',';
        std::memcpy(st.actors + at, name, n);
        at += n;
    };
    st.pinned_running = static_cast<uint8_t>(g_actors.running());
    for (size_t i = 0; i < g_actors.running(); ++i) add(g_actors.name(i));
    bool guest_here = false;
#if CONFIG_POT_RECONCILER
    if (g_rec != nullptr) {
        for (size_t i = 0; i < g_rec->actor_count(); ++i) {
            if (g_rec->kind_name(i)[0] == 0) continue;
            ++st.portable_total;
            if (g_rec->view(i).running) {
                ++st.portable_here;
                add(g_rec->kind_name(i));
                if (std::strcmp(g_rec->kind_name(i), "guest") == 0) guest_here = true;
            }
        }
    }
#endif
    st.actors[at] = 0;
    // M13: the optional modules this image carries, and what each is doing.
    auto module = [&](ModuleId id, ModuleState state, ModuleReason reason) {
        if (st.module_count < kNodeStatusMaxModules) st.modules[st.module_count++] = ModuleStatus{id, state, reason};
    };
#if CONFIG_POT_WASM
    module(ModuleId::Sandbox, guest_here ? ModuleState::Active : ModuleState::Idle,
           guest_here ? ModuleReason::None : ModuleReason::NothingHere);
#endif
    (void)guest_here;
#if CONFIG_POT_EXTENDER
    module(ModuleId::Extender, extender::active() ? ModuleState::Active : ModuleState::Idle,
           extender::active() ? ModuleReason::None : ModuleReason::NoCredentials);
#endif
#if CONFIG_POT_PRESENT
    // M13: the presenting role -- active where held and connected to the broker; elsewhere standby (another board holds
    // it, or nobody can: no network here).
    if (present::holding()) {
        module(ModuleId::Present, present::connected() ? ModuleState::Active : ModuleState::Idle,
               present::connected() ? ModuleReason::None : ModuleReason::NoNetwork);
    } else {
        module(ModuleId::Present, ModuleState::Idle,
               g_node->connected() ? ModuleReason::Standby : ModuleReason::NoNetwork);
    }
#endif
#if CONFIG_POT_POCKET
    {
        const int why = pocket::idle_reason();
        module(ModuleId::Pocket, why == 0 ? ModuleState::Active : why == 3 ? ModuleState::Failed : ModuleState::Idle,
               why == 0   ? ModuleReason::None
               : why == 1 ? ModuleReason::NoCredentials
               : why == 2 ? ModuleReason::OtherConnection
                          : ModuleReason::Error);
    }
#endif
#if CONFIG_POT_WIFI
    if (uplink::kind() == uplink::Kind::Wifi) {
        module(ModuleId::Wifi, wifi::associated() ? ModuleState::Active : ModuleState::Idle,
               wifi::associated() ? ModuleReason::None : ModuleReason::NoNetwork);
    } else {
        module(ModuleId::Wifi, ModuleState::Idle,
               uplink::active() ? ModuleReason::OtherConnection : ModuleReason::NoCredentials);
    }
#endif
    (void)module;
    std::memcpy(st.mac, g_node->config().mac, kNodeStatusMacLen);  // version 3: what its HELLO is signed over
    return store_node_status(st, out, cap);
}

// M6, and M12's live change: the reconciler, made for the first package with a portable actor.
void deploy_rt::rec_create() {
#if CONFIG_POT_RECONCILER
    g_rec = new (g_rec_storage) Reconciler(*g_node);
    g_rec->set_board(&g_board_services);  // PS-5: portable actors get the LED too
    g_rec->set_persist(&ck_persist::g_hook);  // M12: checkpoints that survive a reboot
    if (!g_rec->load(deploy_rt::g_portable, deploy_rt::g_portable_n)) {
        ESP_LOGE(kTag, "reconciler: could not declare the portable actors' resources");
    }
    ESP_LOGI(kTag, "reconciler: %u portable actor(s)", static_cast<unsigned>(g_rec->actor_count()));
#endif
}
}  // namespace

}  // namespace pot

#if CONFIG_POT_SELFTEST
extern "C" int pot_selftest_run(void);
#endif
#if CONFIG_POT_CRYPTO_BENCH || CONFIG_POT_MEM_BENCH
#include "crypto_bench.hpp"
#endif
#if CONFIG_POT_WASM_BENCH
#include "wasm_bench.hpp"
#endif

extern "C" void app_main(void) {
    using namespace pot;

#if CONFIG_POT_WASM_BENCH
    // M7's experiment, alone on the machine like the crypto bench below.
    const int wasm_failed = pot_wasm_bench_run();
    ESP_LOGI(kTag, "wasm bench finished with %d failure(s)", wasm_failed);
    return;
#endif

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
#if CONFIG_POT_EXTENDER || CONFIG_POT_WIFI
    // M6.1: the hotspot owns Wi-Fi when it has credentials, and ESP-NOW only attaches to it; M13: else
    // Potluck's own station, when it has. Their credentials live in NVS, so NVS comes up first.
    nvs_init_once();
    ecfg.attach = uplink::start(saved_channel);
    // The split probe (pot/split_probe.hpp): an extender's channel is its router's, which the cell follows, so it waits
    // three times as long before probing as Potluck's own station does.
    deploy_rt::g_split.set_split_ms(uplink::kind() == uplink::Kind::Extender ? 3 * SplitProbe::kSplitMs
                                                                              : SplitProbe::kSplitMs);
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
    if (uplink::active()) {
        // M6.1: the router owns this radio's channel. Declare what it is on now (0 until the station
        // first associates), never hop, never follow an announcement -- CR-1's station case.
        const uint8_t now_ch = espnow_current_channel();
        if (now_ch >= cfg.channel_lo && now_ch <= cfg.channel_hi) cfg.channel = now_ch;
        cfg.channel_fixed = true;
        // At boot the station is usually still hunting; the link loop tightens this once it associates.
        if (!uplink::associated()) {
            cfg.hb_miss_limit = uplink::hunt_misses();
            cfg.channel_known = false;  // and declares its channel once it is the router's (link loop)
        }
    }
#if CONFIG_POT_CAN
    cfg.admit_on_beacon = true;  // a CAN bus carries no HELLO (§5.3.1)
#endif
#if CONFIG_POT_SERIAL_LINK
    // M5: the host reaches this board over a cable plugged into it, so its HELLO is admitted
    // unsigned. Deploys over that cable still need a signed package.
    cfg.has_trusted_mac = true;
    std::memcpy(cfg.trusted_mac, kHostMac, kMacLen);
    cfg.trusted_mac_len = 4;  // M19: the PC's node and the stubs it hosts (cable_mac)
    cfg.has_wire_mac = true;  // 2026-10-30: nodes the hub bridges -- on the cable, never trusted (bridged_mac)
    std::memcpy(cfg.wire_mac, kBridgedPrefix, sizeof(kBridgedPrefix));
    cfg.wire_mac_len = sizeof(kBridgedPrefix);
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
    // M19/M20 step 1: an enrolled board takes the id its certificate names -- the cluster CA may have assigned one
    // (kAssignedNodeIdMin) to keep ids unique in a large cell. Unenrolled, or a certificate for this same id: as before.
    {
        Identity enrolled;
        if (trust_load(enrolled)) {
            const uint16_t cid = identity_cert_node_id(enrolled);
            if (cid >= kAssignedNodeIdMin && cid != kNodeBroadcast && cid != cfg.node_id) {
                ESP_LOGI(kTag, "node id 0x%04x from this board's certificate (its MAC gives 0x%04x)", cid, cfg.node_id);
                cfg.node_id = cid;
            }
        }
    }

    NodeHal hal;
    hal.ctx = nullptr;
    hal.send = &hal_send;
    hal.add_peer = &hal_add_peer;
    hal.del_peer = &hal_del_peer;  // M19
    hal.now_ms = &hal_now_ms;
    hal.now_us = &hal_now_us;
    hal.free_dram = &hal_free_dram;
    hal.on_event = &hal_on_event;
    hal.on_rx_sample = &rx_sample_hook;  // M8.2: every accepted frame, to the tee and the actors
#if CONFIG_POT_RSSI_TEE
    tee_start();
#endif
    hal.run_heavy = &trust_rt::run_heavy;
#if !CONFIG_POT_RADIO_DISABLE && !CONFIG_POT_CAN
    hal.set_channel = [](void*, uint8_t ch) { espnow_set_channel(ch); };
    if (uplink::active()) hal.set_channel = nullptr;  // M6.1: nothing here may retune the router's radio
#endif
    hal.persist_safe_state_floors = &trust_rt::persist_floors;

    // Placement new into static storage: the node owns the peer table and the histograms, which §6
    // budgets statically, and there is no heap allocation anywhere on this path.
    g_node = new (g_node_storage) Node(cfg, hal);

    // §7.2: declare what this node owns. Six built-ins every board has, so a fresh fleet has
    // something real to read across a link on the day it is switched on.
    const size_t declared = declare_sys_resources(g_node->ns(), cfg.node_id);
    if (extender::active()) declare_extender_resources(g_node->ns(), cfg.node_id);  // sys/ext/*: this board's own
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
    {
        size_t n_kinds = 0;
        const ActorKind* kinds = app::actor_table(&n_kinds);  // M8.2: built-ins plus external components
        g_actors.set_table(kinds, n_kinds);
    }
    // Before the package is loaded: actors are created there, and one created without the board's services never
    // lights the LED (found by Home Assistant on the bench, M0-LOG session 41: B's led_switch on, its light still green).
    g_actor_env.board = &g_board_services;
    deploy_rt::boot(cfg.node_id);
    fw_rt::boot();  // M11
#if CONFIG_POT_FW_TEST_PANIC
    ESP_LOGE(kTag, "fw: CONFIG_POT_FW_TEST_PANIC - this image crashes at boot, on purpose");
    abort();
#endif
#if CONFIG_POT_RECONCILER
    if (deploy_rt::g_portable_n > 0) {
        deploy_rt::rec_create();
        ck_persist::g_boot_reads = false;
    }
#endif
    // M9: one handler for every CALL and CAST -- the reconciler's own messages first, then any actor's
    // (a lender's units). A CALL nobody accepts is refused at once, so the caller is never left waiting.
    g_node->set_call_handler(
        [](void*, uint16_t from, uint16_t msg_id, uint32_t path, const uint8_t* args, uint16_t len) {
#if CONFIG_POT_RECONCILER
            if (g_rec != nullptr && g_rec->on_cast(from, path, args, len)) return true;
#endif
            if (deploy_rt::g_agent->on_cast(from, path, args, len, now_ms_())) return true;  // M12: catch-up
            if (fw_rt::on_cast(from, path, args, len, now_ms_())) return true;  // M19: firmware catch-up
            return g_actors.on_call(from, msg_id, path, args, len) == Actor::kCallAccepted;
        },
        nullptr);
    bg::start();  // M9: before the actors, which may hand it work from their first tick
    // M8.1: start this node's pinned actors, now that the node they publish through exists.
    g_actor_env.node = g_node;
    g_actor_env.board = &g_board_services;  // PS-5
    {
        const char* failed = nullptr;
        g_actors.start(g_actor_env, now_ms_(), &failed);
        if (failed != nullptr) ESP_LOGE(kTag, "actors: %s could not start; dropped", failed);
        for (size_t i = 0; i < g_actors.running(); ++i) ESP_LOGI(kTag, "actors: %s running", g_actors.name(i));
    }
    // M8.2 (PS-3): a read finding no entry may be for an actor output on another node -- the image
    // says which; the candidates are this node and its peers.
    g_node->set_adopt_handler(
        [](void*, uint32_t hash) {
            uint16_t ids[kMaxPeers + 1];
            size_t n = 0;
            ids[n++] = g_node->config().node_id;
            for (size_t i = 0; i < PeerTable::capacity() && n < kMaxPeers + 1; ++i) {
                const PeerLink& q = g_node->peers().slot(i);
                if (q.state != PeerState::Free && std::memcmp(q.mac, kHostMac, kMacLen) != 0) ids[n++] = q.node_id;
            }
            return g_actors.adopt(g_node->ns(), hash, ids, n);
        },
        nullptr);
    {  // M12: always, since a live package change can bring an actor that calls
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
#if CONFIG_POT_WASM
    // M7: before any task runs a guest actor; a node that is not enrolled runs none.
    guest_rt::start(trust_rt::g_id_boot.enrolled ? trust_rt::g_id_boot.ca_pub : nullptr);
#endif
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
    if (chan_store::relay_load() != 0 && !g_node->relay()) {
        g_node->set_relay(true);  // M12: `POT! relay 1` was saved
        ESP_LOGI(kTag, "relay: on, as saved");
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
#if CONFIG_POT_POCKET
    // M13.1: the pocket page, after the radio: it adds an access point beside the station ESP-NOW rides on.
    if (espnow_up() && pocket::start(extender::active())) {
        declare_pocket_resources(g_node->ns(), cfg.node_id);  // sys/pocket/*: this board's own, where the page is up
    }
#endif

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

