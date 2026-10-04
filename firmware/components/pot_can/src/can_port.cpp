// TWAI glue. Two facts from the ESP-IDF v6 driver shape everything here, both read from its source:
//
// 1. twai_node_transmit() queues a POINTER to the twai_frame_t, not a copy (esp_twai_onchip.c,
//    _node_queue_tx: xQueueSend(tx_mount_queue, &frame, ...)). The frame and its data must stay
//    valid until on_tx_done names it. So frames live in a static pool, freed from that callback.
// 2. A received frame can only be read inside on_rx_done (twai_node_receive_from_isr refuses
//    elsewhere). So the ISR copies it into a queue and the link task does the rest.
//
// Arbitration loss is reported through on_error's arb_lost flag; the controller retries such a frame
// by itself. Counting them is how M4 shows SAFE_STATE winning the bus without an oscilloscope.

#include "pot/can_port.hpp"

#include <cstring>
#include <new>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "pot/frame.hpp"

namespace pot {

namespace {

constexpr const char* kTag = "pot.can";
constexpr size_t kPool = kCanMaxSegments + 8;  // one whole message plus a few single frames
constexpr size_t kRxQueue = 128;
constexpr size_t kDoneQueue = 16;

struct TxSlot {
    twai_frame_t f;
    uint8_t data[8];
    bool last;            // the final CAN frame of its Potluck frame
    bool safe_state;      // a SAFE_STATE, timed from submit to done
    uint32_t submit_us;
    uint8_t dst_mac[6];
    volatile bool busy;
};

struct RxItem {
    CanFrame f;
    uint32_t us;
};

struct DoneItem {
    uint8_t mac[6];
    bool ok;
    uint32_t us;
};

TxSlot g_pool[kPool];
portMUX_TYPE g_pool_mux = portMUX_INITIALIZER_UNLOCKED;
twai_node_handle_t g_node = nullptr;
QueueHandle_t g_rxq = nullptr;
QueueHandle_t g_doneq = nullptr;
CanPortStats g_stats{};
CanReassembler* g_rx = nullptr;
alignas(CanReassembler) uint8_t g_rx_storage[sizeof(CanReassembler)];
uint8_t g_my_alias = 0;
uint16_t g_my_node = 0;
bool g_up = false;

bool IRAM_ATTR on_tx_done(twai_node_handle_t, const twai_tx_done_event_data_t* e, void*) {
    BaseType_t woke = pdFALSE;
    for (TxSlot& s : g_pool) {
        if (&s.f == e->done_tx_frame) {
            if (e->is_tx_success) {
                ++g_stats.tx_ok;
                if (s.safe_state) {
                    const uint32_t lat = static_cast<uint32_t>(esp_timer_get_time()) - s.submit_us;
                    ++g_stats.safe_state_sent;
                    g_stats.safe_state_last_us = lat;
                    if (lat > g_stats.safe_state_max_us) g_stats.safe_state_max_us = lat;
                    if (g_stats.safe_state_min_us == 0 || lat < g_stats.safe_state_min_us) {
                        g_stats.safe_state_min_us = lat;
                    }
                }
            } else {
                ++g_stats.tx_fail;
            }
            if (s.last && g_doneq != nullptr) {
                DoneItem d{};
                std::memcpy(d.mac, s.dst_mac, 6);
                d.ok = e->is_tx_success;
                d.us = static_cast<uint32_t>(esp_timer_get_time());
                xQueueSendFromISR(g_doneq, &d, &woke);
            }
            portENTER_CRITICAL_ISR(&g_pool_mux);
            s.busy = false;
            portEXIT_CRITICAL_ISR(&g_pool_mux);
            break;
        }
    }
    return woke == pdTRUE;
}

bool IRAM_ATTR on_rx_done(twai_node_handle_t h, const twai_rx_done_event_data_t*, void*) {
    RxItem it{};
    twai_frame_t rx{};
    rx.buffer = it.f.data;
    rx.buffer_len = sizeof(it.f.data);
    if (twai_node_receive_from_isr(h, &rx) != ESP_OK) {
        return false;
    }
    it.f.id = rx.header.id;
    it.f.dlc = static_cast<uint8_t>(rx.header.dlc > 8 ? 8 : rx.header.dlc);
    it.us = static_cast<uint32_t>(esp_timer_get_time());
    ++g_stats.rx_frames;
    BaseType_t woke = pdFALSE;
    if (xQueueSendFromISR(g_rxq, &it, &woke) != pdTRUE) {
        ++g_stats.rx_overflow;
    }
    return woke == pdTRUE;
}

bool IRAM_ATTR on_error(twai_node_handle_t, const twai_error_event_data_t* e, void*) {
    if (e->err_flags.arb_lost) ++g_stats.arb_lost;
    if (e->err_flags.bit_err) ++g_stats.bit_err;
    if (e->err_flags.form_err) ++g_stats.form_err;
    if (e->err_flags.stuff_err) ++g_stats.stuff_err;
    if (e->err_flags.ack_err) ++g_stats.ack_err;
    return false;
}

}  // namespace

bool can_start(const CanPortConfig& cfg) {
    g_my_alias = can_alias_of(cfg.node_id);
    g_my_node = cfg.node_id;
    g_rx = new (g_rx_storage) CanReassembler(g_my_alias);
    g_rxq = xQueueCreate(kRxQueue, sizeof(RxItem));
    g_doneq = xQueueCreate(kDoneQueue, sizeof(DoneItem));
    if (g_rxq == nullptr || g_doneq == nullptr) {
        ESP_LOGE(kTag, "queues");
        return false;
    }
    twai_onchip_node_config_t nc{};
    nc.io_cfg.tx = static_cast<gpio_num_t>(cfg.tx_gpio);
    // Loopback without a transceiver needs RX on the TX pin, so the controller reads back what it
    // drives. Espressif's own test does exactly this (esp_driver_twai/test_apps, test_twai_sleep.c:
    // "Using same pin for test without transceiver"). With RX left on its own floating pin, the first
    // try read back noise, hit 17 bit errors and went bus-off before sending anything.
    nc.io_cfg.rx = static_cast<gpio_num_t>(cfg.loopback ? cfg.tx_gpio : cfg.rx_gpio);
    nc.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    nc.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    nc.bit_timing.bitrate = cfg.bitrate;
    nc.tx_queue_depth = kPool;
    nc.fail_retry_cnt = -1;  // arbitration loss and errors: the controller keeps retrying
    nc.flags.enable_self_test = cfg.loopback ? 1 : 0;
    nc.flags.enable_loopback = cfg.loopback ? 1 : 0;
    esp_err_t err = twai_new_node_onchip(&nc, &g_node);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "twai_new_node_onchip: %s", esp_err_to_name(err));
        return false;
    }
    twai_event_callbacks_t cbs{};
    cbs.on_tx_done = on_tx_done;
    cbs.on_rx_done = on_rx_done;
    cbs.on_error = on_error;
    if ((err = twai_node_register_event_callbacks(g_node, &cbs, nullptr)) != ESP_OK ||
        (err = twai_node_enable(g_node)) != ESP_OK) {
        ESP_LOGE(kTag, "twai start: %s", esp_err_to_name(err));
        return false;
    }
    g_up = true;
    ESP_LOGI(kTag, "TWAI up: tx GPIO%d rx GPIO%d, %u bit/s, alias %u%s", cfg.tx_gpio, cfg.rx_gpio,
             static_cast<unsigned>(cfg.bitrate), static_cast<unsigned>(g_my_alias),
             cfg.loopback ? ", LOOPBACK self-test" : "");
    return true;
}

bool can_up() { return g_up; }

int32_t can_send(const uint8_t dst_mac[6], const uint8_t* frame, size_t len) {
    if (!g_up) {
        return -1;
    }
    uint16_t dst_node = 0;
    const uint8_t dst_alias = can_mac_node(dst_mac, dst_node) ? can_alias_of(dst_node) : kCanBroadcastAlias;
    static CanFrame cf[kCanMaxSegments];
    const size_t n = can_encode(frame, len, g_my_alias, dst_alias, cf, kCanMaxSegments);
    if (n == 0) {
        return -2;
    }
    // Claim every slot the message needs before queueing any of it: half a message on the bus is
    // a reassembly timeout at every receiver.
    TxSlot* slots[kCanMaxSegments];
    size_t got = 0;
    portENTER_CRITICAL(&g_pool_mux);
    for (TxSlot& s : g_pool) {
        if (got == n) break;
        if (!s.busy) {
            s.busy = true;
            slots[got++] = &s;
        }
    }
    if (got < n) {
        for (size_t i = 0; i < got; ++i) slots[i]->busy = false;
        portEXIT_CRITICAL(&g_pool_mux);
        ++g_stats.tx_no_slot;
        return -3;
    }
    portEXIT_CRITICAL(&g_pool_mux);

    for (size_t i = 0; i < n; ++i) {
        TxSlot& s = *slots[i];
        std::memcpy(s.data, cf[i].data, cf[i].dlc);
        std::memset(&s.f, 0, sizeof(s.f));
        s.f.header.id = cf[i].id;
        s.f.header.ide = 1;
        s.f.header.dlc = cf[i].dlc;
        s.f.buffer = s.data;
        s.f.buffer_len = cf[i].dlc;
        s.last = (i + 1 == n);
        s.safe_state = can_id_unpack(cf[i].id).opcode == 0x50;  // kOpSafeState
        s.submit_us = static_cast<uint32_t>(esp_timer_get_time());
        std::memcpy(s.dst_mac, dst_mac, 6);
        if (twai_node_transmit(g_node, &s.f, 0) != ESP_OK) {
            for (size_t k = i; k < n; ++k) slots[k]->busy = false;
            return -4;
        }
        ++g_stats.tx_frames;
    }
    return 0;
}

bool can_rx_pop(uint8_t* out, size_t cap, size_t& len, uint8_t src_mac[6], uint32_t& recv_us) {
    RxItem it{};
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    while (g_rxq != nullptr && xQueueReceive(g_rxq, &it, 0) == pdTRUE) {
        const size_t m = g_rx->feed(it.f, now_ms, out, cap);
        if (m == 0) {
            continue;
        }
        Frame f;
        if (parse(out, m, f) != FrameError::Ok) {
            continue;
        }
        if (can_alias_of(f.hdr.src) == g_my_alias && f.hdr.src == g_my_node) {
            ++g_stats.rx_own;  // heard ourselves: loopback self-test only, never a peer
            continue;
        }
        can_mac_of(f.hdr.src, src_mac);
        len = m;
        recv_us = it.us;
        return true;
    }
    if (g_rx != nullptr) {
        g_rx->expire(now_ms);
    }
    return false;
}

bool can_tx_done_pop(uint8_t dst_mac[6], bool& ok, uint32_t& done_us) {
    DoneItem d{};
    if (g_doneq == nullptr || xQueueReceive(g_doneq, &d, 0) != pdTRUE) {
        return false;
    }
    std::memcpy(dst_mac, d.mac, 6);
    ok = d.ok;
    done_us = d.us;
    return true;
}

CanPortStats can_stats() { return g_stats; }

size_t can_tx_free() {
    size_t n = 0;
    portENTER_CRITICAL(&g_pool_mux);
    for (const TxSlot& s : g_pool) {
        if (!s.busy) ++n;
    }
    portEXIT_CRITICAL(&g_pool_mux);
    return n;
}

const CanRxCounters& can_rx_counters() {
    static CanRxCounters none{};
    return g_rx != nullptr ? g_rx->counters() : none;
}

}  // namespace pot
