// Link statistics tests — the numbers M0 exists to produce (§13-M0).
//
// A statistics bug is worse than a crash here, because it does not announce itself: the 24-hour
// soak completes, publishes a PDR, and the figure is wrong. Every accumulator below is therefore
// tested against a case where the naive implementation gives a plausible but false answer.

#include "pot/link_stats.hpp"
#include "pot/membership.hpp"
#include "pot/node.hpp"
#include "pot/payloads.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

PeerLink fresh_peer() {
    PeerLink p;
    p.reset();
    p.state = PeerState::Alive;
    p.hb_period_ms = kHbPeriodWirelessMs;
    p.miss_limit = kHbMissLimitWireless;
    return p;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Inbound loss from sequence gaps
// ------------------------------------------------------------------------------------------

TEST(stats, first_frame_establishes_a_baseline_and_reports_no_loss) {
    // A peer whose first seq we see is 5000 has not lost 5000 frames to us; we simply were not
    // listening. Without the seq_rx_valid flag this is the commonest way to invent packet loss.
    PeerLink p = fresh_peer();
    account_rx_seq(p, 5000);
    CHECK_EQ(p.rx_frames, 1u);
    CHECK_EQ(p.rx_lost_seqgap, 0u);
    CHECK(p.seq_rx_valid);
}

TEST(stats, consecutive_sequence_numbers_are_lossless) {
    PeerLink p = fresh_peer();
    for (int i = 0; i < 1000; ++i) {
        account_rx_seq(p, static_cast<uint16_t>(i));
    }
    CHECK_EQ(p.rx_frames, 1000u);
    CHECK_EQ(p.rx_lost_seqgap, 0u);
    CHECK_EQ(p.rx_reorder_dup, 0u);
}

TEST(stats, a_gap_counts_exactly_the_missing_frames) {
    PeerLink p = fresh_peer();
    account_rx_seq(p, 10);
    account_rx_seq(p, 14);  // 11, 12, 13 are missing → 3, not 4
    CHECK_EQ(p.rx_lost_seqgap, 3u);
    CHECK_EQ(p.rx_frames, 2u);
}

TEST(stats, sequence_wrap_is_not_a_65000_frame_loss) {
    // §5.1: seq wraps. Treating 0xFFFF → 0x0000 as a backwards jump of 65535 would report a
    // catastrophic loss every 65536 frames — which at 100 ms is once every 1.8 hours, i.e. about
    // thirteen times during the M0 soak.
    PeerLink p = fresh_peer();
    account_rx_seq(p, 0xFFFE);
    account_rx_seq(p, 0xFFFF);
    account_rx_seq(p, 0x0000);
    account_rx_seq(p, 0x0001);
    CHECK_EQ(p.rx_lost_seqgap, 0u);
    CHECK_EQ(p.rx_reorder_dup, 0u);
    CHECK_EQ(p.rx_frames, 4u);

    // A real gap that straddles the wrap is still counted correctly.
    PeerLink q = fresh_peer();
    account_rx_seq(q, 0xFFFD);
    account_rx_seq(q, 0x0002);  // 0xFFFE, 0xFFFF, 0x0000, 0x0001 lost → 4
    CHECK_EQ(q.rx_lost_seqgap, 4u);
}

TEST(stats, a_duplicate_is_not_a_loss_and_does_not_rewind_the_baseline) {
    // ESP-NOW retransmits up to 31 times (§3), so duplicates are expected. If a duplicate moved
    // seq_rx_last backwards, the next in-order frame would be counted as an enormous gap.
    PeerLink p = fresh_peer();
    account_rx_seq(p, 100);
    account_rx_seq(p, 101);
    account_rx_seq(p, 101);  // duplicate
    account_rx_seq(p, 100);  // late reorder
    account_rx_seq(p, 102);
    CHECK_EQ(p.rx_lost_seqgap, 0u);
    CHECK_EQ(p.rx_reorder_dup, 2u);
    CHECK_EQ(p.seq_rx_last, static_cast<uint16_t>(102));
    CHECK_EQ(p.rx_frames, 5u);
}

TEST(stats, heartbeat_sequence_gaps_are_tracked_separately) {
    // hb_seq counts heartbeats only, so the heartbeat PDR stays meaningful once M1 adds other
    // traffic to the same link.
    PeerLink p = fresh_peer();
    account_rx_hb_seq(p, 1);
    account_rx_hb_seq(p, 2);
    account_rx_hb_seq(p, 6);  // 3, 4, 5 missing
    CHECK_EQ(p.rx_hb_lost_seqgap, 3u);
    CHECK_EQ(p.hb_seq_last, 6u);

    // Out-of-order heartbeats must not rewind the high-water mark.
    account_rx_hb_seq(p, 4);
    CHECK_EQ(p.hb_seq_last, 6u);
    CHECK_EQ(p.rx_hb_lost_seqgap, 3u);
}

// ------------------------------------------------------------------------------------------
// PDR
// ------------------------------------------------------------------------------------------

TEST(stats, pdr_is_unknown_before_any_traffic) {
    // An unmeasured link reports "unknown", never 100%. §13-M0 wants a measured PDR, and a
    // default of 1.0 would be a fabricated one.
    PeerLink p = fresh_peer();
    uint32_t ppm = 0xDEADBEEF;
    CHECK(!p.pdr_tx_ppm(ppm));
    CHECK(!p.pdr_rx_ppm(ppm));
}

TEST(stats, outbound_pdr_comes_from_the_send_callback) {
    PeerLink p = fresh_peer();
    p.tx_cb_ok = 999;
    p.tx_cb_fail = 1;
    uint32_t ppm = 0;
    CHECK(p.pdr_tx_ppm(ppm));
    CHECK_EQ(ppm, 999000u);  // 99.9%
}

TEST(stats, a_local_enqueue_error_does_not_count_against_the_radio) {
    // esp_now_send() refusing a frame (ESP_ERR_ESPNOW_NO_MEM, say) means the frame never went out.
    // Charging it to the link would blame the radio for a local queue overflow and make the PDR
    // figure describe the wrong thing.
    PeerLink p = fresh_peer();
    p.tx_frames = 1000;
    p.tx_cb_ok = 900;
    p.tx_cb_fail = 0;
    p.tx_enqueue_err = 100;
    uint32_t ppm = 0;
    CHECK(p.pdr_tx_ppm(ppm));
    CHECK_EQ(ppm, 1000000u);  // the 900 that were transmitted all arrived

    // The enqueue errors are still visible; they are just a different number.
    CHECK_EQ(p.tx_enqueue_err, 100u);
    CHECK_EQ(p.tx_frames - p.tx_cb_ok - p.tx_cb_fail - p.tx_enqueue_err, 0u);
}

TEST(stats, inbound_pdr_comes_from_sequence_gaps) {
    PeerLink p = fresh_peer();
    p.rx_frames = 950;
    p.rx_lost_seqgap = 50;
    uint32_t ppm = 0;
    CHECK(p.pdr_rx_ppm(ppm));
    CHECK_EQ(ppm, 950000u);  // 95%
}

TEST(stats, pdr_survives_a_24_hour_soak_worth_of_frames) {
    // 24 h at 100 ms is 864,000 heartbeats each way. The ppm arithmetic must not overflow: a naive
    // `ok * 1000000 / total` in 32-bit arithmetic overflows at 4,295 frames.
    PeerLink p = fresh_peer();
    p.tx_cb_ok = 863999;
    p.tx_cb_fail = 1;
    uint32_t ppm = 0;
    CHECK(p.pdr_tx_ppm(ppm));
    CHECK_EQ(ppm, 999998u);

    p.tx_cb_ok = 4000000000u;  // well past a 24-hour soak, to prove the widening is real
    p.tx_cb_fail = 0;
    CHECK(p.pdr_tx_ppm(ppm));
    CHECK_EQ(ppm, 1000000u);
}

// ------------------------------------------------------------------------------------------
// RTT histogram
// ------------------------------------------------------------------------------------------

TEST(stats, histogram_buckets_are_monotonic_and_cover_everything) {
    for (size_t i = 1; i < kRttBuckets; ++i) {
        CHECK(kRttBucketEdgeUs[i] > kRttBucketEdgeUs[i - 1]);
    }
    CHECK_EQ(kRttBucketEdgeUs[kRttBuckets - 1], 0xFFFFFFFFu);

    // The distribution §3 measured has to land inside the interesting part of the range, or the
    // buckets are decoration. Anything above the last finite edge means something is wrong.
    RttHistogram h;
    h.reset();
    const uint32_t interesting[] = {2783, 3462, 7851, 25628, 59192, 103850};
    for (uint32_t us : interesting) {
        h.add(us);
    }
    CHECK_EQ(h.count(), 6u);
    CHECK_EQ(h.bucket[kRttBuckets - 1], 0u);  // nothing in the overflow bucket
}

TEST(stats, histogram_places_samples_on_the_right_side_of_an_edge) {
    // A sample equal to an edge belongs to that bucket, one microsecond more to the next.
    RttHistogram h;
    h.reset();
    h.add(1000);  // == edge[0]
    h.add(1001);  // > edge[0]
    CHECK_EQ(h.bucket[0], 1u);
    CHECK_EQ(h.bucket[1], 1u);

    h.reset();
    h.add(0);
    CHECK_EQ(h.bucket[0], 1u);

    h.reset();
    h.add(0xFFFFFFFFu);
    CHECK_EQ(h.bucket[kRttBuckets - 1], 1u);
    CHECK_EQ(h.count(), 1u);
}

TEST(stats, percentiles_are_reported_as_intervals_not_invented_numbers) {
    // A histogram knows which bucket a percentile falls in and nothing finer. Interpolating
    // inside the bucket would produce a precise-looking figure that no measurement supports —
    // exactly what §13-M0 means by "measured on your bench, not cited".
    RttHistogram h;
    h.reset();
    for (int i = 0; i < 90; ++i) h.add(2500);    // bucket 2: (2000, 3000]
    for (int i = 0; i < 10; ++i) h.add(50000);   // bucket 11: (42000, 60000]

    uint32_t lo = 0, hi = 0;
    CHECK(h.percentile(50, lo, hi));
    CHECK_EQ(lo, 2000u);
    CHECK_EQ(hi, 3000u);

    CHECK(h.percentile(99, lo, hi));
    CHECK_EQ(lo, 42000u);
    CHECK_EQ(hi, 60000u);

    CHECK(h.percentile(90, lo, hi));
    CHECK_EQ(lo, 2000u);
    CHECK_EQ(hi, 3000u);
}

TEST(stats, percentile_of_an_empty_histogram_is_refused) {
    RttHistogram h;
    h.reset();
    uint32_t lo = 123, hi = 456;
    CHECK(!h.percentile(50, lo, hi));
    CHECK_EQ(lo, 123u);  // out-params untouched, so a caller cannot mistake a stale value for data
    CHECK_EQ(hi, 456u);
}

TEST(stats, percentile_edges) {
    RttHistogram h;
    h.reset();
    h.add(500);
    h.add(1500);
    h.add(2500);
    h.add(3500);

    uint32_t lo = 0, hi = 0;
    CHECK(h.percentile(0, lo, hi));
    CHECK_EQ(hi, 1000u);  // p0 is the smallest sample's bucket
    CHECK(h.percentile(100, lo, hi));
    CHECK_EQ(hi, 4000u);  // p100 is the largest sample's bucket
    CHECK(h.percentile(200, lo, hi));  // clamped to 100
    CHECK_EQ(hi, 4000u);
}

TEST(stats, histogram_counters_saturate_rather_than_wrap) {
    RttHistogram h;
    h.reset();
    h.bucket[3] = 0xFFFFFFFEu;
    h.add(3500);
    CHECK_EQ(h.bucket[3], 0xFFFFFFFFu);
    h.add(3500);
    CHECK_EQ(h.bucket[3], 0xFFFFFFFFu);  // stays pinned rather than becoming zero
}

TEST(stats, rtt_min_max_and_sample_count) {
    PeerLink p = fresh_peer();
    RttHistogram h;
    h.reset();

    CHECK_EQ(p.rtt_min_us, 0xFFFFFFFFu);  // reset() must not leave min at 0, or nothing beats it

    account_rtt(p, &h, 5000);
    account_rtt(p, &h, 3000);
    account_rtt(p, &h, 9000);
    CHECK_EQ(p.rtt_samples, 3u);
    CHECK_EQ(p.rtt_min_us, 3000u);
    CHECK_EQ(p.rtt_max_us, 9000u);
    CHECK_EQ(h.count(), 3u);

    // A null histogram is legal — the scalars still accumulate.
    account_rtt(p, nullptr, 1000);
    CHECK_EQ(p.rtt_samples, 4u);
    CHECK_EQ(p.rtt_min_us, 1000u);
    CHECK_EQ(h.count(), 3u);
}

// ------------------------------------------------------------------------------------------
// Quantisation used by the HEARTBEAT payload
// ------------------------------------------------------------------------------------------

TEST(stats, rtt_quantisation_never_collides_with_the_unknown_sentinel) {
    CHECK_EQ(quantise_us_d8(0), static_cast<uint16_t>(0));
    CHECK_EQ(quantise_us_d8(8), static_cast<uint16_t>(1));
    CHECK_EQ(quantise_us_d8(15), static_cast<uint16_t>(1));  // truncates, as ÷8 does
    CHECK_EQ(quantise_us_d8(3000), static_cast<uint16_t>(375));
    CHECK_EQ(dequantise_us_d8(375), 3000u);

    // A huge sample clamps to one below the sentinel rather than becoming "unknown".
    CHECK_EQ(quantise_us_d8(0xFFFFFFFFu), static_cast<uint16_t>(kRttUnknownD8 - 1));
    CHECK_EQ(quantise_us_d8(kRttUnknownD8 * 8u), static_cast<uint16_t>(kRttUnknownD8 - 1));
    CHECK(quantise_us_d8(0xFFFFFFFFu) != kRttUnknownD8);
    CHECK_EQ(dequantise_us_d8(kRttUnknownD8), 0u);

    // The representable range comfortably covers §3's ~104 ms retry ceiling.
    CHECK(dequantise_us_d8(kRttUnknownD8 - 1) > 500000u);
}

// ------------------------------------------------------------------------------------------
// Event ring
// ------------------------------------------------------------------------------------------

TEST(stats, event_ring_fifo) {
    EventRing r;
    r.reset();
    Event out{};
    CHECK(!r.pop(out));

    for (uint32_t i = 0; i < 5; ++i) {
        Event e{};
        e.at_ms = i;
        e.kind = EventKind::PeerAlive;
        r.push(e);
    }
    CHECK_EQ(r.size(), static_cast<size_t>(5));
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(r.pop(out));
        CHECK_EQ(out.at_ms, i);
    }
    CHECK(!r.pop(out));
    CHECK_EQ(r.dropped(), 0u);
}

TEST(stats, event_ring_overwrites_oldest_and_counts_the_loss) {
    // A full event ring must never stall a heartbeat, so it drops rather than blocks — but a
    // silent drop would make the JSON stream understate how much happened.
    EventRing r;
    r.reset();
    for (uint32_t i = 0; i < kEventRingSize + 10; ++i) {
        Event e{};
        e.at_ms = i;
        e.kind = EventKind::PeerDead;
        r.push(e);
    }
    CHECK_EQ(r.size(), kEventRingSize);
    CHECK_EQ(r.dropped(), 10u);

    // What survives is the newest kEventRingSize events, oldest of them first.
    Event out{};
    CHECK(r.pop(out));
    CHECK_EQ(out.at_ms, 10u);
}

// ------------------------------------------------------------------------------------------
// §6 budget
// ------------------------------------------------------------------------------------------

TEST(budget, structures_fit_their_section_6_allocations) {
    // These duplicate static_asserts in the headers on purpose: a static_assert stops the build,
    // but a test states the budget in the test report where the M0 acceptance check will read it.
    CHECK(sizeof(PeerLink) <= 128);
    CHECK_EQ(sizeof(RttHistogram), static_cast<size_t>(64));
    CHECK_EQ(sizeof(NodeCounters), static_cast<size_t>(64));
    CHECK_EQ(sizeof(Event), static_cast<size_t>(16));

    CHECK(kPeerTableBytes <= 2560);   // §6: "Peer table, 20 × 128 B | 2.5 KB"
    CHECK(kStatsLineBytes <= 2048);   // §6: "Counters, link stats, event ring | 2.0 KB"

    // The outstanding-request table. Section 6 has no line for it: it is charged against the
    // 12.1 KB the budget still had spare, and it is the only static allocation section 7.8's
    // request side makes. Pinned here so a later change to the table cannot enlarge it quietly --
    // one slot per peer the cell can hold is the reason it is this size, not a round number.
    CHECK_EQ(Node::pending_table_bytes(), static_cast<size_t>(320));
    CHECK_EQ(Node::max_calls_outstanding(), kMaxPeers);

    // The whole peer table plus statistics, which is what the two §6 lines buy.
    CHECK(kPeerTableBytes + kStatsLineBytes <= 2560 + 2048);
}

TEST(budget, wire_payloads_fit_the_v1_profile_unfragmented) {
    // §5.4 forbids fragmenting HELLO and HEARTBEAT, so they have to fit the 226-byte floor even
    // when talking to a v1.0 peer (§5.3).
    CHECK(sizeof(HelloPayload) <= kMaxPayloadV1);
    CHECK(sizeof(HeartbeatPayload) <= kMaxPayloadV1);
    CHECK(sizeof(HelloAckPayload) <= kMaxPayloadV1);
    CHECK(sizeof(ByePayload) <= kMaxPayloadV1);

    // And the whole frame fits an ESP-NOW v1 transmission with the auth tag reserved.
    CHECK(kHeaderSize + sizeof(HeartbeatPayload) + kAuthTagSize <= kEspNowV1LinkMtu);
}
