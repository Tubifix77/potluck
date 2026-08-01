// The Potluck node policy — membership, heartbeat, probing, statistics.
//
// This is the behaviour that used to live in firmware/main/m0_main.cpp, lifted out of ESP-IDF so
// that it can run somewhere other than a board. Two callers now:
//
//   firmware/main/m0_main.cpp   thin glue: ESP-NOW callbacks in, esp_now_send out
//   sim/                        N of these over a modelled link, on a virtual clock
//
// The reason is not tidiness. §13-M0's numbers cannot be produced without hardware, but the
// *policy* that produces them — when to beacon, when to probe, when to declare a peer dead — is
// exactly where a scaling bug hides, and a bug found in a simulator three days before the boards
// arrive costs nothing. Running a copy of the logic would prove nothing, so the firmware and the
// simulator run this same object.
//
// No ESP-IDF, no FreeRTOS, no heap, no time source of its own. Everything comes through NodeHal.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/frame.hpp"
#include "pot/link_stats.hpp"
#include "pot/membership.hpp"
#include "pot/namespace.hpp"
#include "pot/ns_payloads.hpp"
#include "pot/payloads.hpp"

namespace pot {

// ---------------------------------------------------------------------------------------------
// Environment. A struct of function pointers rather than an abstract class: no vtable, no RTTI,
// and it is callable from C glue if a later port needs that. `ctx` is the caller's own state.
// ---------------------------------------------------------------------------------------------

struct NodeHal {
    void* ctx = nullptr;

    // Submit a frame. `mac` is the destination, or kBroadcastMacAddr for a broadcast. Returns the
    // transport's error code: 0 for accepted, anything else counts as an enqueue failure. Accepted
    // means queued, never delivered — delivery is what on_tx_done reports.
    int32_t (*send)(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) = nullptr;

    // Add a peer to the transport's own peer list, if it has one. ESP-NOW requires this before a
    // unicast can be sent. May be null when the transport does not care.
    bool (*add_peer)(void* ctx, const uint8_t mac[kMacLen]) = nullptr;

    uint32_t (*now_ms)(void* ctx) = nullptr;
    uint32_t (*now_us)(void* ctx) = nullptr;

    // Free internal DRAM, for the HEARTBEAT payload and the node record. May be null.
    uint32_t (*free_dram)(void* ctx) = nullptr;

    // Membership transitions and errors, as they happen. May be null; they are also queued into the
    // node's event ring regardless, so a null here loses nothing but immediacy.
    void (*on_event)(void* ctx, const Event& e) = nullptr;

    // Human-readable log line. May be null.
    void (*log)(void* ctx, const char* msg) = nullptr;
};

// The link-layer broadcast address. §5.1's dst 0xFFFF is a Potluck node id; this is its transport
// equivalent, and on ESP-NOW it must be registered as a peer before anything can be broadcast.
extern const uint8_t kBroadcastMacAddr[kMacLen];

// ---------------------------------------------------------------------------------------------
// How liveness is carried. This is the §8.2 scaling fix, made switchable so the simulator can
// measure both rather than take the fix on trust.
// ---------------------------------------------------------------------------------------------

enum class BeaconMode : uint8_t {
    // Unicast a probe to every peer every period, each answered immediately. What M0 shipped, and
    // what two boards could never show to be wrong: it costs 2·N·(N−1) frames per period, which at
    // §3's 1 Mbit/s does not fit the channel beyond a handful of nodes. Retained so the simulator
    // can reproduce the saturation rather than assert it.
    UnicastFullMesh = 0,

    // One broadcast beacon per node per period carries liveness to every peer — O(N) instead of
    // O(N²) — and RTT/PDR ride a separate round-robin unicast probe at their own, slower interval.
    // §8.2's period, miss limit and 600 ms death declaration are unchanged; only the transport of
    // the beacon differs.
    BroadcastBeacon = 1,
};

const char* beacon_mode_str(BeaconMode m);

struct NodeConfig {
    uint16_t node_id = 0;
    uint32_t boot_epoch = 0;
    uint8_t mac[kMacLen] = {};
    uint8_t espnow_version = 2;

    // §8.2, wireless row.
    uint32_t hb_period_ms = kHbPeriodWirelessMs;
    uint8_t hb_miss_limit = kHbMissLimitWireless;

    uint32_t hello_interval_ms = 2000;

    BeaconMode beacon_mode = BeaconMode::BroadcastBeacon;

    // Round-robin unicast probe, used only in BroadcastBeacon mode. One peer per interval, so a
    // cell of N nodes costs 2N frames per interval regardless of N. RTT is a diagnostic, not a
    // liveness signal, so this does not need to be the heartbeat period: at one second, a
    // 20-node cell still yields 4,320 samples per link over a 24-hour soak.
    uint32_t probe_interval_ms = 1000;

    // A probe with no reply by this deadline is counted as a timeout and the slot freed. Must
    // exceed §3's ~104 ms in-protocol retry window or a frame still legitimately in flight would be
    // written off.
    uint32_t probe_timeout_ms = 500;

    // A namespace request nobody answered. Longer than probe_timeout_ms because a READ may
    // have to wait on the owner's own scheduling, not only on the link.
    uint32_t ns_request_timeout_ms = 2000;
};

// ---------------------------------------------------------------------------------------------
// The node.
// ---------------------------------------------------------------------------------------------

class Node {
  public:
    Node(const NodeConfig& cfg, const NodeHal& hal);

    // Announce ourselves. Call once, after the transport is up.
    void start();

    // A frame arrived. `recv_us` is the arrival instant on *our* clock, taken as close to the radio
    // as the transport can manage — the RTT measurement is only as good as this timestamp.
    void on_rx(const uint8_t src_mac[kMacLen], const uint8_t* data, size_t len, uint32_t recv_us,
               int8_t rssi);

    // The transport reported a send completion. On ESP-NOW `ok` is the 802.11 MAC-layer ACK, which
    // is what makes it usable as an outbound-delivery signal.
    void on_tx_done(const uint8_t dst_mac[kMacLen], bool ok, uint32_t done_us);

    // Time passed. Safe to call more or less often than the period: everything inside is scheduled
    // against absolute deadlines, and §8.2's miss counting derives from elapsed time rather than
    // from how often this was called.
    void tick(uint32_t now_ms);

    // Milliseconds until tick() next has work to do, so a caller can sleep exactly that long.
    uint32_t next_deadline_in_ms(uint32_t now_ms) const;

    // Announce an intentional departure (§5.2) and stop participating. `rejoin()` undoes it.
    void depart();
    void rejoin();
    bool departed() const { return departed_; }

    // ---- M1: the namespace (§7.2) --------------------------------------------------------
    // Resources this node owns, plus cached copies of remote ones. A read of a local path is
    // answered from here directly; a read of a remote path becomes a READ frame.
    Namespace& ns() { return ns_; }
    const Namespace& ns() const { return ns_; }

    // Read a path. Local resources answer synchronously and completely. A remote path answers from
    // cache — with its true age and quality, per §4 rule 2 — and returns `false` in `is_local` so
    // the caller knows a request_read() would refresh it.
    //
    // `owner_alive` is resolved here from the peer table, because §4 rule 2's Unavailable outranks
    // any cached freshness and the namespace deliberately does not track liveness itself.
    NsError read(uint32_t path_hash, Reading& out, bool* is_local = nullptr);

    // Publish a value for a resource this node owns — the driver's path, not access-checked.
    NsError publish(uint32_t path_hash, const Value& v);

    // Apply a write as if it had arrived from the wire, honouring `access`. Remote writes are
    // request_write().
    NsError write_local(uint32_t path_hash, const Value& v);

    // Ask a peer for a path. Returns the msg_id used, or 0 if the request could not be sent.
    // The answer arrives asynchronously and lands in the namespace cache; on_reading is called if
    // set. This is deliberately not a blocking call — a blocking read across a link whose p99 is
    // hundreds of milliseconds (§3) is how a control loop acquires a hidden deadline.
    uint16_t request_read(uint16_t peer_node_id, uint32_t path_hash);
    uint16_t request_write(uint16_t peer_node_id, uint32_t path_hash, const Value& v);

    struct NsCounters {
        uint32_t reads_served;
        uint32_t reads_requested;
        uint32_t replies_matched;
        uint32_t replies_unmatched;  // answered a request we had already given up on
        uint32_t writes_served;
        uint32_t writes_rejected;
        uint32_t read_timeouts;
    };
    const NsCounters& ns_counters() const { return ns_counters_; }

    PeerTable& peers() { return peers_; }
    const PeerTable& peers() const { return peers_; }
    NodeCounters& counters() { return counters_; }
    const NodeCounters& counters() const { return counters_; }
    EventRing& events() { return events_; }
    const NodeConfig& config() const { return cfg_; }

    // Frames this node has put on the wire, by kind. The simulator uses these to compute channel
    // load; they are also the cheapest sanity check that a policy change did what was intended.
    struct TxTally {
        uint32_t beacons;
        uint32_t probes;
        uint32_t replies;
        uint32_t hellos;
        uint32_t hello_acks;
        uint32_t byes;
        uint32_t errs;
        uint32_t bytes;  // total bytes handed to the transport, for airtime arithmetic
    };
    const TxTally& tx_tally() const { return tally_; }

  private:
    void emit(EventKind kind, const PeerLink* p, uint32_t a = 0, uint32_t b = 0);
    void note(MembershipChange c, PeerLink* p);
    void pin_version(PeerLink& p, uint8_t peer_version);

    bool send_frame(PeerLink* p, const uint8_t mac[kMacLen], uint8_t opcode, const void* payload,
                    uint16_t payload_len, bool ack_req, uint16_t msg_id, uint16_t dst_override,
                    bool broadcast);
    void send_hello(bool want_ack);
    void send_hello_ack(PeerLink& p, uint8_t decision, uint16_t ref_msg_id);
    void fill_heartbeat(HeartbeatPayload& hb, const PeerLink* p) const;
    void send_beacon();
    void send_probe(PeerLink& p);
    void send_reply(PeerLink& p, uint16_t ack_of_msg_id, uint32_t recv_us);
    void send_bye(PeerLink& p);
    void send_err(PeerLink* p, const uint8_t mac[kMacLen], uint16_t code, uint16_t ref_msg_id);

    void handle_hello(PeerLink* p, const uint8_t src_mac[kMacLen], const Frame& f, int8_t rssi);
    void handle_hello_ack(PeerLink* p, const Frame& f);
    void handle_heartbeat(PeerLink* p, const Frame& f, uint32_t recv_us, bool was_broadcast);
    void handle_bye(PeerLink* p, const Frame& f);
    void handle_read(PeerLink* p, const Frame& f);
    void handle_write(PeerLink* p, const Frame& f);
    void handle_reply(PeerLink* p, const Frame& f);
    void handle_err(PeerLink* p, const Frame& f);

    void do_beacon_round(uint32_t now_ms);
    void do_probe_round(uint32_t now_ms);

    NodeConfig cfg_;
    NodeHal hal_;

    PeerTable peers_;
    NodeCounters counters_{};
    EventRing events_;
    TxTally tally_{};

    // §5.1: seq is per (src,dst). Broadcast is its own destination, so it gets its own counter —
    // sharing one with the unicast streams would make every alternation look like a gap.
    // Outstanding namespace requests. Small and fixed: §6 has no line for a request table, and
    // an unbounded one on a link with a 500 ms p99 is a memory leak waiting for a partition.
    static constexpr size_t kMaxPendingNs = 4;
    struct PendingNs {
        uint16_t msg_id;      // 0 = free
        uint16_t peer_node;
        uint32_t path_hash;
        uint32_t sent_ms;
        uint8_t op;           // kOpRead or kOpWrite
    };
    PendingNs pending_[kMaxPendingNs]{};
    PendingNs* pending_find(uint16_t msg_id);
    PendingNs* pending_claim();
    void pending_expire(uint32_t now_ms);

    Namespace ns_;
    NsCounters ns_counters_{};

    uint16_t seq_tx_bcast_ = 0;
    uint32_t hb_seq_bcast_ = 0;

    uint32_t next_beacon_ms_ = 0;
    uint32_t next_probe_ms_ = 0;
    uint32_t next_hello_ms_ = 0;
    size_t probe_cursor_ = 0;  // round-robin position in the peer table

    bool departed_ = false;
    bool started_ = false;

    // One encode buffer, reused. §6 budgets a TX ring slot at one ESP-NOW v2 MTU and this is it;
    // the node is single-threaded by contract, so one is enough.
    uint8_t tx_[kEspNowV2LinkMtu];
};

// §3's twenty-peer ESP-NOW ceiling includes the broadcast entry, so a cell admits nineteen unicast
// peers. Stated here because it is a property of the policy, not of the transport code.
constexpr size_t kMaxUnicastPeers = kMaxPeers - 1;

}  // namespace pot
