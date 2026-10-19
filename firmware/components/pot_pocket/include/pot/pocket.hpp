// M13.1: the pocket page -- the board half (an optional module, `firmware.modules: ["pocket"]`, CONFIG_POT_POCKET).
//
// A board makes an access point of its own, with a name and password the owner sets (`POT! pocket ap ...`; no
// password, no access point -- a fresh board never offers an open network), and serves one read-only page on it
// with ESP-IDF's HTTP server: `/` (HTML) and `/status.json`. Any other method on them is answered 405, any other path
// 404; nothing writes, nothing deploys. The station interface stays where it was, so ESP-NOW keeps its channel and the
// access point moves with it when the cell moves (a phone on it reconnects).
//
// It is the one place a board listens (section 8.4's exception): optional, and a security liability -- the password
// is the boundary, not Wi-Fi's range (section 9.4's row). Not on an extender whose hotspot is up: the extender owns
// the board's access point.
//
// The link task fills a Snapshot (pot/pocket_core.hpp) once a second and hands it over with publish(); the server's
// task renders a copy. Compiled to stubs without CONFIG_POT_POCKET.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/pocket_core.hpp"

namespace pot::pocket {

// After ESP-NOW is up. False -- idle -- without a saved name and password, when another module owns the access point
// (`other_ap`, the extender), or if the access point or the server would not start.
bool start(bool other_ap);
bool active();
// Why it is idle, for NODE_STATUS: 0 active, 1 no credentials, 2 another module owns the access point, 3 failed.
int idle_reason();

void publish(const Snapshot& s);

// {"t":"pocket",...}: active, phones on it, pages served, refused requests, the internal heap.
void print_status();
// What the node publishes (sys/pocket/*, pot/sys_resources.hpp): false while the access point is not up.
bool figures(uint32_t* phones, uint32_t* pages, uint32_t* refused);

// Console, for the owner only:
//   POT! pocket ap <name> <password>   the access point's name and WPA2 password (8-63 characters), at the next boot
//   POT! pocket forget                 erase them: no access point
// Prints the result only, never the arguments. True if the line was one of these.
bool handle_console(const char* line, size_t len);

}  // namespace pot::pocket
