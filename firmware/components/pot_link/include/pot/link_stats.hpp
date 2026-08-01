// Per-peer link statistics — ARCHITECTURE.md §6 (memory budget), §13-M0 (what M0 must produce).
//
// M0 exists to produce three measured numbers: PDR, a delay histogram, and retry counts (§13-M0).
// This file is where they are accumulated. It has no ESP-IDF dependency so the accumulation logic
// is testable on the host; the transport feeds it.
//
// ---------------------------------------------------------------------------------------------
// DELAY METHODOLOGY — read this before adding a field
// ---------------------------------------------------------------------------------------------
// The two nodes' clocks are not synchronised. Nothing here converts between them, and nothing here
// reports a one-way delay, because a one-way figure would require either synchronised clocks
// (which M0 does not have) or an assumption of path symmetry (which ESP-NOW's retry machinery
// does not provide — a frame may sit in up to 31 retransmissions on one side and none on the
// other, §3). ARCHITECTURE.md §3's 2782.85 µs is a *cited* one-way figure from an instrumented
// experiment; it is not something this firmware can reproduce, and M0 must not pretend otherwise.
//
// What is measured, and on whose clock:
//
//   t_submit   node A   esp_now_send() returns
//   t_sendcb   node A   send callback fires — the MAC-layer ACK for our probe arrived (or failed)
//   t_recv     node B   receive callback fires
//   t_reply    node B   esp_now_send() of the reply returns
//   t_done     node A   receive callback for the reply fires
//
//   rtt_us          = t_done - t_submit        both on A's clock          ← the headline number
//   txq_us          = t_sendcb - t_submit      both on A's clock          ← local queue + ACK wait
//   turnaround_us   = t_reply - t_recv         both on B's clock          ← carried in the payload
//   net_rtt_us      = rtt_us - turnaround_us   a difference of two measured durations
//
// A duration is comparable across unsynced clocks (they differ in rate by ppm, not offset); an
// instant is not. Every quantity above is a duration on one clock. net_rtt_us is honest for the
// same reason: it subtracts a measured duration from a measured duration and is named for exactly
// what it is. There is deliberately no `one_way_us` field, and `net_rtt_us / 2` does not appear in
// this codebase or in the JSON it emits.
//
// ---------------------------------------------------------------------------------------------
// MEMORY — §6
// ---------------------------------------------------------------------------------------------
// §6 allocates "Peer table, 20 × 128 B = 2.5 KB" and, separately, "Counters, link stats, event
// ring = 2.0 KB". The RTT histogram is charged to the second line, not the first: §6's 128 B per
// peer was sized for membership and counters, and a histogram does not fit beside them. Budget:
//
//   PeerLink  × 20                      = 20 × 128 B = 2560 B   (the §6 peer-table line)
//   RttHistogram × 20                   = 20 ×  64 B = 1280 B  ┐
//   NodeCounters                        =         64 B          ├ the §6 2.0 KB stats line
//   EventRing 32 × 16 B                 =        512 B         ┘  → 1856 B of 2048 B
//
// static_asserts at the bottom of this file hold those numbers. If a future field breaks one, the
// budget is the thing that decides, not the field.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

// ---------------------------------------------------------------------------------------------
// RTT histogram. Bucket edges come from the measured ESP-NOW distribution in §3 rather than from a
// tidy power-of-two ladder: the interesting structure is a ~2.8 ms floor, a tail to 59 ms, and an
// in-protocol retry ceiling near 104 ms (31 × 3350 µs), so the buckets are dense where the samples
// will be and coarse where they will not.
// ---------------------------------------------------------------------------------------------

constexpr size_t kRttBuckets = 16;

// Upper edge of each bucket, in µs; the last bucket is everything above the previous edge.
// A sample lands in the first bucket whose edge it does not exceed.
constexpr uint32_t kRttBucketEdgeUs[kRttBuckets] = {
    1000,    // sub-ms: only plausible on a bench with both boards touching
    2000,    //
    3000,    // §3's 2782.85 µs best-case one-way lives here; an RTT below it would be suspicious
    4000,    //
    6000,    // §3's 3461.65 µs mean at 52 m doubles into roughly here
    8000,    //
    11000,   //
    16000,   //
    22000,   // §3's 25628 µs max at 99.85% PDR is just above
    30000,   //
    42000,   //
    60000,   // §3's 59192 µs max at 83.2% PDR is just below
    85000,   //
    110000,  // §3's ~104 ms in-protocol retry ceiling (31 × 3350 µs)
    200000,  // beyond the protocol's own patience: something else is wrong
    0xFFFFFFFFu,
};

struct RttHistogram {
    uint32_t bucket[kRttBuckets];

    void reset();
    void add(uint32_t us);
    uint32_t count() const;

    // Percentile as the bucket interval containing it, not a single number. A histogram cannot
    // support more precision than its buckets, and inventing an interpolated value would be
    // exactly the kind of fabricated figure §13-M0 forbids. Returns false when there are no
    // samples. On success lo_us/hi_us bracket the percentile; hi_us is 0xFFFFFFFF for the
    // overflow bucket.
    bool percentile(uint8_t pct, uint32_t& lo_us, uint32_t& hi_us) const;
};

static_assert(sizeof(RttHistogram) == 64, "§6: 16 × u32 = 64 B per peer on the stats line");

// ---------------------------------------------------------------------------------------------
// Peer link state and counters. Times are milliseconds since boot in a uint32_t: unsigned
// subtraction stays correct across the 49.7-day wrap as long as the interval measured is shorter
// than that, which every interval here is (the longest is a 600 ms death timer). RTT samples are
// microseconds because that is the resolution the measurement has.
// ---------------------------------------------------------------------------------------------

enum class PeerState : uint8_t {
    Free = 0,   // slot unused
    Known = 1,  // HELLO seen or configured, no heartbeat yet
    Alive = 2,  // heartbeats arriving within the miss limit
    Dead = 3,   // miss limit reached (§8.2); the slot and its stats are kept
    Left = 4,   // BYE received — departed on purpose, which is not a failure
};

const char* peer_state_str(PeerState s);

// ESP-NOW version pinned for this peer — §5.3. Unknown until HELLO or HELLO_ACK settles it, and
// the payload cap stays at the v1 floor until then, because guessing v2 and being wrong means a
// silently truncated frame.
enum class EspNowVersion : uint8_t {
    Unknown = 0,
    V1 = 1,
    V2 = 2,
};

constexpr size_t kMacLen = 6;

struct PeerLink {
    // --- identity -------------------------------------------------------------------------
    uint8_t mac[kMacLen];
    uint16_t node_id;
    uint32_t boot_epoch;  // peer's incarnation; a change means it rebooted, not that it was late

    // --- liveness (§8.2) ------------------------------------------------------------------
    uint32_t last_rx_ms;    // arrival of the most recent accepted frame from this peer
    uint32_t hb_period_ms;  // this link's heartbeat period; 100 ms on ESP-NOW
    uint32_t hb_seq_last;    // peer's heartbeat counter, for heartbeat-specific gap detection
    uint32_t hb_seq_last_tx; // our own heartbeat counter towards this peer

    // --- outbound -------------------------------------------------------------------------
    uint32_t tx_frames;       // submitted to the transport
    uint32_t tx_cb_ok;        // send callback reported a MAC-layer ACK
    uint32_t tx_cb_fail;      // send callback reported failure
    uint32_t tx_enqueue_err;  // esp_now_send() itself refused — a different failure from a lost
                              // frame, and conflating the two would corrupt the PDR figure

    // --- inbound --------------------------------------------------------------------------
    uint32_t rx_frames;         // accepted
    uint32_t rx_lost_seqgap;    // inferred from seq gaps (§5.1: seq is per (src,dst) monotonic)
    uint32_t rx_reorder_dup;    // seq moved backwards: a duplicate or a reorder, never a loss
    uint32_t rx_dropped_bad;    // failed parse() — counted, never silently discarded
    uint32_t rx_hb_lost_seqgap; // heartbeat-specific gaps, from hb_seq

    // --- RTT (see the methodology note above) ---------------------------------------------
    uint32_t rtt_samples;
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint32_t rtt_timeouts;         // probe never answered before the next probe replaced it
    uint32_t txq_last_us;          // t_sendcb - t_submit for the most recent probe
    uint32_t txq_max_us;
    uint32_t remote_turnaround_last_us;  // as reported by the peer, on the peer's own clock
    uint32_t remote_turnaround_max_us;

    // --- outstanding probe ----------------------------------------------------------------
    // Microseconds, not milliseconds: an RTT is measured in µs and a uint32_t of µs wraps only
    // every 71.6 minutes, which is far longer than any interval compared here. Unsigned
    // subtraction stays correct across that wrap.
    uint32_t probe_submit_us;  // when the outstanding probe was submitted
    uint32_t probe_sendcb_us;  // 0 until the send callback for it fires
    uint32_t owed_recv_us;     // arrival of a probe we have not answered, for turnaround_us

    // --- sequencing -----------------------------------------------------------------------
    uint16_t probe_msg_id;  // 0 = no probe outstanding
    uint16_t seq_tx;        // next seq to use towards this peer
    uint16_t seq_rx_last;   // last seq accepted from this peer
    uint16_t msg_id_next;   // next correlation id
    uint16_t owed_msg_id;   // msg_id of a received probe not yet answered; 0 = none

    // --- state ----------------------------------------------------------------------------
    // The byte-wide fields are grouped here so the struct has no implicit padding: an unnamed
    // padding byte in a table this size is 20 bytes nobody is accounting for.
    PeerState state;
    EspNowVersion version;
    uint8_t misses;      // whole heartbeat periods since last_rx_ms
    uint8_t miss_limit;  // 6 on ESP-NOW (§8.2)
    int8_t last_rssi;    // from wifi_pkt_rx_ctrl_t; correlates loss with §3's range cliff
    bool seq_rx_valid;   // false until the first frame, so a peer's first seq is not a phantom gap

    // Broadcast beacons accepted from this peer. §5.1 makes `seq` per (src,dst), so a sender's
    // broadcast stream and its unicast stream are two different sequences — mixing them into one
    // seq_rx_last would manufacture a gap on every alternation. Broadcast loss is therefore counted
    // from the payload's hb_seq into rx_hb_lost_seqgap, and this is its denominator.
    uint32_t rx_bcast_frames;

    // Payload cap for this peer, derived from `version` — §5.3. The v1 floor until proven v2.
    uint16_t max_payload() const;

    // Packet delivery ratio, in parts per million so that no float appears in firmware. Returns
    // false when the denominator is zero: an unmeasured link reports "unknown", not 100%.
    bool pdr_tx_ppm(uint32_t& ppm) const;
    bool pdr_rx_ppm(uint32_t& ppm) const;

    void reset();
};

// Exactly the §6 allocation, with every byte named. The equality is asserted rather than the
// inequality on purpose: a field added without thinking about the budget should fail the build,
// and "<= 128" would let the table silently absorb three more counters and then break on the
// fourth, by which time it is not obvious which one was the mistake.
static_assert(sizeof(PeerLink) == 128, "§6: the peer table is 128 B per peer, exactly");

// ---------------------------------------------------------------------------------------------
// Node-wide counters. Things that are not per-peer: frames that failed to parse before a peer
// could be identified, frames from unknown peers, and the Wi-Fi-stack DRAM measurement (§6's
// [MEASURE] item) so it travels with the rest of the statistics.
// ---------------------------------------------------------------------------------------------

struct NodeCounters {
    uint32_t rx_total;
    uint32_t rx_bad_frame;      // parse() rejected before a peer was known
    uint32_t rx_unknown_peer;   // well-formed, from a MAC not in the peer table
    uint32_t rx_wrong_dst;      // addressed to another node id and not broadcast
    uint32_t rx_short_payload;  // opcode's fixed payload did not arrive in full
    uint32_t rx_unknown_opcode;
    uint32_t tx_total;
    uint32_t tx_enqueue_err;
    uint32_t peer_table_full;
    uint32_t deaths_declared;  // §8.2 death declarations since boot
    uint32_t revivals;         // peers that came back
    uint32_t reboots_seen;     // peers whose boot_epoch advanced under us

    // Frames carrying an epoch *older* than the newest already seen from that peer. ESP-NOW retries
    // for up to ~104 ms (§3), so a frame sent before a peer rebooted can arrive after one sent
    // afterwards. Those must be ignored rather than treated as another reboot — see peer_on_frame().
    uint32_t rx_stale_epoch;

    // The bring-up DRAM readings live in DramProfile (dram_probe.hpp), which is where the boot
    // record reads them; duplicating them here served nothing. Kept as named reserve so the
    // structure stays exactly 64 B against §6 rather than silently shrinking.
    uint32_t reserved0[2];
    uint32_t free_dram_now;

    void reset();
};

static_assert(sizeof(NodeCounters) == 64, "§6: 64 B on the stats line");

// ---------------------------------------------------------------------------------------------
// Event ring. §4 rule 4 wants demotion to be loud, and §5.2 says an ERR is never silently
// dropped; both need somewhere to put an event that the JSON dump has not carried yet. Fixed
// size, overwrite-oldest: a full ring must not be able to block a heartbeat.
// ---------------------------------------------------------------------------------------------

enum class EventKind : uint8_t {
    None = 0,
    PeerDiscovered = 1,
    PeerAdmitted = 2,
    PeerAlive = 3,
    PeerDead = 4,      // §8.2 death declaration
    PeerRevived = 5,
    PeerRebooted = 6,  // boot_epoch changed
    PeerLeft = 7,      // BYE
    BadFrame = 8,
    TxError = 9,
    ProbeTimeout = 10,
    VersionPinned = 11,
};

const char* event_kind_str(EventKind k);

struct Event {
    uint32_t at_ms;
    EventKind kind;
    uint8_t peer_slot;
    uint16_t node_id;
    uint32_t detail_a;
    uint32_t detail_b;
};

static_assert(sizeof(Event) == 16, "§6: 16 B per event");

constexpr size_t kEventRingSize = 32;

class EventRing {
  public:
    void reset();
    void push(const Event& e);

    // Pop the oldest unread event. Returns false when empty.
    bool pop(Event& out);

    uint32_t dropped() const { return dropped_; }
    size_t size() const;

  private:
    Event buf_[kEventRingSize]{};
    uint32_t head_ = 0;  // next write
    uint32_t tail_ = 0;  // next read
    uint32_t dropped_ = 0;
};

// ---------------------------------------------------------------------------------------------
// §6 budget arithmetic, asserted rather than asserted-to.
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxPeers = 20;  // the ESP-NOW peer ceiling, §3

constexpr size_t kPeerTableBytes = sizeof(PeerLink) * kMaxPeers;
constexpr size_t kStatsLineBytes =
    sizeof(RttHistogram) * kMaxPeers + sizeof(NodeCounters) + sizeof(Event) * kEventRingSize;

static_assert(kPeerTableBytes <= 2560, "§6: peer table budget is 20 × 128 B = 2.5 KB");
static_assert(kStatsLineBytes <= 2048, "§6: counters + link stats + event ring budget is 2.0 KB");

// ---------------------------------------------------------------------------------------------
// Free functions the transport and the tests share.
// ---------------------------------------------------------------------------------------------

// Fold a newly received seq into the peer's inbound accounting. §5.1 says seq is per (src,dst)
// monotonic and wraps, so the delta is interpreted as a signed 16-bit quantity: forward means
// `delta - 1` frames were lost, backwards or equal means a duplicate or a reorder. Treating a
// backwards delta as a 65000-frame loss is the classic wrap bug, so it is tested for explicitly.
void account_rx_seq(PeerLink& p, uint16_t seq);

// Fold a heartbeat's hb_seq in the same way, into the heartbeat-specific counter.
void account_rx_hb_seq(PeerLink& p, uint32_t hb_seq);

// Record a completed RTT measurement. `histogram` may be null when a peer has no histogram slot.
void account_rtt(PeerLink& p, RttHistogram* histogram, uint32_t rtt_us);

}  // namespace pot
