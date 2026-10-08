// M8.1: the actor API. What an application actor is written against, instead of inside m0_main.cpp.
//
// An actor is a C++ class compiled into the image (code loaded at run time is M7's, which stays
// gated). The deploy image (pot/deploy.hpp) says which actors run on which node and with what
// configuration; the firmware turns each declaration into an instance through ONE table of
// ActorKind rows -- the registration table -- and from then on drives every instance the same way:
// start once, tick every pass of the link task, offer each CALL result, print its stats line.
//
// What an actor may touch is what ActorEnv hands it: the node (the namespace, the peer table,
// CALL/CAST), and nothing of the runtime's own state. It runs on the link task with the node's lock
// held, so tick() must return promptly: it is sharing a loop with the heartbeat. Anything slower --
// a driver read that blocks, a computation -- belongs on its own task, publishing through the node.
//
// The API is the same on the host: the unit tests compile actors against it, and a host runtime can
// (M10).

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

// M8.2 (PS-4): section 7.7's checkpoint -- "re-activation from its last checkpoint if one exists".
// A Value holds at most 8 bytes, so a checkpoint is a small blob the reconciler replicates itself, under
// the outputs' fencing. For portable actors only (ActorEnv::checkpoint is null otherwise).
constexpr size_t kMaxCheckpoint = 128;
struct CheckpointStore {
    void* ctx = nullptr;
    // Keep a snapshot. Only while this node is the actor's fenced owner, at most one a second, at most
    // kMaxCheckpoint bytes; replicated to every other eligible node that is alive. False otherwise.
    bool (*save)(void* ctx, const uint8_t* data, size_t len) = nullptr;
    // The newest snapshot this node holds for the actor -- from any earlier holder, this node included
    // -- and its age (since this node received it). False if it holds none.
    bool (*load)(void* ctx, uint8_t* out, size_t cap, size_t* len, uint32_t* age_ms) = nullptr;
};

// What the runtime gives an actor.
struct ActorEnv {
    Node* node = nullptr;
    // The deploy trial window (CONFIG_POT_TRIAL_HEARTBEATS x the heartbeat period). An actor that
    // could stop the node from confirming a bad module must act inside it (the fault actor does).
    uint32_t trial_window_ms = 0;
    // M8.2 (PS-4): portable actors only. Kept by the actor from create(); valid for its lifetime.
    const CheckpointStore* checkpoint = nullptr;
};

class Actor {
  public:
    virtual ~Actor() = default;

    // Once, after the node exists. Declare the resources the actor owns here. False: the actor
    // could not start (a resource refused, a driver missing); the runtime logs it and drops it.
    virtual bool start(uint32_t now_ms) = 0;

    // Every pass of the link task, with the node's lock held. Return promptly.
    virtual void tick(uint32_t now_ms) { (void)now_ms; }

    // A result for a CALL some actor made (Node::set_call_result). True if it was this actor's: the
    // runtime stops offering it to the others.
    virtual bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash,
                                Node::CallOutcome outcome, const Value& v) {
        (void)from_node, (void)msg_id, (void)path_hash, (void)outcome, (void)v;
        return false;
    }

    // M8.2 (PS-2): one frame accepted over the RADIO from a peer this node knows -- every one, in arrival order, as
    // the link task handles it (RxSample, pot/node.hpp): its sender, RSSI, local receive time,
    // beacon or unicast, the beacon's heartbeat sequence, and whether it came through the relay
    // (then the RSSI is the relay's). Synchronous, with the node's lock held, before the frame is
    // handled: record and return. Do not send from here; do anything heavier in tick(). Nothing is
    // queued in between, so an actor never misses a sample the node accepted; frames the radio
    // queue dropped before the node saw them are counted there ("queue_dropped" in the link stats).
    // Pinned actors only: a portable actor is not offered samples. Frames from the host's cable or a
    // CAN bus produce none: their RSSI is not a measurement.
    virtual void on_rx_sample(const RxSample& s) { (void)s; }

    // A console line, for test instruments (e.g. "POT! die_temp fail 1"). True if it was this
    // actor's; the runtime stops offering it. Called with the node's lock held.
    virtual bool on_console(const char* line, size_t len) {
        (void)line, (void)len;
        return false;
    }

    // One JSON object for the periodic stats output, without a trailing newline: e.g.
    // {"t":"svc",...}. Written into `buf` (NUL-terminated); returns its length, 0 for no line.
    // Called with the node's lock held.
    virtual size_t stats_json(char* buf, size_t cap, uint32_t now_ms) {
        (void)buf, (void)cap, (void)now_ms;
        return 0;
    }
};

// Every instance lives in a fixed slot of this many bytes; the registration table static_asserts
// that each actor class fits. No heap.
constexpr size_t kActorSlotBytes = 192;
constexpr size_t kMaxPinnedActors = 8;
// The longest actor configuration the runtime keeps a copy of (a ticker's is 7 + 3 x 8 = 31).
constexpr size_t kMaxActorCfg = 48;

// One row of the registration table: everything the runtime needs to know about one actor type.
struct ActorKind {
    ActorType type;
    const char* name;  // for logs: "led", "svc_client", ...
    // Validate a declaration meant for this node, with no side effects. False sets `why`. Called for
    // every actor in the image before any is created: a half-applied image is worse than either whole
    // one.
    bool (*check)(const ActorDecl& decl, const ActorEnv& env, const char** why);
    // Construct the actor in `mem` (kActorSlotBytes, max-aligned) from a declaration that passed
    // check(). The declaration's cfg is the runtime's copy and outlives the actor.
    Actor* (*create)(void* mem, const ActorDecl& decl, const ActorEnv& env);
    // Portable types only (declared with node_id kPortableNode): where the actor may run. Fills the
    // placement fields the reconciler ranks by -- out_hash (the actor's identity and its output),
    // period_ms (its output's publication period, which sets the staleness bound), and the eligible
    // nodes with their data-gravity scores. nullptr: the type is pinned-only.
    bool (*placement)(const ActorDecl& decl, TickerConfig& out) = nullptr;
    // M8.2 (PS-3): the resources the actor publishes. Fills up to `cap` (kMaxOutputs) decls and
    // returns how many; 0 for none.
    //   peer == 0: its per-node outputs on `node`.   peer != 0: its per-link outputs on `node` about `peer`.
    //   `node` is the actor's own node for a pinned actor, any member for an every-node one, and 0
    //   for a portable one, whose outputs are node-free (act/<name>/...).
    // What it is for: other nodes hold replicas of an actor's outputs, worked out from the deploy
    // image -- which every node holds -- so a host on one board's cable, or an actor anywhere, reads
    // a value on another board with section 4's tuple. Pinned per-node outputs are declared at boot;
    // every-node and per-link ones on demand, when a read finds no entry (ActorRuntime::adopt). A
    // portable actor's are declared and fenced by the reconciler. The actor itself declares its own
    // on its own node (it may call this hook to do so). nullptr: the type publishes nothing others read.
    size_t (*outputs)(const ActorDecl& decl, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) = nullptr;
};

constexpr size_t kMaxOutputs = 8;

// Find a type's row, or nullptr.
const ActorKind* find_actor_kind(const ActorKind* table, size_t n, ActorType type);

// M8.2 (PS-1): an external component's contribution -- an application's actors, in its own
// repository, pulled into a firmware variant (tools\build_firmware.ps1 -AppComponents). The build
// generates one entry per component from its directory name; nothing in Potluck names it.
//   namespace pot { const ActorKind* app_rows_<component>(size_t* n); }
struct AppRows {
    const char* component;
    const ActorKind* (*rows)(size_t* n);
};

constexpr size_t kMaxActorKinds = 48;

// Built-in rows first, then each component's. Refused, with `why` and the two parties named (for a
// duplicate, `a` and `b` are the components, "potluck" for a built-in): two rows for one type, an
// external row outside 0x80-0xFF, or more rows than `cap`. On refusal `out` holds the built-ins
// only, so the node still runs, and an image naming an external type is refused as "a type this
// build has no row for" -- loudly, never with a silently chosen winner.
bool merge_actor_tables(const ActorKind* builtins, size_t n_builtins, const AppRows* apps, size_t n_apps,
                        ActorKind* out, size_t cap, size_t& n_out, const char** why, const char** a,
                        const char** b);

// The pinned actors of one node: those whose declaration names this node or every node. Portable
// actors (kPortableNode) are the reconciler's and are skipped here.
class ActorRuntime {
  public:
    ActorRuntime(const ActorKind* table, size_t n) : table_(table), n_kinds_(n) {}
    // M8.2: the firmware's table is merged at boot (built-ins plus external components), after the
    // runtime object exists. Before load().
    void set_table(const ActorKind* table, size_t n) {
        table_ = table;
        n_kinds_ = n;
    }

    // Validate this node's pinned actors and keep a copy of each declaration. All or nothing: on
    // false nothing is kept and `why` names the first problem. An actor type this build has no row
    // for is refused -- a node that silently skipped an actor would report a deployment it is not
    // running.
    bool load(const DeployImage& img, uint16_t node_id, const ActorEnv& env, const char** why);

    // Construct and start every loaded actor. Returns how many started; `failed` names the first
    // that did not (nullptr if none).
    size_t start(const ActorEnv& env, uint32_t now_ms, const char** failed);

    void tick(uint32_t now_ms);
    bool on_console(const char* line, size_t len);
    void on_rx_sample(const RxSample& s);
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v);

    size_t loaded() const { return n_decl_; }
    size_t remote_outputs() const { return n_remote_; }

    // M8.2 (PS-3): declare the replica for `path_hash` if it is an output of an actor in the image on
    // one of `nodes` (this node and its peers), about one of them. True if it declared one.
    bool adopt(Namespace& ns, uint32_t path_hash, const uint16_t* nodes, size_t n_nodes);
    size_t running() const { return n_run_; }
    Actor* actor(size_t i) { return i < n_run_ ? run_[i] : nullptr; }
    const char* name(size_t i) const { return i < n_run_ ? run_kind_[i]->name : "?"; }

    ~ActorRuntime();

  private:
    struct Held {
        ActorDecl decl;
        const ActorKind* kind;
        uint8_t cfg[kMaxActorCfg];
    };
    const ActorKind* table_;
    size_t n_kinds_;
    Held held_[kMaxPinnedActors] = {};
    size_t n_decl_ = 0;
    // Every image declaration that runs, or may run, on other nodes: pinned elsewhere, or every-node.
    // Their per-node outputs (pinned) are declared by start(); the rest are adopted on demand.
    Held others_[kMaxActors] = {};
    size_t n_others_ = 0;
    size_t n_remote_ = 0;
    alignas(max_align_t) uint8_t mem_[kMaxPinnedActors][kActorSlotBytes] = {};
    Actor* run_[kMaxPinnedActors] = {};
    const ActorKind* run_kind_[kMaxPinnedActors] = {};
    size_t n_run_ = 0;
};

}  // namespace pot
