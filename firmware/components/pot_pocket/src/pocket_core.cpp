// M13.1: the pocket page's portable half. See pot/pocket_core.hpp.

#include "pot/pocket_core.hpp"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace pot::pocket {

namespace {

struct Out {
    char* p;
    size_t cap;
    size_t n = 0;
    bool full = false;
    void add(const char* fmt, ...) {
        if (full) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = std::vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) {
            full = true;
            return;
        }
        n += static_cast<size_t>(w);
    }
    // A string from the cell (a cluster name, a resource name, an actor's) escaped for HTML or for JSON. Every one is
    // printable ASCII by construction; anything else is dropped rather than trusted.
    void text(const char* s, bool json) {
        for (; *s != '\0' && !full; ++s) {
            const char c = *s;
            if (c < 0x20 || c > 0x7E) continue;
            if (!json && c == '<') add("&lt;");
            else if (!json && c == '>') add("&gt;");
            else if (!json && c == '&') add("&amp;");
            else if (c == '"') add(json ? "\\\"" : "&quot;");
            else if (json && c == '\\') add("\\\\");
            else add("%c", c);
        }
    }
    size_t done() const { return full ? 0 : n; }
};

// "3 s", "4 min", "2 h 5 min", "3 d": since when, as the captain reads it.
void ago(Out& o, uint32_t s) {
    if (s < 60) o.add("%" PRIu32 " s", s);
    else if (s < 3600) o.add("%" PRIu32 " min", s / 60);
    else if (s < 86400) o.add("%" PRIu32 " h %" PRIu32 " min", s / 3600, (s % 3600) / 60);
    else o.add("%" PRIu32 " d", s / 86400);
}

}  // namespace

size_t render_html(const Snapshot& s, char* out, size_t cap) {
    Out o{out, cap};
    size_t alive = 0;
    for (size_t i = 0; i < s.n_boards; ++i) alive += s.boards[i].alive ? 1 : 0;
    o.add("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
          "<meta http-equiv=\"refresh\" content=\"5\"><title>");
    o.text(s.system, false);
    o.add(" cell</title><style>body{font-family:sans-serif;margin:16px;max-width:520px}td,th{padding:4px 8px;"
          "text-align:left;border-bottom:1px solid #ccc}.dead{color:#c2412b;font-weight:bold}.ok{color:#2f7d32}"
          "small{color:#666}</style></head><body><h2>");
    o.text(s.system, false);
    o.add(" cell &middot; %u of %u alive</h2>", static_cast<unsigned>(alive), static_cast<unsigned>(s.n_boards));
    o.add("<table><tr><th>board</th><th>state</th><th>signal</th></tr>");
    for (size_t i = 0; i < s.n_boards; ++i) {
        const Board& b = s.boards[i];
        o.add("<tr><td>node-%04x%s</td>", static_cast<unsigned>(b.node), b.self ? " (this one)" : "");
        if (b.alive) {
            o.add("<td class=\"ok\">alive</td>");
        } else {
            o.add("<td class=\"dead\">dead &middot; ");
            ago(o, b.silent_s);
            o.add("</td>");
        }
        if (b.self) o.add("<td>-</td></tr>");
        else o.add("<td>%d dBm</td></tr>", static_cast<int>(b.rssi));
    }
    o.add("</table>");
    if (s.n_items > 0) {
        o.add("<h3>values</h3><table><tr><th>resource</th><th>value</th><th>quality</th><th>age</th></tr>");
        for (size_t i = 0; i < s.n_items; ++i) {
            const Item& it = s.items[i];
            o.add("<tr><td>");
            o.text(it.name, false);
            o.add("</td><td>");
            if (it.value[0] != '\0') {
                o.text(it.value, false);
                if (it.unit[0] != '\0') {
                    o.add(" ");
                    o.text(it.unit, false);
                }
            } else {
                o.add("-");
            }
            o.add("</td><td%s>", (std::strcmp(it.quality, "GOOD") == 0) ? " class=\"ok\"" : " class=\"dead\"");
            o.text(it.quality, false);
            o.add("</td><td>");
            ago(o, it.age_ms / 1000);
            o.add("</td></tr>");
        }
        o.add("</table>");
    }
    o.add("<p><small>running here: ");
    o.text(s.actors[0] != '\0' ? s.actors : "nothing", false);
    o.add("<br>package %" PRIu32 " &middot; channel %u &middot; up ", s.package, static_cast<unsigned>(s.channel));
    ago(o, s.uptime_s);
    if (s.other_cell_frames > 0) {
        o.add("<br>another cell shares this channel: %" PRIu32 " of its frames refused", s.other_cell_frames);
    }
    o.add("<br>cell ");
    o.text(s.cell_id, false);
    o.add(" &middot; read-only: this page changes nothing</small></p></body></html>");
    return o.done();
}

size_t render_json(const Snapshot& s, char* out, size_t cap) {
    Out o{out, cap};
    o.add("{\"system\":\"");
    o.text(s.system, true);
    o.add("\",\"cell\":\"");
    o.text(s.cell_id, true);
    o.add("\",\"node\":\"%04x\",\"uptime_s\":%" PRIu32 ",\"channel\":%u,\"package\":%" PRIu32 ",\"actors\":\"",
          static_cast<unsigned>(s.self), s.uptime_s, static_cast<unsigned>(s.channel), s.package);
    o.text(s.actors, true);
    o.add("\",\"other_cell_frames\":%" PRIu32 ",\"boards\":[", s.other_cell_frames);
    for (size_t i = 0; i < s.n_boards; ++i) {
        const Board& b = s.boards[i];
        o.add("%s{\"node\":\"%04x\",\"self\":%s,\"alive\":%s,\"silent_s\":%" PRIu32 ",\"rssi\":%d}", i ? "," : "",
              static_cast<unsigned>(b.node), b.self ? "true" : "false", b.alive ? "true" : "false", b.silent_s,
              static_cast<int>(b.rssi));
    }
    o.add("],\"values\":[");
    for (size_t i = 0; i < s.n_items; ++i) {
        const Item& it = s.items[i];
        o.add("%s{\"name\":\"", i ? "," : "");
        o.text(it.name, true);
        o.add("\",\"value\":");
        if (it.value[0] != '\0') {
            o.add("\"");
            o.text(it.value, true);
            o.add("\"");
        } else {
            o.add("null");
        }
        o.add(",\"unit\":\"");
        o.text(it.unit, true);
        o.add("\",\"quality\":\"");
        o.text(it.quality, true);
        o.add("\",\"age_ms\":%" PRIu32 "}", it.age_ms);
    }
    o.add("]}");
    return o.done();
}

}  // namespace pot::pocket
