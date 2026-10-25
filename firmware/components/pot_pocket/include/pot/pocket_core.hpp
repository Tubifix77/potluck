// M13.1: the pocket page's portable half -- what a phone in reach sees when it joins a board's own access point.
//
// The boat (ARCHITECTURE section 13, M13.1): no network and no PC, a board high in the mast dies, the captain joins the
// nearest board's own Wi-Fi and sees which board is dead and since when. The page is the dashboard's first screen as
// one board can know it: every board of the cell alive or dead and since when (its peer table), what runs here, every
// exposed resource with its value, quality and age (section 4: never a number without its age), the package, the radio
// channel, and frames from boards outside this cell (another cell sharing the channel).
//
// Read-only unless the owner turns writes on (M26, pot/pocket_auth.hpp): this file renders a snapshot, and with a
// PageView that is logged in, a form for each value the package lets the outside write. Nothing here deploys. The snapshot is filled on the link task (pot/pocket.hpp), the page rendered on
// the HTTP server's task from a copy -- the server never touches the node.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot::pocket {

constexpr size_t kMaxBoards = 21;  // this board and kMaxPeers (20)
constexpr size_t kMaxValues = 32;  // the outside section's limit (pot/namespace.hpp kMaxOutside)

struct Board {
    uint16_t node = 0;
    bool self = false;
    bool alive = false;
    uint32_t silent_s = 0;  // seconds since its last frame reached this board (0 for this board)
    int8_t rssi = 0;        // of its last frame, dBm; 0 for this board
};

struct Item {
    char name[41] = {};     // the outside section's name: the path after potluck://<system>/
    char value[24] = {};    // formatted; "" when it has none to show
    char quality[12] = {};  // GOOD, STALE, UNAVAILABLE, NO_DATA, FAULTY
    char unit[8] = {};
    uint32_t age_ms = 0;
    bool writable = false;  // M26: the outside section lets it be written, and it is not safety-relevant
};

struct Snapshot {
    char system[32] = {};
    char cell_id[9] = {};
    uint16_t self = 0;
    uint32_t uptime_s = 0;
    uint8_t channel = 0;
    uint32_t package = 0;        // the running package's counter; 0 = none
    char actors[96] = {};        // what runs here, comma-separated
    uint32_t other_cell_frames = 0;  // HELLOs refused as another cell's (trust: a CA this board does not know)
    size_t n_boards = 0;
    Board boards[kMaxBoards];
    size_t n_items = 0;
    Item items[kMaxValues];
};

// M26: what the page offers the phone asking: writes on (the owner set a login), and whether this phone is logged in --
// then its session's form token goes in every form -- and the last write's result, if any.
struct PageView {
    bool writes_on = false;
    bool logged_in = false;
    char csrf[33] = {};  // the session's token, hex
    char note[80] = {};  // e.g. "node-7368/led/on: applied"
};

// The page (HTML, refreshing itself every 5 s while no one is logged in) and the same as JSON (/status.json). Each
// returns the length written, NUL-terminated; 0 if `cap` is short -- never a cut page.
size_t render_html(const Snapshot& s, const PageView& v, char* out, size_t cap);
size_t render_html(const Snapshot& s, char* out, size_t cap);  // read-only: no login, no forms
size_t render_json(const Snapshot& s, char* out, size_t cap);

// The worst `cap` the two need for a full snapshot, for the board's buffer.
constexpr size_t kPageMax = 20480;  // M26: with a form beside each writable value (PSRAM on the board)

}  // namespace pot::pocket
