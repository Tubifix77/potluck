// M6.1 (CR-6): the extender build -- an application owns the Wi-Fi association, Potluck rides on it.
//
// The application is poor-mans-extender's `pme_hotspot` component (a station on the house router, a
// SoftAP for phones, NAT between them), pulled in from that repository unmodified. This file is the
// seam on Potluck's side, and all of it compiles to stubs unless CONFIG_POT_EXTENDER is set.
//
// Order at boot: start() brings the hotspot up BEFORE espnow_start(), which then only attaches
// (EspNowConfig::attach). From then on the channel flows inward only: each association reports its
// channel through take_channel(), and the node turns it into M5.1's announcement to the cell. Potluck
// never retunes or scans this radio -- the router owns the channel.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot::extender {

// Bring the hotspot up if this is an extender build and it has credentials. True if the application
// now owns Wi-Fi, so ESP-NOW must attach. False otherwise -- an ordinary build, or an extender with
// nothing saved yet, which then runs as an ordinary member until it is given credentials.
// `cell_channel` (CR-7): the cell's saved channel, 0 if none. At boot the hotspot then prefers an access
// point of the house mesh on that channel, within a signal margin of the strongest -- a tie-breaker
// among the mesh's access points, not a pin (pme/hotspot.h).
bool start(uint8_t cell_channel);

// start() returned true.
bool active();

// The channel of the latest router association, once: 0 if none arrived since the last call.
uint8_t take_channel();

// The station is associated to its router. False while it hunts (and on a build that is not an
// extender, or one whose hotspot did not start): while hunting, scans take the radio off the cell's
// channel, so the node declares a looser heartbeat window (CONFIG_POT_EXTENDER_HUNT_MISSES).
bool associated();

// The baseline's status line, field for field (poor-mans-extender's `{"t":"hs"}`), so step 2's figures
// sit next to step 0's without translation. Prints nothing on a non-extender build.
void print_status();

// Console commands, for the owner only (physical access already owns the board):
//   POT! pme sta <ssid> <password>   the house Wi-Fi
//   POT! pme ap <ssid> <password>    the hotspot (WPA2, 8-64 characters)
//   POT! pme forget                  erase both
// Arguments may be double-quoted, with \" and \\ escapes, as at the extender's own `pme>` prompt.
// Nothing typed is ever printed back. True if the line was one of these (handled or refused).
bool handle_console(const char* line, size_t len);

}  // namespace pot::extender
