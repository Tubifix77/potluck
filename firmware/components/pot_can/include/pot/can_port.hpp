// The TWAI (CAN) transport on the ESP32 -- M4. Board-only; the portable half is can_profile.hpp.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/can_profile.hpp"

namespace pot {

struct CanPortConfig {
    int tx_gpio = 4;
    int rx_gpio = 5;
    uint32_t bitrate = 500000;
    bool loopback = false;  // the controller hears itself and needs no ACK: a one-board self-test
    uint16_t node_id = 0;
};

struct CanPortStats {
    uint32_t tx_frames;      // CAN frames handed to the driver
    uint32_t tx_ok;          // ... and reported sent
    uint32_t tx_fail;
    uint32_t tx_no_slot;     // a message refused because the frame pool was full
    uint32_t arb_lost;       // the controller lost arbitration (it retries by itself)
    uint32_t bit_err;
    uint32_t form_err;
    uint32_t stuff_err;
    uint32_t ack_err;
    uint32_t rx_frames;
    uint32_t rx_overflow;    // the ISR's receive queue was full
    uint32_t rx_own;         // our own frames, heard back (loopback self-test only); not delivered
    // SAFE_STATE timing, on this node's clock: from can_send() to the controller reporting it sent.
    // Under saturating traffic this is what shows it was not kept waiting.
    uint32_t safe_state_sent;
    uint32_t safe_state_last_us;
    uint32_t safe_state_max_us;
    uint32_t safe_state_min_us;
};

bool can_start(const CanPortConfig& cfg);
bool can_up();

// Encode and queue one Potluck frame. The destination is the CAN address from can_mac_of(), or the
// broadcast MAC. Returns 0 on success.
int32_t can_send(const uint8_t dst_mac[6], const uint8_t* frame, size_t len);

// The next complete Potluck frame received, reassembled. False when there is none.
bool can_rx_pop(uint8_t* out, size_t cap, size_t& len, uint8_t src_mac[6], uint32_t& recv_us);

// A message that finished transmitting: its last CAN frame was sent (ok) or failed.
bool can_tx_done_pop(uint8_t dst_mac[6], bool& ok, uint32_t& done_us);

CanPortStats can_stats();
// Free frames in the transmit pool: traffic that must not starve beacons checks this first.
size_t can_tx_free();
const CanRxCounters& can_rx_counters();

}  // namespace pot
