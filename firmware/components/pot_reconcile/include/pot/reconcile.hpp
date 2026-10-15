// The reconciler -- ARCHITECTURE.md section 7.7, milestone M6.
//
// Desired state is the signed image: these portable actors, each with the nodes it may run on and a
// data-gravity score per node, frozen by the build. Observed state is membership. The reconciler
// closes the gap on every node at once, with no coordinator, no election and no consensus (ADR-006):
//
//   assign(actor, live)   a pure function of the actor and the live view: the eligible live node
//                         with the highest gravity, ties broken by rendezvous hashing (Thaler and
//                         Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", 1998 -- HRW
//                         "always maps a given object name to the same server within a given
//                         cluster"). Identical views give identical answers on every node.
//
//   claims                a node running a portable actor says so, with a term, to every peer: on
//                         every change at once, and round-robin as a refresh. A claim is the
//                         sender's whole set, so dropping an actor from it is the release. A claim
//                         lives exactly as long as its sender's membership: declared dead, rebooted
//                         (new boot epoch) or departed, and its claims are gone.
//
//   fencing               every consumer accepts the highest (term, node id) among live claims and
//                         takes values for that actor from that node only -- the fencing token of
//                         Kleppmann's "How to do distributed locking" (2016), "a number that increases
//                         ... every time a client acquires the lock". A node starting an actor takes
//                         a term above any it has seen, so after a partition heals, the instance the
//                         other side started is the one everybody keeps.
//
// The rules, per actor, on a node that has settled (below):
//
//   running   another live claim with a higher (term, id)       -> stop: fenced.
//             assign() names another node, alive, settled and   -> stop and release it to that node,
//               not itself running the actor, and we have not     once per membership change -- the
//               already released to it since the view changed     handover.
//   stopped   any other live claim                              -> nothing; it runs elsewhere.
//             no live claim, and assign() names us              -> start, at once.
//             no live claim, and we are rank r > 0 in our view  -> start after r x orphan_step_ms
//                                                                  unclaimed. The fallback for views
//                                                                  that disagree, so an actor whose
//                                                                  rank-0 node never takes it still
//                                                                  runs somewhere.
//
// What this gives, and what it does not, stated as section 7.7 states it: at-least-once activation.
// A node returning from the dead does not double-run -- it settles first, hears the running claim,
// and the holder hands over. A partition can run an actor on both sides until the claims meet again;
// consumers fence the loser the moment they hear the winner. Actuator-owning actors are never
// portable (ADR-006), and nothing here can make one so.
//
// Settling. A node that has just booted knows nothing about who runs what. It starts nothing until it
// has heard a claim set from every node the image lists as eligible for some actor (each peer sends
// one the moment it sees a new incarnation) and at least settle_min_ms has passed -- or until
// settle_max_ms, for eligible nodes that never answer. Its own claim set carries the SETTLED flag, and
// a holder hands over only to a settled node. A node whose channel is not known yet (a station hunting
// for its router, Node::channel_known) does not settle before settle_max_ms either.
//
// Portable, like the node: no ESP-IDF, no heap, single-threaded under the caller's node lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/deploy.hpp"
#include "pot/namespace.hpp"
#include "pot/actor.hpp"
#include "pot/node.hpp"

namespace pot {

// 16 since M19: a board that may hold its cell's foreman role keeps its own and every sibling cell's (whose summaries
// it reads in the upper cell) -- ten cells, ten roles -- beside the cell's apps. The reconciler lives in PSRAM.
constexpr size_t kMaxPortable = 16;

// The CAST path the claim sets travel on. Not a namespace resource: a reserved hash, like a port.
constexpr uint32_t kReconcilePath = path_hash("potluck:sys/reconcile/v1");
// M8.2 (PS-4): a portable actor's checkpoint, CAST from its holder to the other eligible nodes:
//   key u32, term u32, seq u32, len u8, then len bytes (<= kMaxCheckpoint).
constexpr uint32_t kCheckpointPath = path_hash("potluck:sys/checkpoint/v1");
constexpr size_t kCheckpointHeaderLen = 13;

// The rendezvous weight of `node` for the actor `key`. MurmurHash3's fmix32 finaliser (Appleby,
// public domain) applied twice, so a one-bit change in either input moves every bit of the weight.
uint32_t hrw_weight(uint32_t key, uint16_t node);

// Rank the eligible nodes of `cfg` that `live(node)` says are alive: best first, by gravity, then
// weight, then node id (weights tie at 1 in 2^32; the id makes even that deterministic). Writes up to
// kMaxEligible node ids into `out` and returns how many.
using LiveFn = bool (*)(void* ctx, uint16_t node);
size_t rank_nodes(const TickerConfig& cfg, LiveFn live, void* ctx, uint16_t out[kMaxEligible]);

// ---- the claim set, as a CAST payload ---------------------------------------------------------
//
//   version u8 (1), flags u8, count u8, then count x (out_hash u32, term u32)
constexpr uint8_t kClaimVersion = 1;
constexpr uint8_t kClaimFlagSettled = 0x01;
constexpr size_t kClaimHeaderLen = 3;
constexpr size_t kClaimEntryLen = 8;
constexpr size_t kClaimMaxLen = kClaimHeaderLen + kMaxPortable * kClaimEntryLen;

struct ReconcileConfig {
    uint32_t refresh_ms = 250;        // one peer's claim refresh per interval, round-robin
    uint32_t settle_min_ms = 500;     // never settled sooner than this after start()
    // Settled by this time even if an eligible node stays silent. Three HELLO intervals: a booting
    // node admits its peers one HELLO interval (2 s) plus a signature check after it starts, so a
    // shorter wait cannot tell "alone" from "not admitted yet" -- 3 s did not, on the boards.
    uint32_t settle_max_ms = 6000;
    uint32_t orphan_step_ms = 1000;   // rank r starts an unclaimed actor after r x this
};

// M8.1: one portable actor as the reconciler holds it -- where it may run (`place`), and what to build
// when it runs here (`kind` and its declaration, whose cfg is the spec's own copy).
struct PortableSpec {
    TickerConfig place;
    const ActorKind* kind = nullptr;
    ActorDecl decl{};
    uint8_t cfg[kMaxActorCfg] = {};
};

// M12: where a node keeps checkpoints across a reboot -- NVS on a board. The reconciler writes a
// checkpoint it holds (as the owner, or a replica it accepted) at most once per interval_ms and only when it
// changed, and restores what it finds at load(): so a whole-cell reboot does not lose an actor's state.
// write() must copy `data` before it returns and must not block (a board queues it for another task).
struct CheckpointPersist {
    void* ctx = nullptr;
    bool (*write)(void* ctx, uint32_t key, uint32_t term, uint32_t seq, const uint8_t* data, size_t len) = nullptr;
    bool (*read)(void* ctx, uint32_t key, uint32_t* term, uint32_t* seq, uint8_t* out, size_t cap, size_t* len) = nullptr;
    // Drop every stored checkpoint whose key is not among `keys` (an actor the package no longer has).
    void (*retain)(void* ctx, const uint32_t* keys, size_t n) = nullptr;
    uint32_t interval_ms = 60000;
};

// Collect an image's portable actors through a registration table. False sets `why`: a type with no
// row, or no placement hook, or a config its row refuses, or more than `cap`. `only_eligible` (M14): when non-zero,
// actors whose eligible set leaves that node out are skipped rather than judged -- a placement host (pot_hostnode)
// need not know a board-only actor's type, since it can never be placed there. Boards pass 0: every board runs one
// image, so an unknown type is a fault.
bool collect_portable(const DeployImage& img, const ActorKind* table, size_t n_kinds, PortableSpec* out,
                      size_t cap, size_t& count, const char** why, uint16_t only_eligible = 0);

class Reconciler {
  public:
    Reconciler(Node& node, const ReconcileConfig& cfg = ReconcileConfig{});

    // Load the image's portable actors. Each one's output resource is declared in the namespace,
    // ownerless until a claim says otherwise. Call before start(). False if more than kMaxPortable.
    bool load(const PortableSpec* actors, size_t n);

    // M12: before load(), where checkpoints survive a reboot. Null: they live in RAM only (as before M12).
    void set_persist(const CheckpointPersist* p) { persist_ = p; }

    // M12: a live package change. Every actor whose declaration is byte-identical (type and config) keeps
    // its slot, its instance, its claim, its term and its checkpoint; the others are stopped, their outputs
    // removed from the namespace and their claims dropped; the new ones are added, ownerless, and placed
    // as at boot. Peers hear the new claim set at once. False (nothing changed) if more than kMaxPortable.
    struct ReloadStats {
        size_t kept = 0, stopped = 0, added = 0, outputs_removed = 0;
        size_t carried = 0;  // added in place of a changed declaration of the same actor, with its checkpoint
    };
    bool reload(const PortableSpec* actors, size_t n, uint32_t now_ms, ReloadStats* stats = nullptr);
    // Tickers only (kTickerKind), from their configs: what M6's tests and tools hold.
    bool load(const TickerConfig* actors, size_t n);
    ~Reconciler();

    void start(uint32_t now_ms);

    // Call after every Node::tick, under the same lock.
    void tick(uint32_t now_ms);

    // The node's call handler hands every CAST/CALL here first. True if it was a claim set (consumed).
    bool on_cast(uint16_t from_node, uint32_t path_hash, const uint8_t* args, uint16_t len);

    struct ActorView {
        uint32_t key;        // the actor's output path hash
        bool running;        // this node runs it
        uint32_t term;       // our claim's term, while running
        uint16_t owner;      // whose values consumers here accept; 0 = nobody
        uint32_t owner_term;
        uint32_t max_term;   // highest term seen for this actor
        uint16_t assigned;   // assign() in our current view; 0 = no eligible node alive
        uint16_t standby;    // M19: second in that ranking; 0 = none
        uint32_t activations;
        uint32_t fenced;     // stopped because a higher claim appeared
        uint32_t released;   // stopped to hand over to the assigned node
    };
    size_t actor_count() const { return count_; }
    ActorView view(size_t i) const;
    // M8.2 (PS-5): the board's services, handed to every portable instance this node runs.
    void set_board(const BoardServices* b) { board_ = b; }
    // M8.2: the instance running here for actor i, or nullptr -- for its stats line.
    Actor* instance(size_t i) { return i < count_ ? slots_[i].inst : nullptr; }
    // M12: the actor's type name (its registration row's), "" for a slot a live reload freed.
    const char* kind_name(size_t i) const {
        return i < count_ && slots_[i].active && slots_[i].kind != nullptr ? slots_[i].kind->name : "";
    }

    struct Counters {
        uint32_t sets_sent;
        uint32_t sets_received;
        uint32_t sets_malformed;   // wrong version, truncated, or a count that disagrees with the length
        uint32_t sets_unknown_peer;
        uint32_t view_changes;
        uint32_t send_failures;
        // M8.2 (PS-4)
        uint32_t checkpoints_saved;     // kept as the holder
        uint32_t checkpoints_sent;      // CASTs to other eligible nodes
        uint32_t checkpoints_accepted;  // received from the actor's owner and newer than ours
        uint32_t checkpoints_refused;   // received from a non-owner, older, or malformed
        // M12
        uint32_t checkpoints_persisted;  // handed to CheckpointPersist::write
        uint32_t checkpoints_restored;   // found by CheckpointPersist::read at load
    };
    const Counters& counters() const { return counters_; }
    bool settled() const { return settled_; }

  private:
    struct Slot {
        bool active;               // M12: false for a slot a live reload freed
        TickerConfig cfg;          // the placement
        const ActorKind* kind;
        ActorDecl decl;            // cfg -> decl_cfg
        uint8_t decl_cfg[kMaxActorCfg];
        alignas(std::max_align_t) uint8_t mem[kActorSlotBytes];
        Actor* inst;               // while running here
        // M8.2 (PS-3): its outputs (the kind's outputs(), or the one at cfg.out_hash), each declared
        // here and moved and fenced together.
        uint32_t out_hash[kMaxOutputs];
        uint8_t n_out;
        // M8.2 (PS-4): the newest checkpoint this node holds for the actor, and the store its
        // instances see.
        struct CkCtx {
            Reconciler* rec;
            uint8_t slot;
        } ck_ctx;
        CheckpointStore ck_store;
        bool ck_have;
        uint32_t ck_term, ck_seq, ck_at_ms, ck_last_save_ms;
        uint8_t ck_len;
        uint8_t ck[kMaxCheckpoint];
        bool ck_dirty;             // M12: changed since it was last persisted
        uint32_t ck_persisted_ms;  // M12: when it was last persisted (0: never)
        bool running;
        uint32_t term;
        uint32_t max_term;
        uint16_t owner;
        uint32_t owner_term;
        uint32_t unclaimed_since;  // 0 = claimed, or not yet settled
        uint16_t released_to;      // handover already offered to this node in the current view
        uint32_t activations, fenced, released;
        bool member;               // M19: told the kind's membership hook "member" (ActorKind::membership)
    };
    // What we last heard from one peer, keyed by node id and incarnation.
    struct PeerClaims {
        uint16_t node_id;  // 0 = free
        uint32_t epoch;
        bool settled;
        uint32_t term[kMaxPortable];  // 0 = no claim
    };

    bool init_slot(size_t i, const PortableSpec& a);  // M12: fill slot i and declare its outputs
    static bool same(const Slot& s, const PortableSpec& a);
    void persist_tick(uint32_t now);
    static bool live_cb(void* ctx, uint16_t node);
    struct CapsCtx {
        const Reconciler* r;
        uint32_t need;
    };
    static bool live_caps_cb(void* ctx, uint16_t node);  // M13: live and declaring need_caps
    size_t rank_for(const TickerConfig& cfg, uint16_t out[kMaxEligible]) const;  // M13: need_caps, avoid_caps
    bool peer_live(uint16_t node, uint32_t* epoch = nullptr) const;
    const PeerClaims* claims_of(uint16_t node) const;
    PeerClaims* claims_slot(uint16_t node, bool create);
    bool refresh_view(uint32_t now);
    void update_settled(uint32_t now);
    void decide(size_t i, uint32_t now);
    void resolve_owner(size_t i);
    void activate(size_t i, uint32_t now);
    void stop(size_t i, uint32_t why);  // 1 fenced, 2 handed over
    void run(size_t i, uint32_t now);  // tick the instance, while we are the fenced owner
    void membership(size_t i, bool member);  // M19: tell the kind's hook, on a change
    static bool ck_save(void* ctx, const uint8_t* data, size_t len);
    static bool ck_load(void* ctx, uint8_t* out, size_t cap, size_t* len, uint32_t* age_ms);
    bool on_checkpoint(uint16_t from, const uint8_t* args, uint16_t len);
    void ck_send(const Slot& s, uint16_t node);
    void send_checkpoints_to(uint16_t node);  // a peer (re)appeared: give it what we hold as owner
    size_t encode(uint8_t* out, size_t cap) const;
    void send_to(uint16_t node);
    void send_all();

    Node& node_;
    ReconcileConfig cfg_;
    Slot slots_[kMaxPortable]{};
    size_t count_ = 0;
    PeerClaims peers_[kMaxPeers]{};
    // The view: which peers are alive, and in which incarnation. A change re-arms handovers and pushes
    // our claim set to whoever is new.
    struct Seen {
        uint16_t node_id;
        uint32_t epoch;
    };
    Seen seen_[kMaxPeers]{};
    size_t seen_n_ = 0;
    uint32_t started_ms_ = 0;
    bool started_ = false;
    bool settled_ = false;
    bool dirty_ = false;  // our claim set changed: send it to everyone now
    uint32_t now_ = 0;    // the last tick's time (M8.2: checkpoints are saved and loaded from inside it)
    const BoardServices* board_ = nullptr;
    const CheckpointPersist* persist_ = nullptr;  // M12
    uint32_t next_refresh_ms_ = 0;
    size_t refresh_cursor_ = 0;
    Counters counters_{};
};

}  // namespace pot
