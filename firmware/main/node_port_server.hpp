// M29: the node port on this board's own station -- a node from the house network (a tablet) logs in here exactly as
// at the hub's node port (pot/node_port.hpp for the login, COBS + CRC-16 Potluck Frames after it), so it needs no PC.
//
// One connection at a time: a node logging in again replaces the old one (its Wi-Fi dropped and came back before the
// old socket timed out), and a node silent for kIdleMs is dropped. Its frames reach the link task through rx_pop(),
// which hands them to the Node from the node's wire-family address (02:00:00:01:<id>), as the cable does; frames to
// that address go back over the socket. The task's stack and every buffer are in PSRAM: the core has 2 KB of internal
// RAM left. Serves only while the station holds an address.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot::nodeport {

constexpr uint32_t kIdleMs = 15000;  // as the hub's IDLE_S: the node beats every 250 ms

struct Hooks {
    const uint8_t* ca_pub;  // 32 bytes, the cell's CA: who may log in
    uint16_t board_id;
    uint8_t board_mac[6];  // the address this board signs its HELLO over
    // Runs fn(arg) where the crypto may run (trust_rt::run_heavy: the console task) and returns when it has.
    void (*run_heavy)(void* ctx, void (*fn)(void*), void* arg);
};

bool start(const Hooks& h);       // the server task; false if it could not be created
void set_rx_notify(void* task);   // the link task, woken for each frame queued
bool rx_pop(uint8_t* out, size_t cap, size_t& len, uint16_t& node);
bool serves(uint16_t node);       // that node is logged in here now
bool any();                       // anyone is
int32_t send_to(uint16_t node, const uint8_t* frame, size_t len);  // 0 sent, -1 not here or not taken
void send_all(const uint8_t* frame, size_t len);                    // a broadcast to whoever is logged in
uint32_t address();               // the station's IPv4, host order (a.b.c.d = a << 24 ...); 0 while not serving
uint64_t published();             // sys/nodeport's value: (address << 16) | port; 0 while not serving

struct Counters {
    uint32_t logins, refused, replaced, idle_drops, frames_in, frames_out, dropped_src, dropped_rate, dropped_queue,
        send_fail;
};
Counters counters();

}  // namespace pot::nodeport
