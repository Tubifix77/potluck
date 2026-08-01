// Serial stats format tests — the contract between the firmware and host/potluck.
//
// Two jobs. First, golden-string assertions so the format cannot drift silently: potluck-capture parses
// these bytes, and a field quietly renamed on the firmware side would show up as a missing column
// in a 24-hour soak report rather than as a build failure. Second, `--emit-fixtures <dir>` writes
// the same lines to a file that the Python side loads and parses with a real JSON parser, so both
// halves of the loop are checked against one source of truth.

#include <cstdio>
#include <cstring>
#include <string>

#include "pot/membership.hpp"
#include "pot/payloads.hpp"
#include "pot/stats_json.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

// A peer with values chosen to be individually recognisable in the output, so a swapped pair of
// fields is visible by reading rather than by arithmetic.
PeerLink sample_peer() {
    PeerLink p;
    p.reset();
    const uint8_t mac[kMacLen] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    std::memcpy(p.mac, mac, kMacLen);
    p.node_id = 0xCDEF;
    p.boot_epoch = 17;
    p.state = PeerState::Alive;
    p.version = EspNowVersion::V2;
    p.hb_period_ms = kHbPeriodWirelessMs;
    p.miss_limit = kHbMissLimitWireless;
    p.misses = 1;

    p.tx_frames = 1000;
    p.tx_cb_ok = 990;
    p.tx_cb_fail = 8;
    p.tx_enqueue_err = 2;

    p.rx_frames = 950;
    p.rx_lost_seqgap = 50;
    p.rx_reorder_dup = 3;
    p.rx_dropped_bad = 1;
    p.rx_hb_lost_seqgap = 40;

    p.rtt_samples = 4;
    p.rtt_min_us = 2600;
    p.rtt_max_us = 51000;
    p.rtt_timeouts = 2;
    p.txq_last_us = 900;
    p.txq_max_us = 1500;
    p.remote_turnaround_last_us = 320;
    p.remote_turnaround_max_us = 640;
    return p;
}

RttHistogram sample_histogram() {
    RttHistogram h;
    h.reset();
    h.add(2600);   // bucket 2: (2000, 3000]
    h.add(2900);   // bucket 2
    h.add(5000);   // bucket 4: (4000, 6000]
    h.add(51000);  // bucket 11: (42000, 60000]
    return h;
}

bool json_structure_is_sound(const std::string& line) {
    // Not a parser — the Python side does that. This checks the properties a truncated or
    // mis-escaped line would violate: balanced braces and brackets outside strings, a quote count
    // that closes, a single trailing newline, and no stray control characters.
    if (line.empty() || line.back() != '\n') return false;
    if (line.find('\n') != line.size() - 1) return false;

    int braces = 0, brackets = 0;
    bool in_string = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        switch (c) {
            case '"': in_string = true; break;
            case '{': ++braces; break;
            case '}': --braces; break;
            case '[': ++brackets; break;
            case ']': --brackets; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) return false;
                break;
        }
        if (braces < 0 || brackets < 0) return false;
    }
    return !in_string && braces == 0 && brackets == 0;
}

LinkRecord sample_link_record(const PeerLink& p, const RttHistogram& h) {
    LinkRecord lr{};
    lr.uptime_ms = 123456;
    lr.node_id = 0x0102;
    lr.peer_node_id = p.node_id;
    lr.peer = &p;
    lr.histogram = &h;
    lr.last_rssi = -67;
    return lr;
}

}  // namespace

TEST(json, link_line_is_exactly_this) {
    const PeerLink p = sample_peer();
    const RttHistogram h = sample_histogram();
    const LinkRecord lr = sample_link_record(p, h);

    char buf[kJsonLineMax];
    const size_t n = write_link_json(buf, sizeof(buf), lr);
    CHECK(n > 0);

    const char* want =
        "{\"t\":\"link\",\"up_ms\":123456,\"node\":258,\"peer\":52719,"
        "\"mac\":\"24:6f:28:ab:cd:ef\",\"state\":\"alive\",\"epoch\":17,\"espnow_ver\":2,"
        "\"mtu\":1446,\"misses\":1,\"rssi\":-67,"
        "\"tx\":{\"frames\":1000,\"cb_ok\":990,\"cb_fail\":8,\"enqueue_err\":2,"
        "\"pdr_ppm\":991983},"
        "\"rx\":{\"frames\":950,\"lost_seqgap\":50,\"reorder_dup\":3,\"dropped_bad\":1,"
        "\"hb_lost\":40,\"pdr_ppm\":950000},"
        "\"rtt\":{\"samples\":4,\"min_us\":2600,\"max_us\":51000,\"timeouts\":2,"
        "\"txq_last_us\":900,\"txq_max_us\":1500,\"remote_turnaround_us\":320,"
        "\"remote_turnaround_max_us\":640,\"p50_us\":[2000,3000],\"p99_us\":[42000,60000],"
        "\"hist\":[0,0,2,0,1,0,0,0,0,0,0,1,0,0,0,0]}}\n";

    CHECK_STR_EQ(std::string(buf, n), std::string(want));
    CHECK(json_structure_is_sound(std::string(buf, n)));
}

TEST(json, there_is_no_one_way_delay_field) {
    // The methodology commitment, as a test. A one-way figure would need synchronised clocks or an
    // assumption of path symmetry, and ESP-NOW's retry machinery provides neither (§3). If someone
    // adds such a field later, this fails and they have to argue with link_stats.hpp first.
    const PeerLink p = sample_peer();
    const RttHistogram h = sample_histogram();
    const LinkRecord lr = sample_link_record(p, h);

    char buf[kJsonLineMax];
    const size_t n = write_link_json(buf, sizeof(buf), lr);
    const std::string line(buf, n);

    CHECK(line.find("one_way") == std::string::npos);
    CHECK(line.find("oneway") == std::string::npos);
    CHECK(line.find("owd") == std::string::npos);
    CHECK(line.find("latency_us") == std::string::npos);

    // And every duration reported names the clock it was measured on, by carrying either "rtt",
    // "txq" or "remote_turnaround" in its key.
    CHECK(line.find("\"remote_turnaround_us\"") != std::string::npos);
    CHECK(line.find("\"txq_last_us\"") != std::string::npos);
}

TEST(json, unmeasured_values_are_null_not_zero) {
    // §13-M0 wants a measured PDR. A fresh link has no measurement, and reporting 0 would be a
    // claim of total loss while reporting 1000000 would be a claim of perfection. Both are lies.
    PeerLink p;
    p.reset();
    p.state = PeerState::Known;
    p.node_id = 7;
    RttHistogram h;
    h.reset();

    LinkRecord lr{};
    lr.uptime_ms = 5;
    lr.node_id = 1;
    lr.peer_node_id = 7;
    lr.peer = &p;
    lr.histogram = &h;

    char buf[kJsonLineMax];
    const size_t n = write_link_json(buf, sizeof(buf), lr);
    CHECK(n > 0);
    const std::string line(buf, n);

    CHECK(line.find("\"pdr_ppm\":null") != std::string::npos);
    CHECK(line.find("\"min_us\":null") != std::string::npos);
    CHECK(line.find("\"max_us\":null") != std::string::npos);
    CHECK(line.find("\"p50_us\":null") != std::string::npos);
    CHECK(line.find("\"p99_us\":null") != std::string::npos);
    CHECK(line.find("\"pdr_ppm\":0") == std::string::npos);

    // An unknown ESP-NOW version pins the payload cap to the v1 floor (§5.3), and the line says so.
    CHECK(line.find("\"espnow_ver\":0") != std::string::npos);
    CHECK(line.find("\"mtu\":226") != std::string::npos);
}

TEST(json, overflow_bucket_percentile_has_no_upper_bound) {
    PeerLink p = sample_peer();
    RttHistogram h;
    h.reset();
    h.add(500000);  // above the last finite edge

    LinkRecord lr = sample_link_record(p, h);
    char buf[kJsonLineMax];
    const size_t n = write_link_json(buf, sizeof(buf), lr);
    const std::string line(buf, n);
    CHECK(line.find("\"p50_us\":[200000,null]") != std::string::npos);
}

TEST(json, boot_line_reports_the_section_6_measurement) {
    BootRecord r{};
    r.node_id = 0x0102;
    r.boot_epoch = 42;
    const uint8_t mac[kMacLen] = {0x24, 0x6F, 0x28, 0x00, 0x11, 0x22};
    std::memcpy(r.mac, mac, kMacLen);
    r.espnow_version = 2;
    r.channel = 1;
    r.hb_period_ms = 100;
    r.hb_miss_limit = 6;
    r.fw_version = "m0";
    r.idf_version = "v6.0.2";
    // Plausible readings: free DRAM falls as each layer comes up.
    r.dram_at_boot = 300000;
    r.dram_after_nvs = 296000;
    r.dram_after_netif = 292000;
    r.dram_after_wifi_init = 260000;
    r.dram_after_wifi_start = 258000;
    r.dram_after_espnow = 256000;
    r.dram_largest_block = 110000;

    char buf[kJsonLineMax];
    const size_t n = write_boot_json(buf, sizeof(buf), r);
    CHECK(n > 0);
    const std::string line(buf, n);
    CHECK(json_structure_is_sound(line));

    // The derived figures are computed by the emitter so a reader cannot get the subtraction
    // backwards. wifi_stack is the number §6's [MEASURE] item asks for: after_netif − after_wifi_start.
    CHECK(line.find("\"wifi_stack\":34000") != std::string::npos);
    CHECK(line.find("\"espnow\":2000") != std::string::npos);
    CHECK(line.find("\"total_to_radio\":44000") != std::string::npos);
    CHECK(line.find("\"idf\":\"v6.0.2\"") != std::string::npos);
    CHECK(line.find("\"hb_period_ms\":100") != std::string::npos);
    CHECK(line.find("\"hb_miss_limit\":6") != std::string::npos);
}

TEST(json, node_and_event_lines) {
    NodeCounters c;
    c.reset();
    c.rx_total = 5000;
    c.rx_bad_frame = 4;
    c.rx_unknown_peer = 3;
    c.rx_wrong_dst = 2;
    c.rx_short_payload = 1;
    c.rx_unknown_opcode = 6;
    c.tx_total = 5100;
    c.tx_enqueue_err = 7;
    c.deaths_declared = 2;
    c.revivals = 2;
    c.reboots_seen = 1;
    c.peer_table_full = 0;

    NodeRecord nr{};
    nr.uptime_ms = 60000;
    nr.node_id = 0x0102;
    nr.boot_epoch = 42;
    nr.counters = &c;
    nr.free_dram_now = 200000;
    nr.largest_free_block = 100000;
    nr.rx_queue_dropped = 9;
    nr.tx_done_queue_dropped = 8;
    nr.event_ring_dropped = 5;
    nr.peers_alive = 1;
    nr.peers_dead = 1;

    char buf[kJsonLineMax];
    size_t n = write_node_json(buf, sizeof(buf), nr);
    CHECK(n > 0);
    std::string line(buf, n);
    CHECK(json_structure_is_sound(line));
    CHECK(line.find("\"t\":\"node\"") != std::string::npos);
    CHECK(line.find("\"deaths\":2") != std::string::npos);
    // A queue overrun is Potluck's own fault and is reported separately from anything the radio did.
    CHECK(line.find("\"queue_dropped\":9") != std::string::npos);
    CHECK(line.find("\"events_dropped\":5") != std::string::npos);

    Event e{};
    e.at_ms = 601;
    e.kind = EventKind::PeerDead;
    e.peer_slot = 0;
    e.node_id = 0xCDEF;
    e.detail_a = 6;    // misses
    e.detail_b = 600;  // ms silent
    n = write_event_json(buf, sizeof(buf), 0x0102, e);
    CHECK(n > 0);
    line.assign(buf, n);
    CHECK(json_structure_is_sound(line));
    CHECK_STR_EQ(line,
                 std::string("{\"t\":\"event\",\"at_ms\":601,\"node\":258,\"kind\":\"peer_dead\","
                             "\"peer\":52719,\"slot\":0,\"a\":6,\"b\":600}\n"));
}

TEST(json, a_line_that_would_overflow_is_refused_not_truncated) {
    // A truncated JSON line is a line the reader has to guess about. Returning 0 means the caller
    // prints nothing, which a reader handles correctly by definition.
    const PeerLink p = sample_peer();
    const RttHistogram h = sample_histogram();
    const LinkRecord lr = sample_link_record(p, h);

    char full[kJsonLineMax];
    const size_t need = write_link_json(full, sizeof(full), lr);
    CHECK(need > 0);
    CHECK(need < kJsonLineMax);  // the real line fits with room to spare

    // Every buffer size below what is needed must refuse, and must not write past its end.
    for (size_t cap = 1; cap < need; cap += 7) {
        char small[kJsonLineMax];
        std::memset(small, 0x7E, sizeof(small));
        CHECK_EQ(write_link_json(small, cap, lr), static_cast<size_t>(0));
        for (size_t i = cap; i < sizeof(small); ++i) {
            if (small[i] != 0x7E) {
                ::tst::fail(__FILE__, __LINE__,
                            "write_link_json wrote past cap=" + std::to_string(cap));
                break;
            }
        }
    }

    // A null peer or null counters is refused rather than dereferenced.
    LinkRecord empty{};
    CHECK_EQ(write_link_json(full, sizeof(full), empty), static_cast<size_t>(0));
    NodeRecord empty_node{};
    CHECK_EQ(write_node_json(full, sizeof(full), empty_node), static_cast<size_t>(0));
}

TEST(json, every_peer_state_and_event_kind_has_a_name) {
    // potluck-capture switches on these strings, so an unnamed enumerator would reach it as "unknown"
    // and silently lose a membership transition.
    const PeerState states[] = {PeerState::Free, PeerState::Known, PeerState::Alive,
                                PeerState::Dead, PeerState::Left};
    for (PeerState s : states) {
        CHECK(std::strcmp(peer_state_str(s), "unknown") != 0);
    }
    for (int k = 0; k <= static_cast<int>(EventKind::VersionPinned); ++k) {
        CHECK(std::strcmp(event_kind_str(static_cast<EventKind>(k)), "unknown") != 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Fixture emission. `pot_tests --emit-fixtures <dir>` writes one of each record type so the Python
// tests parse exactly what a board emits. Run from tools/run_host_tests.ps1.
// ---------------------------------------------------------------------------------------------

namespace pot_test_fixtures {

int emit(const char* dir) {
    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "stats_sample.jsonl";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    char buf[kJsonLineMax];

    BootRecord br{};
    br.node_id = 0x0102;
    br.boot_epoch = 42;
    const uint8_t mac[kMacLen] = {0x24, 0x6F, 0x28, 0x00, 0x11, 0x22};
    std::memcpy(br.mac, mac, kMacLen);
    br.espnow_version = 2;
    br.channel = 1;
    br.hb_period_ms = 100;
    br.hb_miss_limit = 6;
    br.fw_version = "m0";
    br.idf_version = "v6.0.2";
    br.dram_at_boot = 300000;
    br.dram_after_nvs = 296000;
    br.dram_after_netif = 292000;
    br.dram_after_wifi_init = 260000;
    br.dram_after_wifi_start = 258000;
    br.dram_after_espnow = 256000;
    br.dram_largest_block = 110000;
    if (write_boot_json(buf, sizeof(buf), br) > 0) std::fputs(buf, f);

    const PeerLink p = sample_peer();
    const RttHistogram h = sample_histogram();
    const LinkRecord lr = sample_link_record(p, h);
    if (write_link_json(buf, sizeof(buf), lr) > 0) std::fputs(buf, f);

    // A second link record with nothing measured yet, so the Python side sees the null case too.
    PeerLink fresh;
    fresh.reset();
    fresh.state = PeerState::Known;
    fresh.node_id = 9;
    RttHistogram empty_hist;
    empty_hist.reset();
    LinkRecord lr2{};
    lr2.uptime_ms = 200;
    lr2.node_id = 0x0102;
    lr2.peer_node_id = 9;
    lr2.peer = &fresh;
    lr2.histogram = &empty_hist;
    if (write_link_json(buf, sizeof(buf), lr2) > 0) std::fputs(buf, f);

    NodeCounters c;
    c.reset();
    c.rx_total = 5000;
    c.tx_total = 5100;
    c.deaths_declared = 2;
    c.revivals = 2;
    NodeRecord nr{};
    nr.uptime_ms = 60000;
    nr.node_id = 0x0102;
    nr.boot_epoch = 42;
    nr.counters = &c;
    nr.free_dram_now = 200000;
    nr.largest_free_block = 100000;
    nr.peers_alive = 1;
    nr.peers_dead = 1;
    if (write_node_json(buf, sizeof(buf), nr) > 0) std::fputs(buf, f);

    for (int k = 1; k <= static_cast<int>(EventKind::VersionPinned); ++k) {
        Event e{};
        e.at_ms = static_cast<uint32_t>(600 + k);
        e.kind = static_cast<EventKind>(k);
        e.peer_slot = 0;
        e.node_id = 0xCDEF;
        e.detail_a = static_cast<uint32_t>(k);
        e.detail_b = 600;
        if (write_event_json(buf, sizeof(buf), 0x0102, e) > 0) std::fputs(buf, f);
    }

    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
    return 0;
}

}  // namespace pot_test_fixtures
