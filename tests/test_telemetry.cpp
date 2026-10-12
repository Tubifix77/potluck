// M15: telemetry over the cell -- the binary sections the hub pulls (pot/telemetry.hpp).

#include <cstring>
#include <string>
#include <vector>

#include "pot/telemetry.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

TEST(telemetry, every_reply_fits_the_cables_payload_floor) {
    CHECK(kTelemetryHeaderLen + kTelemetryHealthLen <= kTelemetryReplyMax);
    CHECK(kTelemetryHeaderLen + 3 + kTelemetryLinksPerReply * kTelemetryLinkLen <= kTelemetryReplyMax);
    CHECK(kTelemetryHeaderLen + 3 + kTelemetryActorsPerReply * kTelemetryActorLen <= kTelemetryReplyMax);
    CHECK(kTelemetryHeaderLen + 5 + kTelemetryEventsPerReply * kTelemetryEventLen <= kTelemetryReplyMax);
    CHECK(kTelemetryReplyMax <= 226);
    CHECK_EQ(kTelemetryLinksPerReply, static_cast<size_t>(5));
}

TEST(telemetry, the_health_section_has_its_stated_length_and_fields_in_order) {
    TelemetryHealth h;
    h.uptime_s = 3600;
    h.boot_epoch = 344;
    h.channel = 1;
    h.flags = kTelHealthConnected | kTelHealthEnrolled;
    h.hb_period_ms = 100;
    h.int_free = 191603;
    h.ext_min = 7000000;
    h.peers_alive = 3;
    h.deaths = 2;
    h.writes_rejected = 9;
    h.link_stack_free = 1234;
    uint8_t w[kTelemetryReplyMax];
    const size_t n = store_telemetry_health(0x8160, h, w, sizeof(w));
    CHECK_EQ(n, kTelemetryHeaderLen + kTelemetryHealthLen);
    CHECK_EQ(static_cast<int>(w[0]), static_cast<int>(kTelemetryVersion));
    CHECK_EQ(static_cast<int>(w[1]), static_cast<int>(kTelemetryHealth));
    CHECK_EQ(rd16(w + 2), static_cast<uint16_t>(0x8160));
    CHECK_EQ(rd32(w + 4), 3600u);
    CHECK_EQ(rd32(w + 8), 344u);
    CHECK_EQ(static_cast<int>(w[12]), 1);
    CHECK_EQ(static_cast<int>(w[13]), static_cast<int>(kTelHealthConnected | kTelHealthEnrolled));
    CHECK_EQ(rd32(w + 16), 191603u);
    CHECK_EQ(rd32(w + 32), 7000000u);  // ext_min
    CHECK_EQ(static_cast<int>(w[36]), 3);  // peers_alive
    CHECK_EQ(rd32(w + 40), 2u);            // deaths
    CHECK_EQ(rd32(w + 84), 9u);            // writes_rejected
    CHECK_EQ(rd16(w + 88), static_cast<uint16_t>(1234));
    CHECK_EQ(store_telemetry_health(1, h, w, n - 1), static_cast<size_t>(0));
}

TEST(telemetry, a_link_carries_the_peer_tables_counters_and_rtt_percentiles) {
    PeerLink p{};
    p.reset();
    p.node_id = 0x7368;
    p.state = PeerState::Alive;
    p.version = EspNowVersion::V2;
    p.last_rssi = -45;
    p.hb_period_ms = 100;
    p.miss_limit = 60;
    p.tx_frames = 1000;
    p.tx_cb_ok = 990;
    p.tx_cb_fail = 10;
    p.rx_frames = 2000;
    p.rx_lost_seqgap = 3;
    p.rx_hb_lost_seqgap = 4;
    p.rtt_timeouts = 70000;  // saturates
    RttHistogram h{};
    h.reset();
    for (int i = 0; i < 99; ++i) h.add(5000);  // 5 ms
    h.add(20000);
    const TelemetryLink l = telemetry_link(p, h, kTelLinkVerified);
    CHECK_EQ(l.node, static_cast<uint16_t>(0x7368));
    CHECK_EQ(static_cast<int>(l.rssi), -45);
    CHECK_EQ(l.rx_lost, 7u);
    CHECK_EQ(l.rtt_timeouts, static_cast<uint16_t>(0xFFFF));
    CHECK(l.rtt_p50_us100 != 0xFFFF && l.rtt_p50_us100 >= 50);  // the 5 ms bucket's upper bound, in 100 us
    CHECK(l.rtt_p99_us100 >= l.rtt_p50_us100);
    const RttHistogram empty{};
    CHECK_EQ(telemetry_link(p, empty, 0).rtt_p50_us100, static_cast<uint16_t>(0xFFFF));  // unmeasured, not zero

    TelemetryLink many[8];
    for (int i = 0; i < 8; ++i) many[i] = l;
    uint8_t w[kTelemetryReplyMax];
    const size_t n = store_telemetry_links(0x6300, 8, 0, many, 8, w, sizeof(w));
    CHECK_EQ(n, kTelemetryHeaderLen + 3 + kTelemetryLinksPerReply * kTelemetryLinkLen);  // clipped to five
    CHECK_EQ(static_cast<int>(w[4]), 8);
    CHECK_EQ(static_cast<int>(w[6]), 5);
    CHECK_EQ(rd16(w + 7), static_cast<uint16_t>(0x7368));
    CHECK_EQ(static_cast<int8_t>(w[10]), static_cast<int8_t>(-45));
}

TEST(telemetry, the_event_log_pages_by_sequence_and_says_what_was_missed) {
    TelemetryEventLog log;
    CHECK_EQ(log.next(), 1u);
    TelemetryEvent out[kTelemetryEventsPerReply];
    CHECK_EQ(log.since(0, out, kTelemetryEventsPerReply), static_cast<size_t>(0));
    for (uint32_t i = 0; i < 20; ++i) log.push(i * 10, 3, 0x7368, i, 0);
    size_t n = log.since(0, out, kTelemetryEventsPerReply);
    CHECK_EQ(n, kTelemetryEventsPerReply);
    CHECK_EQ(out[0].seq, 1u);
    n = log.since(out[n - 1].seq, out, kTelemetryEventsPerReply);
    CHECK_EQ(n, static_cast<size_t>(9));
    CHECK_EQ(out[8].seq, 20u);
    CHECK_EQ(log.next(), 21u);
    // Past the log's size the oldest are gone: a hub that asks from 0 gets the oldest kept, and seq tells it the gap.
    for (uint32_t i = 0; i < 100; ++i) log.push(0, 1, 1, 0, 0);
    n = log.since(0, out, 1);
    CHECK_EQ(out[0].seq, log.next() - static_cast<uint32_t>(TelemetryEventLog::kSize));
    // A hub whose `since` is past `next` (the board rebooted) gets nothing, and `next` tells it to start over.
    CHECK_EQ(log.since(100000, out, kTelemetryEventsPerReply), static_cast<size_t>(0));

    uint8_t w[kTelemetryReplyMax];
    const size_t wl = store_telemetry_events(0x8160, log.next(), out, 1, w, sizeof(w));
    CHECK_EQ(wl, kTelemetryHeaderLen + 5 + kTelemetryEventLen);
    CHECK_EQ(rd32(w + 4), log.next());
}

TEST(telemetry, the_request_round_trips) {
    TelemetryRequest r;
    r.target = 0x8160;
    r.section = kTelemetryEvents;
    r.start = 2;
    r.since = 0xA1B2C3D4u;
    uint8_t w[kTelemetryRequestLen];
    CHECK_EQ(store_telemetry_request(r, w, sizeof(w)), kTelemetryRequestLen);
    TelemetryRequest s;
    CHECK(load_telemetry_request(w, sizeof(w), s));
    CHECK_EQ(s.target, static_cast<uint16_t>(0x8160));
    CHECK_EQ(s.since, 0xA1B2C3D4u);
    CHECK(!load_telemetry_request(w, sizeof(w) - 1, s));
}

namespace {
std::vector<uint8_t> unhex(const char* hex) {
    std::vector<uint8_t> v;
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2) v.push_back(static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return v;
}
}  // namespace

TEST(telemetry, the_host_reads_exactly_these_bytes) {
    // host/potluck/tests/test_telemetry.py's HEALTH_GOLDEN and LINKS_GOLDEN.
    TelemetryHealth h;
    h.uptime_s = 3600;
    h.boot_epoch = 344;
    h.channel = 1;
    h.flags = kTelHealthConnected | kTelHealthEnrolled;
    h.hb_period_ms = 100;
    h.int_free = 191603;
    h.ext_min = 7000000;
    h.peers_alive = 3;
    h.deaths = 2;
    h.writes_rejected = 9;
    h.link_stack_free = 1234;
    uint8_t w[kTelemetryReplyMax];
    size_t n = store_telemetry_health(0x8160, h, w, sizeof(w));
    CHECK(std::vector<uint8_t>(w, w + n) ==
          unhex("01016081100e0000580100000114640073ec0200000000000000000000000000c0cf6a000300000002000000000000000000"
                "0000000000000000000000000000000000000000000000000000000000000000000009000000d2040000"));
    TelemetryLink l;
    l.node = 0x7368;
    l.state = 2;
    l.rssi = -45;
    l.espnow_version = 2;
    l.flags = kTelLinkVerified;
    l.hb_period_ms = 100;
    l.miss_limit = 60;
    l.misses = 1;
    l.tx_frames = 1000;
    l.tx_cb_ok = 990;
    l.tx_cb_fail = 10;
    l.rx_frames = 2000;
    l.rx_lost = 7;
    l.rx_bcast = 40;
    l.rtt_p50_us100 = 51;
    l.rtt_p99_us100 = 160;
    l.rtt_timeouts = 0xFFFF;
    n = store_telemetry_links(0x6300, 2, 0, &l, 1, w, sizeof(w));
    CHECK(std::vector<uint8_t>(w, w + n) ==
          unhex("01020063020001687302d3020264003c01e8030000de0300000a000000d007000007000000280000003300a000ffff"));
}
