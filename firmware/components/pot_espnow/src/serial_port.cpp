// UART transport for Potluck Frames — see serial_port.hpp.

#include "pot/serial_port.hpp"

#include <cstring>

#include "driver/uart.h"
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

// Four slots. A host is a far slower talker than a radio full of peers, and §6 has no line for this
// ring — it is M2 instrumentation, sized to the smallest number that cannot lose a burst.
constexpr size_t kSerialRxSlots = 4;

StaticQueue_t g_rx_ctrl;
uint8_t g_rx_storage[kSerialRxSlots * sizeof(SerialRxSlot)];
QueueHandle_t g_rx_queue = nullptr;

SerialPortConfig g_cfg;
bool g_running = false;

SerialReassembler g_reassembler;
SerialPortStats g_stats{};

// Scratch for the reader task. The task is the only writer, so one instance is enough and a 1470-byte
// frame never lands on a stack.
SerialRxSlot g_scratch;

StaticTask_t g_reader_tcb;
StackType_t g_reader_stack[3072 / sizeof(StackType_t)];

// Called by the reassembler for each good frame.
void on_frame(void* /*ctx*/, const uint8_t* frame, size_t len) {
    if (len == 0 || len > sizeof(g_scratch.data)) {
        return;
    }
    g_scratch.recv_us = static_cast<uint32_t>(esp_timer_get_time());
    g_scratch.len = static_cast<uint16_t>(len);
    g_scratch.reserved0 = 0;
    std::memcpy(g_scratch.data, frame, len);

    // Zero timeout, for the same reason the ESP-NOW callback uses one: blocking here would stall
    // the reader and let the UART's own buffer overflow, turning a full queue into lost bytes
    // rather than a lost frame. A dropped frame is counted; lost bytes desynchronise the stream.
    if (xQueueSend(g_rx_queue, &g_scratch, 0) != pdTRUE) {
        ++g_stats.rx_queue_dropped;
    }
}

void reader_task(void*) {
    // A modest read buffer: the reassembler holds the frame state, so this only needs to be big
    // enough to keep the UART driver from backing up.
    uint8_t buf[256];
    for (;;) {
        const int n = uart_read_bytes(static_cast<uart_port_t>(g_cfg.uart_num), buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            g_reassembler.feed(buf, static_cast<size_t>(n), &on_frame, nullptr);
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

    g_rx_queue = xQueueCreateStatic(kSerialRxSlots, sizeof(SerialRxSlot), g_rx_storage, &g_rx_ctrl);
    if (g_rx_queue == nullptr) {
        ESP_LOGW(kTag, "could not create the serial rx queue");
        return false;
    }

    g_reassembler.reset();
    g_stats = SerialPortStats{};
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

bool serial_port_running() { return g_running; }

int32_t serial_port_send(const uint8_t* frame, size_t len) {
    if (!g_running) {
        return -1;
    }
    // Framed on the stack: kSerialFrameMax is ~1.5 KB and the link task's stack is 4 KB. A static
    // buffer would be shared with the reader task and need a lock for no benefit, since only the
    // link task sends.
    static uint8_t wire[kSerialFrameMax];
    const size_t n = write_serial_frame(frame, len, wire, sizeof(wire));
    if (n == 0) {
        ++g_stats.tx_errors;
        return -2;
    }
    const int written = uart_write_bytes(static_cast<uart_port_t>(g_cfg.uart_num), wire, n);
    if (written != static_cast<int>(n)) {
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

}  // namespace pot
