// M13.1: the pocket page -- the boat's three boards, one dead in the mast, as the captain's phone shows them.

#include <cstring>
#include <string>

#include "pot/pocket_core.hpp"
#include "test_harness.hpp"

using namespace pot::pocket;

namespace {

Snapshot boat() {
    Snapshot s;
    std::strcpy(s.system, "boat");
    std::strcpy(s.cell_id, "ef76cc2e");
    s.self = 0x6300;
    s.uptime_s = 7380;
    s.channel = 1;
    s.package = 47;
    std::strcpy(s.actors, "die_temp,autosteer");
    s.n_boards = 3;
    s.boards[0] = Board{0x6300, true, true, 0, 0};
    s.boards[1] = Board{0x7368, false, true, 0, -40};
    s.boards[2] = Board{0x8160, false, false, 840, -71};  // the mast: dead 14 min
    s.n_items = 2;
    std::strcpy(s.items[0].name, "node-7368/hw/die_temp");
    std::strcpy(s.items[0].value, "41.5");
    std::strcpy(s.items[0].unit, "C");
    std::strcpy(s.items[0].quality, "GOOD");
    s.items[0].age_ms = 1200;
    std::strcpy(s.items[1].name, "node-8160/hw/die_temp");
    std::strcpy(s.items[1].quality, "UNAVAILABLE");
    s.items[1].age_ms = 840000;
    return s;
}

bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

}  // namespace

TEST(pocket, the_page_shows_which_board_is_dead_and_since_when) {
    static char buf[kPageMax];
    const size_t n = render_html(boat(), buf, sizeof(buf));
    CHECK(n > 0);
    const std::string h(buf, n);
    CHECK(has(h, "boat cell &middot; 2 of 3 alive"));
    CHECK(has(h, "node-8160</td><td class=\"dead\">dead &middot; 14 min"));
    CHECK(has(h, "node-6300 (this one)"));
    CHECK(has(h, "41.5 C"));
    CHECK(has(h, ">UNAVAILABLE<"));  // a dead board's value: no number, its quality said
    CHECK(has(h, "package 47 &middot; channel 1 &middot; up 2 h 3 min"));
    CHECK(has(h, "read-only"));
    CHECK(!has(h, "<form"));  // nothing on the page can send anything back
    CHECK(!has(h, "another cell"));
}

TEST(pocket, another_cell_on_the_channel_is_shown_and_names_are_escaped) {
    Snapshot s = boat();
    s.other_cell_frames = 12;
    std::strcpy(s.system, "a<b>&\"c");
    static char buf[kPageMax];
    const std::string h(buf, render_html(s, buf, sizeof(buf)));
    CHECK(has(h, "another cell shares this channel: 12"));
    CHECK(has(h, "a&lt;b&gt;&amp;&quot;c cell"));
    const std::string j(buf, render_json(s, buf, sizeof(buf)));
    CHECK(has(j, "\"system\":\"a<b>&\\\"c\""));
}

TEST(pocket, the_json_carries_the_same_and_a_full_snapshot_fits) {
    static char buf[kPageMax];
    const std::string j(buf, render_json(boat(), buf, sizeof(buf)));
    CHECK(has(j, "{\"node\":\"8160\",\"self\":false,\"alive\":false,\"silent_s\":840,\"rssi\":-71}"));
    CHECK(has(j, "{\"name\":\"node-8160/hw/die_temp\",\"value\":null,\"unit\":\"\",\"quality\":\"UNAVAILABLE\""));
    Snapshot full = boat();
    full.n_boards = kMaxBoards;
    for (size_t i = 0; i < kMaxBoards; ++i) full.boards[i] = Board{static_cast<uint16_t>(0x1000 + i), i == 0, i % 2 == 0, 4000000, -90};
    full.n_items = kMaxValues;
    for (size_t i = 0; i < kMaxValues; ++i) {
        std::memset(full.items[i].name, 'n', 40);
        std::strcpy(full.items[i].value, "-123456.789");
        std::strcpy(full.items[i].unit, "mV");
        std::strcpy(full.items[i].quality, "UNAVAILABLE");
        full.items[i].age_ms = 4000000000u;
    }
    std::memset(full.actors, 'a', sizeof(full.actors) - 1);
    CHECK(render_html(full, buf, sizeof(buf)) > 0);
    CHECK(render_json(full, buf, sizeof(buf)) > 0);
    CHECK_EQ(render_html(boat(), buf, 100), static_cast<size_t>(0));  // short: nothing, never a cut page
}
