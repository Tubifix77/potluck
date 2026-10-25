// M13: Potluck's own Wi-Fi station -- an optional board module (`firmware.modules: ["wifi"]`, CONFIG_POT_WIFI).
//
// A board on the house network, without the private extender app: what the presenting module (M13) needs to
// reach a broker. The station owns the Wi-Fi driver and ESP-NOW attaches to it (EspNowConfig::attach), exactly as
// on an extender build: the router owns the channel, and the node announces it to the cell (M6.1). What it adds:
//
//  * credentials in its own NVS namespace, typed by the owner on the console (`POT! wifi sta ...`, or
//    tools/wifi_set.py from a file); nothing typed is ever printed back, and ESP-IDF's own "connected with
//    <name>" line is silenced, since a soak captures the console whole;
//  * a scan before each association that prefers an access point on the channel the cell is on, within a margin
//    of the strongest or heard at -70 dBm or better (the house mesh serves one network on several channels:
//    CR-7, M0-LOG session 31; the floor from 2026-10-25), so a
//    station does not drag or split the cell when it joins;
//  * reconnects that back off (1 s, doubling to 60 s): each scan takes the radio off the cell's channel for
//    seconds (M0-LOG session 30), and the node declares a looser heartbeat window while the station hunts.
//
// Compiled to stubs without CONFIG_POT_WIFI, so callers need no #if.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot::wifi {

// Bring the station up if this build carries the module and credentials are saved. True if it now owns Wi-Fi,
// so ESP-NOW must attach. `cell_channel`: the cell's saved channel, 0 if none -- preferred at the first pick.
// Needs NVS up.
bool start(uint8_t cell_channel);

// start() returned true.
bool active();

// Associated to an access point (false while it scans or waits to retry).
bool associated();

// Associated and holding an address: what a module that connects out (the presenter) waits for.
bool has_ip();

// The channel of the latest association, once: 0 if none arrived since the last call.
uint8_t take_channel();

// The channel to prefer at the next pick (0: none). The node's link loop keeps it at the cell's channel, or the
// channel authority's when a lower-id station outranks this one (uplink::prefer).
void prefer_channel(uint8_t channel);

// Drop the association and pick again, preferring prefer_channel()'s -- what a station does when the cell's
// channel authority (a lower node id) is on another channel. At most once per 30 s; false if refused for that.
bool repick();

// The channels this network was heard on in the station's scans since boot (bit c for channel c): a house mesh serves
// one network on several (pot/split_probe.hpp).
uint16_t seen_channels();

// Drop the association and join an access point on `channel`, whatever the margin -- a split station probing for the
// rest of its cell (pot/split_probe.hpp), or going back. Not rate-limited: the probe paces itself. False if the
// station is not up.
bool join_channel(uint8_t channel);

struct Status {
    bool active = false, associated = false, has_ip = false;
    uint8_t channel = 0;
    int8_t rssi = 0;
    uint32_t associations = 0;  // since boot
    uint32_t scans = 0;
    uint32_t retry_in_ms = 0;   // 0 unless waiting to retry
    uint8_t last_reason = 0;    // ESP-IDF's disconnect reason, 0 if none yet
};
Status status();

// One JSON line, {"t":"wifi",...}: never a network name, never a password.
void print_status();

// Console commands, for the owner only (physical access already owns the board):
//   POT! wifi sta <name> <password>   save the house Wi-Fi (takes effect at the next boot)
//   POT! wifi forget                  erase it
// Arguments may be double-quoted, with \" and \\ escapes. Prints the result only, never the arguments.
// True if the line was one of these.
bool handle_console(const char* line, size_t len);

}  // namespace pot::wifi
