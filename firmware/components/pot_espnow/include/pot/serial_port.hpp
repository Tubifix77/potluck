// UART transport for Potluck Frames — ARCHITECTURE.md §5.3, §7.1.
//
// §7.1: "potluck-bridge | Physical link ⇄ local frame socket … Forwards Potluck Frames unchanged … Nothing
// above potluck-bridge may open a serial port." This is the node's end of that link.
//
// The host appears to the node as an ordinary peer with a reserved MAC (kHostMac). Everything above
// the transport — membership, the namespace, §4 rule 2 — treats it exactly like a radio peer, which
// is the point: §8.1 says a mode transition is a non-event, and that only holds if a host is not a
// special case in the code.
//
// ---------------------------------------------------------------------------------------------
// WHY A SEPARATE UART, NOT THE CONSOLE
// ---------------------------------------------------------------------------------------------
// The console carries human-readable logs and the statistics stream, and ESP-IDF's logging writes
// to it from any task at any time. Interleaving binary frames with that would corrupt both. So the
// frame link gets its own UART, and the console keeps doing what it does — which also means a
// bridge and a human can watch the same node at once.
//
// On the ESP32-S3-DevKitC-1 the console is UART0 on the "UART" USB-C port. UART1 on two spare GPIOs
// is the frame link, reachable with any USB-serial adapter. M0-RUNBOOK.md says which pins.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/link_stats.hpp"
#include "pot/serial_framing.hpp"

namespace pot {

// The reserved link-layer address for "the host on the other end of the UART". Locally
// administered, and deliberately not a real MAC: nothing on the radio can claim it, so a frame
// addressed here can only ever mean the serial link.
extern const uint8_t kHostMac[kMacLen];

struct SerialPortConfig {
    int uart_num = 1;      // UART1; 0 is the console
    int tx_gpio = 17;      // see M0-RUNBOOK.md for the DevKitC-1 pin choice
    int rx_gpio = 18;
    int baud = 921600;     // matches the console rate and potluck-capture's default
    size_t rx_buffer = 2048;
};

// Bring the UART up. Returns false and logs on failure; a node whose frame link failed keeps
// running on the radio rather than refusing to boot — §8.1's Mode C is the base state, and a
// missing host is normal.
bool serial_port_start(const SerialPortConfig& cfg);

bool serial_port_running();

// Frame a Potluck Frame and write it. Returns 0 on success, or a negative error, matching NodeHal::send's
// contract so it can be dropped straight into the routing decision.
int32_t serial_port_send(const uint8_t* frame, size_t len);

// Pop the next complete, CRC-checked frame received from the host. Returns false when none is
// waiting. `out` needs kEspNowV2LinkMtu bytes.
bool serial_port_rx_pop(uint8_t* out, size_t cap, size_t& len, uint32_t& recv_us);

// Framing and queue statistics, so a bad cable is visible as a CRC count rather than as silence.
struct SerialPortStats {
    SerialReassembler::Stats framing;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t tx_errors;
    uint32_t rx_queue_dropped;  // ours, not the link's — never folded into a link figure
};
SerialPortStats serial_port_stats();

}  // namespace pot
