// M13: the board's own network connection -- the one Wi-Fi station a board has, whichever module owns it.
//
// Two modules can put a board on the house network: the extender app's hotspot (M6.1, poor-mans-extender) and
// Potluck's own station (M13, pot/wifi.hpp). A radio has one station interface, so a board has at most one
// connection: the extender's when its hotspot has credentials, else Potluck's when it has, else none. Either way
// ESP-NOW attaches, the router owns the channel and the node announces it to the cell; this seam is where
// m0_main.cpp asks about it without knowing which module answers. Modules that connect out -- the presenter --
// ask has_ip().

#pragma once

#include <cstdint>

namespace pot::uplink {

enum class Kind : uint8_t { None = 0, Extender = 1, Wifi = 2 };

// The extender first (it has always owned the radio when it has credentials), then Potluck's station. True if
// one now owns Wi-Fi, so ESP-NOW must attach. Needs NVS up.
bool start(uint8_t cell_channel);

Kind kind();
const char* name();       // "extender", "wifi", "none"
bool active();            // kind() != None
bool associated();        // associated to an access point
bool has_ip();            // ... and holding an address
uint8_t take_channel();   // the latest association's channel, once; 0 if none since the last call
uint8_t hunt_misses();    // the heartbeat misses to declare while the station looks for its network

// M13 (the review's one channel authority among stations): the node says a lower-id station is on `channel`.
// Potluck's station prefers it and picks again (rate-limited); the extender's cannot, and stays where its router
// is -- a partition the failure contract states. False if nothing was done.
bool defer_to(uint8_t channel);

// The channel Potluck's station prefers at its next pick (no-op for the extender, which keeps its own CR-7 rule).
void prefer(uint8_t channel);

// A split station probing for the rest of its cell (pot/split_probe.hpp): the channels its network was heard on, and
// joining one. Potluck's station only: the extender's app has no way to pick again at run time yet, so for it these
// are 0 / false and the rest of the cell comes to it (its channel is the router's, which the cell follows).
uint16_t seen_channels();
bool join_channel(uint8_t channel);

}  // namespace pot::uplink
