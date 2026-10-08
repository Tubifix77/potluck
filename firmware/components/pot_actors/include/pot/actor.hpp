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

// What the runtime gives an actor.
struct ActorEnv {
    Node* node = nullptr;
    // The deploy trial window (CONFIG_POT_TRIAL_HEARTBEATS x the heartbeat period). An actor that
    // could stop the node from confirming a bad module must act inside it (the fault actor does).
    uint32_t trial_window_ms = 0;
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
};

// Find a type's row, or nullptr.
const ActorKind* find_actor_kind(const ActorKind* table, size_t n, ActorType type);

// The pinned actors of one node: those whose declaration names this node or every node. Portable
// actors (kPortableNode) are the reconciler's and are skipped here.
class ActorRuntime {
  public:
    ActorRuntime(const ActorKind* table, size_t n) : table_(table), n_kinds_(n) {}

    // Validate this node's pinned actors and keep a copy of each declaration. All or nothing: on
    // false nothing is kept and `why` names the first problem. An actor type this build has no row
    // for is refused -- a node that silently skipped an actor would report a deployment it is not
    // running.
    bool load(const DeployImage& img, uint16_t node_id, const ActorEnv& env, const char** why);

    // Construct and start every loaded actor. Returns how many started; `failed` names the first
    // that did not (nullptr if none).
    size_t start(const ActorEnv& env, uint32_t now_ms, const char** failed);

    void tick(uint32_t now_ms);
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v);

    size_t loaded() const { return n_decl_; }
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
    alignas(max_align_t) uint8_t mem_[kMaxPinnedActors][kActorSlotBytes] = {};
    Actor* run_[kMaxPinnedActors] = {};
    const ActorKind* run_kind_[kMaxPinnedActors] = {};
    size_t n_run_ = 0;
};

}  // namespace pot
