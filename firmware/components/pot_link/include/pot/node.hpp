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
#include "pot/hello_auth.hpp"
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

    // M5: run `fn(arg)` to completion, then return -- on a task with room for it. Every Ed25519 and
    // X25519 operation the node does goes through here: each needs ~2-3 KB of stack, and on the board
    // the link task that calls into the node does not have it (a 4 KB stack overflowed on the first
    // enrolled boot, M0-LOG session 21). Null runs it inline, which is right on a host.
    void (*run_heavy)(void* ctx, void (*fn)(void*), void* arg) = nullptr;
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

    // CAN (M4): a wired single-frame bus carries no HELLO, so a valid beacon -- its payload naming
    // the same node as its header -- admits its sender. Off on the radio, where HELLO also pins the
    // ESP-NOW version and must come first.
    bool admit_on_beacon = false;

    // M5 (section 9.3): one link trusted because it is a cable rather than because it signed
    // anything -- the serial frame link to the host, which is physically attached to this board. Its
    // HELLO is admitted unsigned. Deploys over it still need a signed package (section 9.3's deploy
    // key), so this trusts the cable's endpoint to be the operator, not to be the CA.
    bool has_trusted_mac = false;
    uint8_t trusted_mac[kMacLen] = {};

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

    // ---- M5: authenticated admission (section 9.3) -----------------------------------------
    // Hand the node its identity -- a snapshot taken at boot; a later enrolment applies after a
    // reboot, so nothing here ever races the console. With an enrolled identity every HELLO this node
    // sends is signed. With `require` set, a HELLO is admitted only if it carries a certificate from
    // our CA and a signature by the certified key over the sender's MAC; anything else is refused,
    // counted and logged as a peer_refused event. With no identity the node behaves as before M5.
    void set_trust(const Identity* id, bool require);
    bool trust_required() const { return trust_ != nullptr && require_auth_; }
    bool trust_enrolled() const { return trust_ != nullptr && trust_->enrolled; }

    struct PeerAuth {
        bool verified;
        uint32_t epoch;  // the boot epoch the signature covered, and the session key was derived for
        uint8_t pub[kEdPubLen];
        uint8_t key[kSessionKeyLen];
        uint8_t digest[kHelloDigestLen];  // of the exact HELLO verified, to skip identical repeats
    };
    const PeerAuth* peer_auth(const PeerLink* p) const;

    struct AuthCounters {
        uint32_t verified;       // HELLOs checked in full and accepted
        uint32_t cache_hits;     // repeats of an accepted HELLO, recognised by digest
        uint32_t refused;        // every refusal, including the ones not re-logged as events
        uint32_t rate_limited;   // dropped unexamined: over the verification budget
        uint32_t stale_epoch;    // signed or not, older than the epoch already verified
        uint32_t verify_us_max;  // longest full check: certificate, signature, session key
    };
    const AuthCounters& auth_counters() const { return auth_counters_; }

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

    // ---- section 7.8: work units (CALL / REPLY / CAST) ------------------------------------
    //
    // A coordinator hands a unit of work to a peer with request_call(); the peer runs it and
    // answers with reply_call(). Neither side is synchronous, and that is the whole design: a
    // unit of work worth shipping across a link whose round trip is milliseconds runs for far
    // longer than one, so a handler that answered inline would hold the receive path for the
    // duration of the job. The handler therefore only *accepts* the unit.
    //
    // The result is a Value, so at most 8 bytes. Anything bigger travels the way every other
    // datum here travels: the worker publishes it to a path and the reply says only that it is
    // there.
    enum class CallOutcome : uint8_t {
        Ok = 0,       // the worker answered with a value
        Refused,      // the worker answered, but with an error status
        Unavailable,  // the worker died before answering; this unit was never completed
    };

    // Worker side. Return true to accept the unit -- the result is owed, and must be delivered
    // with reply_call(msg_id). Return false to refuse it, which sends an error back at once
    // rather than letting the coordinator wait out a death window for an answer never coming.
    using CallHandler = bool (*)(void* ctx, uint16_t from_node, uint16_t msg_id,
                                 uint32_t path_hash, const uint8_t* args, uint16_t arg_len);

    // Coordinator side. Called once per request_call(), with Ok and a value, or with Refused or
    // Unavailable and no value. Exactly once: a unit is either answered or written off, never
    // both, because a coordinator that can be told twice will hand out the same work twice.
    using CallResultFn = void (*)(void* ctx, uint16_t from_node, uint16_t msg_id,
                                  uint32_t path_hash, CallOutcome outcome, const Value& v);

    void set_call_handler(CallHandler fn, void* ctx) { call_handler_ = fn; call_ctx_ = ctx; }
    void set_call_result(CallResultFn fn, void* ctx) { call_result_ = fn; result_ctx_ = ctx; }

    // §7.4 deploy. The node core only routes: the payload formats and the storage live in
    // pot_deploy, which this component does not depend on.
    //
    // Server side: a DEPLOY_* frame arrived. Write the REPLY payload into `reply` and return its
    // length; the node sends it as a REPLY with the request's msg_id. Return 0 to answer later
    // with send_reply_raw(from_node, msg_id, ...) -- a COMMIT that is first passed on to peers.
    using DeployServerFn = size_t (*)(void* ctx, uint16_t from_node, uint16_t msg_id,
                                      uint8_t opcode, const uint8_t* payload, uint16_t len,
                                      uint8_t* reply, size_t reply_cap);
    // Client side: the answer to a send_deploy(), or its timeout (timed_out, no payload).
    using DeployResultFn = void (*)(void* ctx, uint16_t peer_node, uint16_t msg_id, uint8_t opcode,
                                    bool timed_out, const uint8_t* reply, uint16_t len);
    void set_deploy_server(DeployServerFn fn, void* ctx) { deploy_server_ = fn; deploy_server_ctx_ = ctx; }
    void set_deploy_result(DeployResultFn fn, void* ctx) { deploy_result_ = fn; deploy_result_ctx_ = ctx; }

    // Send one DEPLOY_* request to a peer. Returns the msg_id, or 0 if nothing was sent. Times out
    // like a read (ns_request_timeout_ms): a deploy step is one round trip, not a work unit.
    uint16_t send_deploy(uint16_t peer_node_id, uint8_t opcode, const uint8_t* payload, uint16_t len);

    // §5.2 SAFE_STATE: broadcast at priority 31, class L0, one CAN frame, no ACKREQ. Returns the
    // counter it carried, or 0 if it was not sent. A delivery mechanism, not a safety function (§12).
    uint32_t send_safe_state(uint16_t reason);
    // Called once per SAFE_STATE received, with the sender's node id, counter and reason.
    using SafeStateFn = void (*)(void* ctx, uint16_t from_node, uint32_t counter, uint16_t reason);
    void set_safe_state_handler(SafeStateFn fn, void* ctx) { safe_state_fn_ = fn; safe_state_ctx_ = ctx; }

    // A REPLY with a raw payload, correlated to `msg_id`: the deferred answer above.
    bool send_reply_raw(uint16_t peer_node_id, uint16_t msg_id, const uint8_t* payload, uint16_t len);

    // Send a unit of work. Returns the msg_id, or 0 if it could not be sent -- no peer, no free
    // slot, or the transport refused. 0 is not a failure of the job: nothing was dispatched, so
    // the caller still owns the work.
    uint16_t request_call(uint16_t peer_node_id, uint32_t path_hash, const uint8_t* args,
                          uint16_t arg_len);

    // Answer a unit accepted earlier. `msg_id` and `peer_node_id` are the ones the handler was
    // given.
    bool reply_call(uint16_t peer_node_id, uint16_t msg_id, uint32_t path_hash,
                    const Value& v);

    // Give up on a unit without waiting for the peer to die. A work unit gets no timeout from the
    // node -- section 7.8's units run for far longer than the read timeout, and a node that
    // guessed a deadline would be guessing about somebody else's workload. A coordinator that
    // knows its own job durations calls this; the outcome is reported as Unavailable, once.
    bool cancel_call(uint16_t msg_id);

    // Fire and forget: same layout, no reply, no pending slot. For work whose result the sender
    // does not want -- section 7.8's "stays sleepy unless told otherwise" side of the pattern.
    bool cast(uint16_t peer_node_id, uint32_t path_hash, const uint8_t* args, uint16_t arg_len);

    // Units this node is still owed answers for. A coordinator uses it to decide whether it has
    // capacity to dispatch more, rather than discovering the ceiling by getting 0 back.
    size_t calls_outstanding() const;
    static constexpr size_t max_calls_outstanding() { return kMaxPendingNs; }

    // The static cost of that table, so it can be checked against the section 6 budget rather than
    // discovered by a linker map. It is charged against the headroom section 6 has left, and it is
    // the only allocation section 7.8's request side makes.
    static constexpr size_t pending_table_bytes();

    struct NsCounters {
        uint32_t reads_served;
        uint32_t reads_requested;
        uint32_t replies_matched;
        uint32_t replies_unmatched;  // answered a request we had already given up on
        uint32_t writes_served;
        uint32_t writes_rejected;
        uint32_t read_timeouts;
        uint32_t calls_requested;
        uint32_t calls_served;     // units accepted by a handler here
        uint32_t calls_refused;    // units this node declined, or had no handler for
        uint32_t calls_lost;       // units dispatched to a peer that died before answering
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

    // single_frame: the message is one of §5.3.1's single-frame-on-CAN kinds (beacon, SAFE_STATE),
    // so its header carries seq 0 on every transport and decodes byte-identical from a CAN ID.
    bool send_frame(PeerLink* p, const uint8_t mac[kMacLen], uint8_t opcode, const void* payload,
                    uint16_t payload_len, bool ack_req, uint16_t msg_id, uint16_t dst_override,
                    bool broadcast, bool single_frame = false, uint8_t lclass = kClassL3,
                    uint8_t priority = 1);
    PeerLink* admit_from_beacon(const uint8_t src_mac[kMacLen], const Frame& f, int8_t rssi);
    void handle_safe_state(PeerLink* p, const Frame& f);
    SafeStateFn safe_state_fn_ = nullptr;
    void* safe_state_ctx_ = nullptr;
    uint32_t safe_state_tx_ = 0;
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
    // Declare a peer's built-in resource on first demand; see handle_read(). True if declared.
    bool adopt_peer_sys_resource(uint32_t path_hash);
    void handle_write(PeerLink* p, const Frame& f);
    void handle_reply(PeerLink* p, const Frame& f);
    void handle_call(PeerLink* p, const Frame& f, bool wants_reply);
    void abandon_calls_to(uint16_t peer_node_id);
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
    // One outstanding request per peer the cell can hold. The read path never needs more than a
    // couple, but a coordinator handing out work units (section 7.8) needs one per worker or its
    // parallelism is capped by this table rather than by the cell -- and a ceiling that is an
    // accident of a constant is the kind of limit nobody remembers is there. kMaxPeers is the
    // cell's own ceiling, so the table cannot be the binding constraint. 20 slots at 16 B is
    // 320 B of static DRAM, which section 6 has room for; anything unbounded does not.
    static constexpr size_t kMaxPendingNs = kMaxPeers;
    struct PendingNs {
        uint16_t msg_id;      // 0 = free
        uint16_t peer_node;
        uint32_t path_hash;
        uint32_t sent_ms;
        uint8_t op;           // kOpRead or kOpWrite
    };
    PendingNs pending_[kMaxPendingNs]{};
    PendingNs* pending_find(uint16_t msg_id, uint16_t peer_node);
    PendingNs* pending_claim();
    void pending_expire(uint32_t now_ms);

    Namespace ns_;
    NsCounters ns_counters_{};

    CallHandler call_handler_ = nullptr;
    void* call_ctx_ = nullptr;
    CallResultFn call_result_ = nullptr;
    void* result_ctx_ = nullptr;
    DeployServerFn deploy_server_ = nullptr;
    void* deploy_server_ctx_ = nullptr;
    DeployResultFn deploy_result_ = nullptr;
    void* deploy_result_ctx_ = nullptr;
    void handle_deploy(PeerLink* p, const Frame& f);

    uint16_t seq_tx_bcast_ = 0;
    uint32_t hb_seq_bcast_ = 0;

    uint32_t next_beacon_ms_ = 0;
    uint32_t next_probe_ms_ = 0;
    uint32_t next_hello_ms_ = 0;
    size_t probe_cursor_ = 0;  // round-robin position in the peer table

    bool departed_ = false;
    bool started_ = false;

    // ---- M5 ---------------------------------------------------------------------------------
    enum class AuthOutcome : uint8_t { Fresh, Cached, Legacy, Ignore, Refused };
    AuthOutcome authenticate_hello(PeerLink* p, const uint8_t mac[kMacLen], const Frame& f,
                                   const HelloPayload& h, PeerAuth& out);
    void refuse(uint16_t claimed_id, HelloAuthError e, CertError ce);
    bool is_trusted_link(const uint8_t mac[kMacLen]) const;
    void heavy(void (*fn)(void*), void* arg);

    const Identity* trust_ = nullptr;
    bool require_auth_ = false;
    PeerAuth auth_[kMaxPeers]{};
    AuthCounters auth_counters_{};
    // Our own HELLO, signed once per boot for each value of the want-ack flag: Ed25519 signing is
    // deterministic and the content is fixed for the boot, so re-signing every 2 s would be waste.
    uint8_t signed_hello_[2][kHelloSignedLen]{};
    bool signed_hello_ok_[2] = {false, false};
    // Verification budget: each full check costs ~54 ms of this task (two Ed25519 verifies, M0-LOG
    // session 21), so a flood of forged HELLOs must not be able to buy more than this.
    static constexpr uint32_t kVerifyBurst = 4;
    static constexpr uint32_t kVerifyRefillMs = 250;
    uint32_t verify_tokens_ = kVerifyBurst;
    uint32_t verify_refill_ms_ = 0;
    // The last few refusals, so a refused node announcing itself every 2 s is one event per 10 s
    // rather than a flood. Every refusal is still counted.
    struct Refusal {
        uint16_t node_id;
        uint8_t reason;
        uint32_t at_ms;
    };
    Refusal refusals_[4]{};
    size_t refusal_cursor_ = 0;

    // One encode buffer, reused. §6 budgets a TX ring slot at one ESP-NOW v2 MTU and this is it;
    // the node is single-threaded by contract, so one is enough.
    uint8_t tx_[kEspNowV2LinkMtu];
};

constexpr size_t Node::pending_table_bytes() { return sizeof(PendingNs) * kMaxPendingNs; }

// §3's twenty-peer ESP-NOW ceiling includes the broadcast entry, so a cell admits nineteen unicast
// peers. Stated here because it is a property of the policy, not of the transport code.
constexpr size_t kMaxUnicastPeers = kMaxPeers - 1;

}  // namespace pot
