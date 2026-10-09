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
#include "pot/frame_auth.hpp"
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

// M8.2 (PS-0, and PS-2's raw material): one frame accepted from a peer this node knows, as on_rx saw
// it. `rssi` is the radio's figure for the last hop; on a relayed frame that is the relay's, so a
// consumer measuring a link must leave relayed samples out.
struct RxSample {
    uint32_t recv_us;
    uint16_t node_id;    // the member that sent it (the header's src, not the hop)
    uint16_t hb_seq;     // a beacon's 16-bit heartbeat sequence; 0 for other frames
    int8_t rssi;
    uint8_t kind;        // kRxKindBeacon, kRxKindUnicast, kRxKindOtherBroadcast
    bool relayed;
    // The channel this node was on when it accepted the frame. RSSI is a per-channel measurement: on
    // the bench one link read -26 dBm on channel 11 and -33 dBm on channel 1, so a baseline learned on
    // one channel says nothing about another (M0-LOG session 33).
    uint8_t channel;
};
constexpr uint8_t kRxKindBeacon = 1;
constexpr uint8_t kRxKindUnicast = 2;
constexpr uint8_t kRxKindOtherBroadcast = 3;

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

    // M8.2: called for every frame accepted from a known peer, after authentication, before it is
    // handled -- on the link task, with the node's lock held, so it must return at once. May be null.
    void (*on_rx_sample)(void* ctx, const RxSample& s) = nullptr;

    // M5: run `fn(arg)` to completion, then return -- on a task with room for it. Every Ed25519 and
    // X25519 operation the node does goes through here: each needs ~2-3 KB of stack, and on the board
    // the link task that calls into the node does not have it (a 4 KB stack overflowed on the first
    // enrolled boot, M0-LOG session 21). Null runs it inline, which is right on a host.
    void (*run_heavy)(void* ctx, void (*fn)(void*), void* arg) = nullptr;

    // M5 step 5: the SAFE_STATE high-water table changed; persist it (section 8.3: "so a mesh power
    // cycle does not reopen the window"). Called after every accepted signed SAFE_STATE. May be null.
    void (*persist_safe_state_floors)(void* ctx, const void* table, size_t bytes) = nullptr;

    // M5.1 CR-1: retune the radio. The node calls this when the cell moves or while it scans for a
    // cell it has lost. Null: the node never changes channel (a host test, or a transport that has none).
    void (*set_channel)(void* ctx, uint8_t channel) = nullptr;
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

    // M5.1 CR-1: the channel, and finding the cell again. A node that has lost every peer for
    // `scan_after_ms` hops channel_lo..channel_hi, `scan_dwell_ms` on each, greeting on each, and stays
    // where a peer answers. A node whose channel someone else owns -- a Wi-Fi station, which must stay
    // on its router's channel -- sets channel_fixed and never hops. 1..11 is the default country's
    // range ("01", world-safe; ledger 2026-10-07).
    uint8_t channel = 1;
    uint8_t channel_lo = 1;
    uint8_t channel_hi = 11;
    bool channel_fixed = false;
    // M8.2 (found on the bench, M0-LOG session 33): whether this node knows the channel it is on. A
    // station still looking for its router does not -- its scan carries the radio across channels, and
    // a HELLO sent from channel 9 that declared the channel it booted on (1) took the cell to channel 1
    // just before the station associated on 11. While false the HELLO declares channel 0: a fixed node
    // is still the authority (the others sweep for it if it vanishes) but sends nobody anywhere.
    bool channel_known = true;
    uint32_t scan_after_ms = 3000;
    uint32_t scan_dwell_ms = 300;

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
        // The issue time of the certificate that verified. The fence is (issued, epoch), not epoch
        // alone: a board whose flash was erased and re-enrolled restarts its epoch counter with its
        // flash, and only its newer certificate tells that apart from a replay (M0-LOG session 27).
        uint32_t issued;
        uint8_t pub[kEdPubLen];
        uint8_t key[kSessionKeyLen];
        uint8_t digest[kHelloDigestLen];  // of the exact HELLO verified, to skip identical repeats
        // Step 4: the implicit high 16 bits of our seq towards this peer, and its anti-replay window.
        // Both start over with every new key (frame_auth.hpp).
        uint16_t tx_hi;
        ReplayWindow rx;
    };
    const PeerAuth* peer_auth(const PeerLink* p) const;

    struct AuthCounters {
        uint32_t verified;       // HELLOs checked in full and accepted
        uint32_t cache_hits;     // repeats of an accepted HELLO, recognised by digest
        uint32_t refused;        // every refusal, including the ones not re-logged as events
        uint32_t rate_limited;   // dropped unexamined: over the verification budget
        uint32_t stale_epoch;    // signed or not, older than the epoch already verified
        uint32_t verify_us_max;  // longest full check: certificate, signature, session key
        // Step 4, unicast frames from verified peers:
        uint32_t tags_ok;
        uint32_t bad_tag;        // tag did not verify: forged, corrupted, or a previous key's frame
        uint32_t replayed;       // a duplicate, or older than the 64-frame window
        uint32_t untagged;       // no tag, or from a peer whose key we do not hold yet
        uint32_t bcast_dropped;  // a broadcast of an opcode that may not be broadcast unauthenticated
        uint32_t beacon_epoch_ignored;  // a beacon claiming an epoch no signed HELLO has shown
        uint32_t tag_us_max;     // longest tag check
    };
    const AuthCounters& auth_counters() const { return auth_counters_; }

    // Step 5: the highest (epoch, counter) accepted per SAFE_STATE source. Restored at boot from what
    // persist_safe_state_floors wrote, before start().
    struct SafeStateFloor {
        uint16_t node_id;  // 0 = free
        // Two bytes of the public key that set this floor (0 = not known: an entry persisted before
        // M6.1). A different key means the sender was re-enrolled; nothing signed by the old key can
        // verify under the new one, so the fence restarts for it rather than refusing it for ever.
        uint16_t key_tag;
        uint32_t epoch;
        uint32_t counter;
        uint32_t last_try_ms;  // per-source verification rate limit; not meaningful across boots
    };
    static constexpr size_t kSafeStateSources = kMaxPeers;
    const SafeStateFloor* safe_state_floors() const { return ss_floor_; }
    void restore_safe_state_floors(const void* table, size_t bytes);
    struct SafeStateCounters {
        uint32_t accepted;
        uint32_t unsigned_refused;
        uint32_t unknown_sender;
        uint32_t bad_signature;
        uint32_t replayed;      // (epoch, counter) not above the source's high-water
        uint32_t rate_limited;
        uint32_t verify_us_max;
    };
    const SafeStateCounters& safe_state_counters() const { return ss_counters_; }

    // ---- M5.1 (CR-3): this node's declared heartbeat window, changed at run time --------------
    // Peers judge a node by the window its HELLO declares, so the window can move while running. A
    // TIGHTER window applies at once (peers are only more tolerant than needed until they hear). A
    // LOOSER one is announced first -- three HELLOs, 100 ms apart -- and applied only after the old
    // death window plus two ordinary HELLO intervals, so a peer that missed every announcement still
    // hears beacons at the old rate until a periodic HELLO has told it too. HELLO carries the period in one byte of
    // centiseconds: 10..2550 ms in steps of 10, miss limit 1..255. False if out of range.
    bool set_heartbeat_window(uint32_t period_ms, uint8_t miss_limit);
    uint32_t heartbeat_period_ms() const { return cfg_.hb_period_ms; }

    // ---- M5.1 (CR-1): moving the cell, and finding it again -----------------------------------
    // Tell the cell to move to `channel` in `delay_ms` (signed when enrolled), and move with it.
    bool move_cell(uint8_t channel, uint16_t delay_ms);
    // Retune this node alone, unannounced -- what a station node experiences when its router moves.
    // The others find it by scanning.
    void set_channel_now(uint8_t channel);
    // A Wi-Fi station's channel belongs to its router: it must never hop. Settable at run time.
    void set_channel_fixed(bool fixed) { cfg_.channel_fixed = fixed; }
    void set_channel_known(bool known) { cfg_.channel_known = known; }
    bool channel_known() const { return cfg_.channel_known; }
    uint8_t channel() const { return channel_; }
    bool scanning() const { return scanning_; }
    bool sweeping_for_authority() const { return sweeping_; }
    struct ChannelCounters {
        uint32_t moves_sent;
        uint32_t moves_followed;
        uint32_t moves_refused;  // unsigned, unknown sender, stale epoch, old move id, bad signature
        uint32_t scans_started;
        uint32_t scan_hops;
        uint32_t found_by_scan;
        uint32_t last_lost_to_found_ms;  // how long the last scan took to find the cell
        // M6.1: sweeps after losing the cell's channel authority while other peers were still here.
        uint32_t authority_sweeps;
        uint32_t authority_found;        // ... that found it on some channel and stayed
        // M8.1 bench: after a fruitless sweep, single-channel visits looking for it (each shorter
        // than the death window), first every 2 s, then ever more rarely.
        uint32_t search_visits;
    };
    const ChannelCounters& channel_counters() const { return ch_counters_; }

    // ---- M5.1 (CR-2, ADR-009): the single-hop relay -----------------------------------------
    // As the relay, this node forwards frames unchanged between members it hears and members only it
    // hears. Star only: a relay never accepts a relayed frame, and two relays resolve to the lower id.
    void set_relay(bool on);
    bool relay() const { return relay_; }
    // A peer reached through the relay (its next-hop MAC is the relay's).
    bool peer_relayed(const PeerLink* p) const;
    struct RelayCounters {
        uint32_t unicast_forwarded;
        uint32_t broadcast_forwarded;
        uint32_t hello_forwarded;
        uint32_t not_forwarded;   // destination not a direct member, or would go back where it came from
        uint32_t relay_dup;       // a relayed copy of a member we hear directly: dropped before auth
        uint32_t own_echo;        // our own frame, re-broadcast back to us
        uint32_t refused;         // a forwarded frame from a node that is not a relay, or to a relay
        uint32_t path_changes;    // a member moved between direct and relayed
    };
    const RelayCounters& relay_counters() const { return relay_counters_; }

    // ---- M5.1 (CR-4): the BUSY capability ------------------------------------------------------
    void set_busy(bool busy);
    bool busy() const { return (caps_ & kHelloCapBusy) != 0; }
    // The caps a peer's last admitted HELLO declared.
    uint32_t peer_caps(const PeerLink* p) const;

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
    // M8.1: the owner's driver reports the sensor behind `path_hash` as failed (Namespace::publish_faulty).
    NsError publish_faulty(uint32_t path_hash);

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

    // M8.2 (PS-3): a read -- a peer's READ, the host's through this board, or a local actor's -- found
    // no entry for `path_hash`. The handler may declare one (a replica of an actor's output on
    // another node, worked out from the deploy image) and return true; the read then proceeds.
    using AdoptFn = bool (*)(void* ctx, uint32_t path_hash);
    void set_adopt_handler(AdoptFn fn, void* ctx) { adopt_ = fn; adopt_ctx_ = ctx; }

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
        uint32_t replies_fenced;   // M6: a value from a node that no longer owns the resource
    };
    const NsCounters& ns_counters() const { return ns_counters_; }

    // An event raised above the node -- the reconciler's -- into the same ring and the same on_event
    // as membership's, so one stream carries both and a capture can order them.
    void record_event(EventKind kind, uint16_t node_id, uint32_t a, uint32_t b);

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
    AdoptFn adopt_ = nullptr;
    void* adopt_ctx_ = nullptr;
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
    bool check_frame_auth(PeerLink& p, const uint8_t* data, const Frame& f);
    void reject_frame(const PeerLink& p, uint32_t reason, uint32_t ext_seq);
    uint32_t last_reject_event_ms_ = 0;

    // M5.1 CR-2
    PeerLink* direct_peer(const uint8_t mac[kMacLen]);
    PeerLink* relayed_peer(uint16_t node_id);
    PeerLink* any_peer(uint16_t node_id);
    void relay_hello(const Frame& f, const uint8_t orig_mac[kMacLen]);
    bool relay_ = false;
    bool relayed_[kMaxPeers]{};
    bool rx_via_relay_ = false;  // set by on_rx for the frame being dispatched
    uint8_t rx_orig_mac_[kMacLen]{};
    RelayCounters relay_counters_{};

    // M5.1
    void handle_channel(PeerLink* p, const Frame& f);
    void retune(uint8_t ch);
    void tick_channel(uint32_t now_ms);
    uint8_t channel_ = 1;
    bool scanning_ = false;
    uint32_t last_alive_ms_ = 0;
    uint32_t scan_next_ms_ = 0;
    uint32_t scan_started_ms_ = 0;
    uint32_t settle_until_ms_ = 0;  // after a scan finds the cell: defer to stable peers' channel until then
    // M6.1: the cell's channel authority -- a peer whose HELLO declares its channel fixed (a Wi-Fi
    // station: its router owns the channel). When it moves with its router, nobody hears it go: it
    // announces on the new channel. CR-1's scan starts only when EVERY peer is lost, so a cell whose
    // other members still hear each other would stay behind for good (found on the bench, M0-LOG
    // session 29). So losing the authority while others remain starts one bounded sweep.
    uint16_t authority_id_ = 0;
    bool sweeping_ = false;
    uint8_t sweep_home_ = 0;   // where to return if one sweep does not find it
    uint8_t sweep_left_ = 0;   // channels still to visit
    // After a sweep that found nothing, keep looking -- the authority may be rebooting, and come
    // back on another channel (a mesh with access points on several channels: M0-LOG session 30).
    // One channel per visit, scan_dwell_ms long, so the members that stay home never miss us for a
    // death window; a pass over the band every 2 s per visit at first, each later pass half as often,
    // down to one visit a minute.
    bool searching_ = false;
    bool visiting_ = false;
    uint8_t search_home_ = 0;
    uint8_t search_cursor_ = 0;
    uint8_t search_in_pass_ = 0;
    uint32_t search_interval_ms_ = 0;
    uint32_t search_next_ms_ = 0;
    uint32_t visit_end_ms_ = 0;
    static constexpr uint32_t kSearchFirstMs = 2000;
    static constexpr uint32_t kSearchMaxMs = 60000;
    bool settling(uint32_t now) const { return scanning_ || static_cast<int32_t>(settle_until_ms_ - now) > 0; }
    uint8_t pending_channel_ = 0;
    uint32_t pending_channel_at_ms_ = 0;
    uint32_t move_tx_ = 0;
    uint32_t move_rx_last_[kMaxPeers]{};
    ChannelCounters ch_counters_{};
    void announce_change();
    uint32_t caps_ = 0;
    uint32_t peer_caps_[kMaxPeers]{};
    uint32_t announced_period_ms_ = 0;  // what HELLO declares; differs from cfg_ while a looser window waits
    uint8_t announced_miss_ = 0;
    uint32_t window_apply_at_ms_ = 0;   // 0 = nothing pending
    uint8_t announce_left_ = 0;
    uint32_t next_announce_ms_ = 0;
    SafeStateFloor ss_floor_[kSafeStateSources]{};
    SafeStateCounters ss_counters_{};
    SafeStateFloor* ss_floor_for(uint16_t node_id, bool create);

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
