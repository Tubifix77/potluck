// M9: borrowing an idle core -- ADR-005's revisit trigger, as an experiment (ARCHITECTURE section 13).
//
// A job on one node (McJobActor) splits a Monte Carlo estimate of pi into units, runs them on its own
// background workers, and -- when waiting for a local worker would take longer than a peer's round trip
// plus its compute time -- sends a unit to a peer's McLenderActor instead. The unit is a pure function
// (mc_hits: bytes in, bytes out, no pins, no hidden state), pre-provisioned in every image, so no code
// moves and nothing the lender owns is touched. Section 7.8's pattern: units travel as CALL/REPLY.
//
//   the owner decides locally   from its own measured round trips and its own previous unit times;
//                               no load figures are gossiped
//   the lender can say no       it runs at background priority and refuses a unit when its slots are
//                               full; the owner runs that unit elsewhere
//   a lender that dies          the node reports the unit Unavailable, once; the owner runs it again
//   reproducibility             unit i is seeded from (seed, i), and hits are summed as integers, so the
//                               result is the same wherever each unit ran -- a test, not a hope
//
// The actors know nothing of FreeRTOS: the board runs work through BoardServices::bg_submit.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

// The CALL path that names the function: a version in the name, so a changed function is a new name.
uint32_t mc_function_path();

// The pure function. Hits inside the unit quarter circle among `samples` points of a stream seeded by
// (seed, unit), in float. `yield` (may be null) is called every kMcYieldEvery samples: on a board it
// gives the core to the idle task for a tick, so a long unit does not starve the task watchdog.
constexpr uint32_t kMcYieldEvery = 65536;
uint32_t mc_hits(uint32_t seed, uint32_t unit, uint32_t samples, void (*yield)(void* ctx), void* yctx);

// One unit handed to a background worker. `state` is the only field the worker and the link task share
// while it runs: 1 running, 2 done (release/acquire), 0 free.
struct McUnit {
    const BoardServices* board = nullptr;
    uint32_t seed = 0;
    uint32_t unit = 0;
    uint32_t samples = 0;
    uint32_t hits = 0;
    uint32_t started_ms = 0;
    std::atomic<uint8_t> state{0};
};
// The work function bg_submit runs: computes, then marks the unit done.
void mc_unit_run(void* arg);

class McLenderActor : public Actor {
  public:
    McLenderActor(Node& node, const BoardServices* board, const McLenderConfig& cfg)
        : node_(node), board_(board), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    int on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args,
                uint16_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    struct Stats {
        uint32_t accepted;
        uint32_t refused;  // every slot busy, no workers, or malformed arguments
        uint32_t served;   // answered
    };
    const Stats& stats() const { return stats_; }

  private:
    struct Slot {
        McUnit u;
        uint16_t from = 0;
        uint16_t msg = 0;
    };
    Node& node_;
    const BoardServices* board_;
    McLenderConfig cfg_;
    Slot slots_[4];
    Stats stats_{};
};

class McJobActor : public Actor {
  public:
    McJobActor(Node& node, const BoardServices* board, const McJobConfig& cfg)
        : node_(node), board_(board), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v) override;
    // "POT! mc run 0" runs the job on this node alone, "POT! mc run 1" lets peers lend.
    bool on_console(const char* line, size_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    // Start a run now (what the console and start_after_ms do). False if one is running.
    bool run(bool lend, uint32_t now_ms);
    bool running() const { return running_; }

    static constexpr size_t kMaxLenders = 4;
    struct Result {
        uint32_t run;          // 1, 2, ...
        uint8_t lend;
        uint32_t elapsed_ms;
        uint64_t hits;
        float pi;
        uint16_t local_units;  // run on this node
        uint16_t lent_units;   // answered by a lender
        uint16_t refused;      // units a lender declined (run again elsewhere)
        uint16_t lost;         // units whose lender died (run again elsewhere)
    };
    const Result& last() const { return last_; }
    uint16_t lent_to(uint16_t node_id) const;  // units of the last run answered by that node

  private:
    struct Remote {
        uint16_t peer = 0;
        uint16_t msg = 0;  // 0: free
        uint16_t unit = 0;
        uint32_t sent_ms = 0;
    };
    struct Lender {
        uint16_t node_id = 0;    // 0: free
        uint16_t answered = 0;   // this run
        uint32_t until_ms = 0;   // backed off until (refused, lost)
        uint32_t compute_ms = 0; // its measured unit time, net of the round trip; 0 until measured
        bool not_a_lender = false;
    };
    bool next_unit(uint16_t& unit);
    void requeue(uint16_t unit);
    uint32_t rtt_ms(uint16_t node_id) const;
    Lender* lender(uint16_t node_id);
    void dispatch(uint32_t now_ms);
    void finish(uint32_t now_ms);

    Node& node_;
    const BoardServices* board_;
    McJobConfig cfg_;
    bool running_ = false;
    bool lend_ = false;
    bool auto_started_ = false;
    uint16_t next_ = 0;   // the next never-assigned unit
    uint16_t done_ = 0;
    uint64_t hits_ = 0;
    uint16_t redo_[8] = {};  // units to run again (refused, or their lender died)
    uint8_t redo_n_ = 0;
    uint32_t t0_ = 0;
    uint32_t now_ = 0;
    uint32_t local_ms_ = 0;  // this node's measured unit time; 0 until one has finished
    McUnit local_[2];
    uint16_t local_unit_[2] = {};
    Remote remote_[kMaxLenders];
    Lender lenders_[kMaxLenders];
    Result cur_{};
    Result last_{};
};

bool mc_lender_check(const ActorDecl& d, const ActorEnv& env, const char** why);
bool mc_job_check(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t mc_job_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kMcLenderKind;
extern const ActorKind kMcJobKind;

}  // namespace pot
