// UART transport for Potluck Frames — see serial_port.hpp.

#include "pot/serial_port.hpp"

#include <cstring>

#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace pot {

const uint8_t kHostMac[kMacLen] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xFE};

namespace {

const char* kTag = "pot.serial";

// One received frame, queued for the link task. Same shape and reasoning as the ESP-NOW RxSlot:
// sized to the profile MTU rather than to today's traffic, with the arrival timestamp taken as
// close to the wire as this layer can manage.
struct SerialRxSlot {
    uint32_t recv_us;
    uint16_t len;
    uint16_t reserved0;
    uint8_t data[kEspNowV2LinkMtu];
};

// Sixteen slots, in PSRAM (below). Four were "the smallest number that cannot lose a burst" while the host was one
// tool; since M14 the hub, the PC's host node and the tools behind the hub's relay share the cable, and the hub reads
// the cell in a burst every 2 s -- with four, ~1 in 120 of the PC's reads was dropped here unanswered (2026-10-26,
// rx_queue_dropped, printed nowhere until then).
constexpr size_t kSerialRxSlots = 16;

// M6: buffers only tasks touch, at the host cable's speed, live in PSRAM when the build has it
// (EXT_RAM_BSS_ATTR is empty otherwise). Never one an ISR touches: the cache, and with it PSRAM, is
// off during a flash write, and only tasks are guaranteed to be held still then (ESP-IDF
// external-ram guide). Section 6's cap counts internal RAM, which is what these were spending.
StaticQueue_t g_rx_ctrl;
EXT_RAM_BSS_ATTR uint8_t g_rx_storage[kSerialRxSlots * sizeof(SerialRxSlot)];
QueueHandle_t g_rx_queue = nullptr;

SerialPortConfig g_cfg;
bool g_running = false;

EXT_RAM_BSS_ATTR SerialReassembler g_reassembler;
SerialPortStats g_stats{};
void* g_rx_notify = nullptr;  // the link task (serial_port_set_rx_notify)

// Scratch for the reader task. The task is the only writer, so one instance is enough and a 1470-byte
// frame never lands on a stack.
EXT_RAM_BSS_ATTR SerialRxSlot g_scratch;

StaticTask_t g_reader_tcb;
StackType_t g_reader_stack[3072 / sizeof(StackType_t)];

// M20: the frame link on the console UART beside the logs (ConsoleDemux). Which UART the host was last heard
// on is where replies go: the dedicated one (an adapter on GPIO17/18) or the console (the board's own USB cable).
EXT_RAM_BSS_ATTR ConsoleDemux g_console_demux;
int g_console_uart = -1;
bool g_host_on_console = false;
bool ensure_queue();

// Called by the reassembler for each good frame.
void on_frame(void* ctx, const uint8_t* frame, size_t len) {
    g_host_on_console = ctx != nullptr;  // the console's demux passes a non-null ctx
    // Two tasks call this (the dedicated UART's reader and the console's): each has its own scratch slot.
    EXT_RAM_BSS_ATTR static SerialRxSlot console_scratch;
    SerialRxSlot& slot = ctx != nullptr ? console_scratch : g_scratch;
    if (len == 0 || len > sizeof(slot.data)) {
        return;
    }
    slot.recv_us = static_cast<uint32_t>(esp_timer_get_time());
    slot.len = static_cast<uint16_t>(len);
    slot.reserved0 = 0;
    std::memcpy(slot.data, frame, len);

    // Zero timeout, for the same reason the ESP-NOW callback uses one: blocking here would stall
    // the reader and let the UART's own buffer overflow, turning a full queue into lost bytes
    // rather than a lost frame. A dropped frame is counted; lost bytes desynchronise the stream.
    if (xQueueSend(g_rx_queue, &slot, 0) != pdTRUE) {
        ++g_stats.rx_queue_dropped;
    } else if (g_rx_notify != nullptr) {
        // Wake the link task at once: it used to wait up to 20 ms on the radio's queue before it looked here, and a
        // burst from the hub overflowed this queue meanwhile (2026-10-26: 89 of 5,584 frames dropped at 16 slots).
        xTaskNotifyGive(static_cast<TaskHandle_t>(g_rx_notify));
    }
}

void reader_task(void*) {
    // A modest read buffer: the reassembler holds the frame state, so this only needs to be big
    // enough to keep the UART driver from backing up.
    uint8_t buf[256];
    const auto port = static_cast<uart_port_t>(g_cfg.uart_num);
    for (;;) {
        // The first byte, then whatever else is already buffered. uart_read_bytes waits for its whole length or the
        // timeout (ESP-IDF uart.c), so asking for 256 held every short frame from the PC 50 ms -- found 2026-10-25: a
        // read of this board's own uptime through the cable took 72 ms at the median (M24's input, 144 ms).
        int n = uart_read_bytes(port, buf, 1, pdMS_TO_TICKS(50));
        if (n > 0) {
            size_t more = 0;
            if (uart_get_buffered_data_len(port, &more) == ESP_OK && more > 0) {
                const int m = uart_read_bytes(port, buf + 1, more < sizeof(buf) - 1 ? more : sizeof(buf) - 1, 0);
                if (m > 0) n += m;
            }
            g_reassembler.feed(buf, static_cast<size_t>(n), &on_frame, nullptr);  // null ctx: the dedicated UART
            g_stats.framing = g_reassembler.stats();
        }
    }
}

}  // namespace

bool serial_port_start(const SerialPortConfig& cfg) {
    if (g_running) {
        return true;
    }
    g_cfg = cfg;

    uart_config_t uc{};
    uc.baud_rate = cfg.baud;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(static_cast<uart_port_t>(cfg.uart_num), static_cast<int>(cfg.rx_buffer),
                                        static_cast<int>(cfg.rx_buffer), 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(static_cast<uart_port_t>(cfg.uart_num), &uc);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(static_cast<uart_port_t>(cfg.uart_num), cfg.tx_gpio, cfg.rx_gpio, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!ensure_queue()) {
        return false;
    }

    g_reassembler.reset();
    g_running = true;

    // Priority 5: below the link task, so frame handling still wins, and above the stats task, so a
    // slow console cannot stall the host link.
    xTaskCreateStaticPinnedToCore(reader_task, "pot_serial_rx",
                                  sizeof(g_reader_stack) / sizeof(StackType_t), nullptr, 5,
                                  g_reader_stack, &g_reader_tcb, 0);

    ESP_LOGI(kTag, "frame link on UART%d, tx=%d rx=%d, %d baud", cfg.uart_num, cfg.tx_gpio,
             cfg.rx_gpio, cfg.baud);
    return true;
}

namespace {
bool ensure_queue() {
    if (g_rx_queue != nullptr) {
        return true;
    }
    g_rx_queue = xQueueCreateStatic(kSerialRxSlots, sizeof(SerialRxSlot), g_rx_storage, &g_rx_ctrl);
    if (g_rx_queue == nullptr) {
        ESP_LOGW(kTag, "could not create the serial rx queue");
        return false;
    }
    g_stats = SerialPortStats{};
    return true;
}
}  // namespace

bool serial_console_attach(int uart_num) {
    if (!ensure_queue()) {
        return false;
    }
    g_console_demux.reset();
    g_console_uart = uart_num;
    ESP_LOGI(kTag, "frame link also on the console, UART%d, beside the logs", uart_num);
    return true;
}

void serial_console_feed(const uint8_t* data, size_t len, ConsoleDemux::TextFn on_text, void* text_ctx) {
    if (g_console_uart < 0) {
        if (on_text != nullptr) on_text(text_ctx, data, len);
        return;
    }
    // The frame callback's ctx marks the console; on_text gets the caller's own.
    struct Both {
        ConsoleDemux::TextFn text;
        void* ctx;
    } both{on_text, text_ctx};
    g_console_demux.feed(
        data, len, &on_frame,
        [](void* c, const uint8_t* t, size_t n) {
            auto* b = static_cast<Both*>(c);
            if (b->text != nullptr) b->text(b->ctx, t, n);
        },
        &both);
}

ConsoleDemux::Stats serial_console_stats() { return g_console_demux.stats(); }

bool serial_port_running() { return g_running || g_console_uart >= 0; }

int32_t serial_port_send(const uint8_t* frame, size_t len) {
    const bool console = g_console_uart >= 0 && (g_host_on_console || !g_running);
    if (!g_running && !console) {
        return -1;
    }
    // Framed in PSRAM: kSerialFrameMax is ~1.5 KB. Only the link task sends, so one buffer needs no lock.
    // wire[0] is the console's opening zero (ConsoleDemux); the dedicated UART is sent from wire + 1, as before.
    EXT_RAM_BSS_ATTR static uint8_t wire[kSerialFrameMax + 1];
    wire[0] = kSerialDelimiter;
    const size_t n = write_serial_frame(frame, len, wire + 1, sizeof(wire) - 1);
    if (n == 0) {
        ++g_stats.tx_errors;
        return -2;
    }
    // One driver call, so neither a log line nor anything else lands inside the frame: printf goes through the
    // same driver once the console has called uart_vfs_dev_use_driver, and the driver serialises its writers.
    const int uart = console ? g_console_uart : g_cfg.uart_num;
    const uint8_t* out = console ? wire : wire + 1;
    const size_t out_n = console ? n + 1 : n;
    const int written = uart_write_bytes(static_cast<uart_port_t>(uart), out, out_n);
    if (written != static_cast<int>(out_n)) {
        ++g_stats.tx_errors;
        return -3;
    }
    ++g_stats.tx_frames;
    g_stats.tx_bytes += static_cast<uint32_t>(n);
    return 0;
}

bool serial_port_rx_pop(uint8_t* out, size_t cap, size_t& len, uint32_t& recv_us) {
    len = 0;
    if (!g_running || g_rx_queue == nullptr || out == nullptr) {
        return false;
    }
    static SerialRxSlot slot;
    if (xQueueReceive(g_rx_queue, &slot, 0) != pdTRUE) {
        return false;
    }
    if (slot.len > cap) {
        return false;
    }
    std::memcpy(out, slot.data, slot.len);
    len = slot.len;
    recv_us = slot.recv_us;
    return true;
}

SerialPortStats serial_port_stats() { return g_stats; }

void serial_port_set_rx_notify(void* task) { g_rx_notify = task; }

}  // namespace pot
