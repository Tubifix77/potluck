// pot_work -- section 7.8's coordinator and workers, on a modelled ESP-NOW cell.
//
//   pot_work                          one coordinator, six workers, 240 units
//   pot_work --workers 1 --workers 6  print the scaling table instead
//   pot_work --scale                  1..N workers, same job, one line each
//   pot_work --kill 1 --at 40         kill worker 1 when 40% of the job is done
//
// WHAT THIS IS FOR
//
// Section 7.8 is the compute half of the project and the answer to the question the whole thing
// started from: an ESP32-S3 has two 240 MHz cores and is far too capable to spend its life
// watching one sensor. The pattern is a coordinator handing units of work to peers that would
// otherwise be idle, as CALL, answered by REPLY. It was designed, and nobody had built any of it.
//
// This asks two questions and nothing else:
//
//   1. Do N workers actually give about N times the throughput of one?
//   2. Does killing a worker mid-job lose the unit it was holding?
//
// WHAT THE NUMBERS MEAN
//
// The channel is section 3's measured model (link_model.hpp). The *compute* is not measured: a
// worker is told a unit takes `--unit-ms` of its time and the simulation charges exactly that.
// That is a stand-in and is not a claim about how long anything really takes on an S3 -- measuring
// that needs the board. What the run does measure honestly is everything around the compute: the
// dispatch cost, the round trips, the coordinator's serialisation, the channel's airtime, and what
// a failure does to a job in flight. Those are the parts that decide whether the pattern is worth
// having, and none of them depends on the number being right.
//
// Efficiency is reported against the ideal a perfect scheduler would reach -- total compute
// divided by the number of workers -- so 100% means the overhead vanished and anything less is the
// price of distribution.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cell.hpp"
#include "link_model.hpp"

using namespace pot;
using namespace potsim;

namespace {

// One unit of a latency-indifferent job. Monte Carlo is the canonical shape and the arguments are
// its whole input: a seed and how many samples to draw. Sixteen bytes out, eight bytes back.
struct UnitArgs {
    uint32_t seed;
    uint32_t samples;
    uint32_t unit_index;
    uint32_t reserved;
};
static_assert(sizeof(UnitArgs) == 16, "the argument list is the whole input");

enum class UnitState : uint8_t {
    Waiting,   // never dispatched, or given back
    Running,   // a worker holds it
    Done,      // answered
};

struct UnitRecord {
    UnitState state = UnitState::Waiting;
    size_t worker = 0;            // cell index of whoever holds it
    uint16_t worker_node_id = 0;  // and its node id, which is half of the key below
    // A msg_id is unique per *peer*, not per node: two workers are both given msg 0x0006 as their
    // sixth request. Matching a result on msg_id alone therefore credits the answer to whichever
    // unit happened to be found first, marking one unit done twice and leaving another running
    // forever. It cost a stalled job at 59 of 60 units, and the identical mistake is available to
    // anybody writing a coordinator, so: the key is (worker, msg_id).
    uint16_t msg_id = 0;
    uint64_t dispatched_us = 0;  // when it went out, for the coordinator's own deadline
    uint32_t dispatches = 0;     // how many times it has been handed out
    uint32_t result = 0;
};

// A worker's own view: it can hold one unit at a time, because section 7.8's background priority
// means a unit runs on the spare capacity of one core and queueing a second would only make both
// later.
struct WorkerState {
    bool busy = false;
    uint64_t finish_us = 0;
    uint16_t msg_id = 0;
    uint32_t path_hash = 0;
    uint16_t coordinator = 0;
    size_t unit_index = 0;
    uint32_t completed = 0;
};

struct Job {
    // Configuration
    size_t coordinator = 0;
    std::vector<size_t> workers;
    uint32_t unit_ms = 200;
    uint32_t unit_timeout_ms = 0;  // 0 = ten times unit_ms
    uint32_t samples_per_unit = 100000;

    // The work
    std::vector<UnitRecord> units;
    std::vector<WorkerState> worker_state;  // the workers' own state, indexed by cell index

    // The coordinator's own record of which worker it is waiting on. Deliberately separate from
    // worker_state: a coordinator cannot see a worker's mind, only what it dispatched and what came
    // back. Reading the worker's flag instead put a second unit on the wire during the 2.8 ms the
    // first was still in flight, and every one of them was refused -- 78 wasted frames in a
    // 60-unit job, from a coordinator that knew something no real one would.
    std::vector<uint8_t> assigned;  // 1 = a unit is out with this worker

    // Failure injection
    long kill_worker = -1;
    double kill_at_percent = 50.0;
    bool killed = false;

    // Results
    uint64_t started_us = 0;
    uint64_t finished_us = 0;
    uint32_t done = 0;
    uint32_t dispatched = 0;
    uint32_t lost_and_reassigned = 0;
    uint32_t refused = 0;
    uint32_t double_computed = 0;  // a unit that was computed more than once
    uint32_t timed_out = 0;        // a unit the coordinator took back on its own deadline
    uint64_t total_result = 0;
    bool ready = false;  // the cell has converged and dispatch may begin

    size_t remaining() const { return units.size() - done; }
};

const uint32_t kJobPath = path_hash("potluck://lab/node-1000/compute/montecarlo");

// -------------------------------------------------------------------------------------------
// The worker side: accept a unit, and finish it later.
// -------------------------------------------------------------------------------------------

struct WorkerCtx {
    Job* job = nullptr;
    size_t index = 0;
    Cell* cell = nullptr;
};

bool worker_on_call(void* ctx, uint16_t from_node, uint16_t msg_id, uint32_t path_hash_in,
                    const uint8_t* args, uint16_t arg_len) {
    WorkerCtx* w = static_cast<WorkerCtx*>(ctx);
    WorkerState& st = w->job->worker_state[w->index];
    if (st.busy || arg_len != sizeof(UnitArgs) || path_hash_in != kJobPath) {
        return false;  // refused now, so the coordinator can place it elsewhere immediately
    }
    UnitArgs a{};
    std::memcpy(&a, args, sizeof(a));

    st.busy = true;
    st.msg_id = msg_id;
    st.path_hash = path_hash_in;
    st.coordinator = from_node;
    st.unit_index = a.unit_index;
    // The compute. Charged as time rather than performed: what is under test is the distribution,
    // not the arithmetic, and a real unit's duration is a bench measurement.
    st.finish_us = w->cell->now_us() + static_cast<uint64_t>(w->job->unit_ms) * 1000ull;
    return true;
}

// -------------------------------------------------------------------------------------------
// The coordinator side: hand out units, collect results, and give reassigned work back out.
// -------------------------------------------------------------------------------------------

void coordinator_on_result(void* ctx, uint16_t from_node, uint16_t msg_id, uint32_t path_hash_in,
                           Node::CallOutcome outcome, const Value& v) {
    Job* job = static_cast<Job*>(ctx);
    (void)path_hash_in;

    // Find the unit by the message it went out as. A coordinator has to keep this itself: the node
    // correlates the reply, but what the reply *means* is the application's business.
    UnitRecord* unit = nullptr;
    for (UnitRecord& u : job->units) {
        if (u.state == UnitState::Running && u.msg_id == msg_id && u.worker_node_id == from_node) {
            unit = &u;
            break;
        }
    }
    if (unit == nullptr) {
        return;  // already accounted for; the node's counters record it
    }

    job->assigned[unit->worker] = 0;

    if (outcome == Node::CallOutcome::Ok) {
        uint32_t hits = 0;
        (void)v.as_u32(hits);
        unit->state = UnitState::Done;
        unit->result = hits;
        job->total_result += hits;
        ++job->done;
        return;
    }
    if (outcome == Node::CallOutcome::Refused) {
        ++job->refused;
    } else {
        ++job->lost_and_reassigned;
    }
    unit->state = UnitState::Waiting;  // back in the queue, and it will go out again
}

// Called once per simulated millisecond: finish whatever is due, then fill every idle worker.
void step(Cell& cell, void* ctx) {
    Job* job = static_cast<Job*>(ctx);

    // The cell has to converge before any of this means anything: a coordinator cannot dispatch to
    // a peer it has not admitted yet.
    if (!job->ready) {
        Node& coord = *cell.node(job->coordinator).node;
        if (coord.peers().count_in_state(PeerState::Alive) < job->workers.size()) {
            return;
        }
        job->ready = true;
        job->started_us = cell.now_us();
    }

    // 1. Units that have finished computing are answered.
    for (size_t w : job->workers) {
        WorkerState& st = job->worker_state[w];
        if (!st.busy || cell.now_us() < st.finish_us) {
            continue;
        }
        // A deterministic stand-in for the unit's result, so the total is checkable.
        const uint32_t hits = job->units[st.unit_index].dispatches * 1000u +
                              static_cast<uint32_t>(st.unit_index);
        Node& worker = *cell.node(w).node;
        worker.reply_call(st.coordinator, st.msg_id, st.path_hash, Value::of_u32(hits));
        st.busy = false;
        ++st.completed;
    }

    // 2. Kill a worker part way through, if asked. Its radio goes off: it keeps holding its unit
    //    and can neither answer nor be reached, which is what a worker falling off a shelf looks
    //    like from the coordinator's side.
    if (job->kill_worker >= 0 && !job->killed && !job->units.empty()) {
        const double pct = 100.0 * static_cast<double>(job->done) /
                           static_cast<double>(job->units.size());
        // Wait until the victim is actually holding a unit. Killing an idle worker tests nothing:
        // the first attempt at this scenario reported "holding nothing" and the interesting half of
        // the property never happened.
        const size_t victim_index = static_cast<size_t>(job->kill_worker);
        if (pct >= job->kill_at_percent && job->worker_state[victim_index].busy) {
            const size_t victim = static_cast<size_t>(job->kill_worker);
            cell.set_asleep(victim, true);
            job->killed = true;
            std::printf("  [%6.2f s] killed worker 0x%04x, holding %s\n",
                        cell.now_us() / 1e6, cell.node(victim).node->config().node_id,
                        job->worker_state[victim].busy ? "a unit" : "nothing");
        }
    }

    // 3. Units that have been out too long come back. The node deliberately puts no deadline on a
    //    work unit -- it cannot know how long somebody else's job takes -- so if nothing does this,
    //    a single lost CALL or REPLY on a worker that stays alive strands its unit for good, and
    //    the job never finishes. That is not a hole in the runtime; it is the coordinator's half of
    //    the contract, and this is what it looks like.
    const uint64_t deadline_us = static_cast<uint64_t>(
        job->unit_timeout_ms != 0 ? job->unit_timeout_ms : job->unit_ms * 10u) * 1000ull;
    for (UnitRecord& u : job->units) {
        if (u.state != UnitState::Running || cell.now_us() - u.dispatched_us < deadline_us) {
            continue;
        }
        Node& c0 = *cell.node(job->coordinator).node;
        ++job->timed_out;
        if (!c0.cancel_call(u.msg_id)) {
            // The node had already resolved it; leave the bookkeeping to the callback.
            continue;
        }
    }

    // 4. Every idle worker gets the next waiting unit.
    Node& coord = *cell.node(job->coordinator).node;
    for (size_t w : job->workers) {
        if (job->assigned[w] != 0) {
            continue;  // still waiting on it, as far as the coordinator knows
        }
        UnitRecord* next = nullptr;
        for (UnitRecord& u : job->units) {
            if (u.state == UnitState::Waiting) {
                next = &u;
                break;
            }
        }
        if (next == nullptr) {
            break;  // nothing left to hand out
        }
        const size_t index = static_cast<size_t>(next - job->units.data());
        UnitArgs a{};
        a.seed = 0x5EED0000u + static_cast<uint32_t>(index);
        a.samples = job->samples_per_unit;
        a.unit_index = static_cast<uint32_t>(index);
        const uint16_t msg_id = coord.request_call(
            cell.node(w).node->config().node_id, kJobPath,
            reinterpret_cast<const uint8_t*>(&a), sizeof(a));
        if (msg_id == 0) {
            continue;  // no peer, no slot, or the transport refused; try again next millisecond
        }
        job->assigned[w] = 1;
        next->state = UnitState::Running;
        next->worker = w;
        next->worker_node_id = cell.node(w).node->config().node_id;
        next->dispatched_us = cell.now_us();
        next->msg_id = msg_id;
        ++next->dispatches;
        if (next->dispatches > 1) {
            // Handed out twice. That is correct after a failure -- the work was never completed --
            // but it is worth counting, because a coordinator that does it without a failure is
            // wasting a worker.
            ++job->double_computed;
        }
        ++job->dispatched;
    }

    // 5. Done when every unit is.
    if (job->done == job->units.size()) {
        job->finished_us = cell.now_us();
        cell.stop();
    }
}

// -------------------------------------------------------------------------------------------
// One run
// -------------------------------------------------------------------------------------------

struct Result {
    size_t workers = 0;
    double elapsed_s = 0.0;
    double ideal_s = 0.0;
    double efficiency = 0.0;
    double throughput = 0.0;  // units per second
    uint32_t done = 0;
    uint32_t dispatched = 0;
    uint32_t reassigned = 0;
    uint32_t refused = 0;
    uint32_t double_computed = 0;
    uint32_t timed_out = 0;
    uint64_t frames = 0;
    double airtime = 0.0;
    bool completed = false;

    // Collected rather than printed from inside the run, so the report comes out in one piece and
    // in an order a person reads.
    struct WorkerLine {
        uint16_t node_id = 0;
        uint32_t completed = 0;
        bool killed = false;
        bool busy = false;        // still holding a unit when the job gave up
        uint32_t accepted = 0;    // units its handler took
        uint32_t refused = 0;     // units it turned down
    };
    std::vector<WorkerLine> per_worker;
    uint32_t calls_requested = 0;
    uint32_t replies_matched = 0;
    uint32_t replies_unmatched = 0;
    uint32_t calls_lost = 0;

    // Any unit the job ended without: what state it was in, who had it, and how many times it had
    // been handed out. A stalled job is the failure mode that matters, and "56 of 60" does not say
    // enough to find it.
    struct StuckUnit {
        size_t index = 0;
        UnitState state = UnitState::Waiting;
        uint16_t holder = 0;
        uint16_t msg_id = 0;
        uint32_t dispatches = 0;
    };
    std::vector<StuckUnit> stuck;
};

Result run_once(size_t workers, uint32_t units, uint32_t unit_ms, const LinkPoint& link,
                uint32_t hb_ms, uint32_t probe_ms, uint64_t seed, long kill_worker,
                double kill_at, double minutes_cap, uint32_t unit_timeout_ms) {
    const size_t nodes = workers + 1;
    Cell cell(nodes, link, BeaconMode::BroadcastBeacon, seed, hb_ms, probe_ms);

    Job job;
    job.coordinator = 0;
    job.unit_ms = unit_ms;
    job.unit_timeout_ms = unit_timeout_ms;
    job.units.resize(units);
    job.worker_state.resize(nodes);
    job.assigned.assign(nodes, 0);
    job.kill_worker = kill_worker;
    job.kill_at_percent = kill_at;
    for (size_t i = 1; i < nodes; ++i) {
        job.workers.push_back(i);
    }

    // Every worker answers calls; the coordinator collects results. A node is one or the other here
    // only because this job says so -- nothing in the runtime makes a node a coordinator.
    std::vector<WorkerCtx> wctx(nodes);
    for (size_t i = 1; i < nodes; ++i) {
        wctx[i].job = &job;
        wctx[i].index = i;
        wctx[i].cell = &cell;
        cell.node(i).node->set_call_handler(&worker_on_call, &wctx[i]);
    }
    cell.node(0).node->set_call_result(&coordinator_on_result, &job);

    cell.set_on_step(&step, &job);
    cell.run(static_cast<uint64_t>(minutes_cap * 60.0 * 1e6));

    Result r;
    r.workers = workers;
    r.done = job.done;
    r.dispatched = job.dispatched;
    r.reassigned = job.lost_and_reassigned;
    r.refused = job.refused;
    r.double_computed = job.double_computed;
    r.timed_out = job.timed_out;
    r.completed = (job.done == units);
    const uint64_t end = r.completed ? job.finished_us : cell.now_us();
    r.elapsed_s = static_cast<double>(end - job.started_us) / 1e6;
    // What a perfect scheduler would take: all the compute, spread over the workers that were
    // actually alive to do it.
    const double alive = (kill_worker >= 0) ? static_cast<double>(workers) - 0.5
                                            : static_cast<double>(workers);
    r.ideal_s = static_cast<double>(units) * unit_ms / 1000.0 / alive;
    r.efficiency = (r.elapsed_s > 0.0) ? r.ideal_s / r.elapsed_s : 0.0;
    r.throughput = (r.elapsed_s > 0.0) ? static_cast<double>(job.done) / r.elapsed_s : 0.0;
    r.frames = cell.frames_sent;
    r.airtime = (r.elapsed_s > 0.0) ? cell.busy_us / (r.elapsed_s * 1e6) : 0.0;

    for (size_t w : job.workers) {
        Result::WorkerLine line;
        line.node_id = cell.node(w).node->config().node_id;
        line.completed = job.worker_state[w].completed;
        line.killed = cell.asleep(w);
        line.busy = job.worker_state[w].busy;
        line.accepted = cell.node(w).node->ns_counters().calls_served;
        line.refused = cell.node(w).node->ns_counters().calls_refused;
        r.per_worker.push_back(line);
    }
    for (size_t i = 0; i < job.units.size(); ++i) {
        const UnitRecord& u = job.units[i];
        if (u.state == UnitState::Done) {
            continue;
        }
        Result::StuckUnit su;
        su.index = i;
        su.state = u.state;
        su.holder = (u.state == UnitState::Running) ? cell.node(u.worker).node->config().node_id : 0;
        su.msg_id = u.msg_id;
        su.dispatches = u.dispatches;
        r.stuck.push_back(su);
    }
    const Node::NsCounters& nc = cell.node(0).node->ns_counters();
    r.calls_requested = nc.calls_requested;
    r.replies_matched = nc.replies_matched;
    r.replies_unmatched = nc.replies_unmatched;
    r.calls_lost = nc.calls_lost;
    return r;
}

void print_header() {
    std::printf("\n  %7s %9s %9s %9s %8s %9s %7s %6s\n", "workers", "elapsed", "ideal", "units/s",
                "speedup", "efficiency", "frames", "air");
    std::printf("  ------- --------- --------- --------- -------- ---------- ------- ------\n");
}

void print_row(const Result& r, double one_worker_throughput) {
    char speedup[16];
    if (one_worker_throughput > 0.0) {
        std::snprintf(speedup, sizeof(speedup), "%.2fx", r.throughput / one_worker_throughput);
    } else {
        // No single-worker baseline was run, so there is no speedup to report. Printing 1.00x
        // would be inventing a measurement.
        std::snprintf(speedup, sizeof(speedup), "%s", "-");
    }
    std::printf("  %7zu %8.2fs %8.2fs %9.2f %8s %9.0f%% %7llu %5.1f%%%s\n", r.workers,
                r.elapsed_s, r.ideal_s, r.throughput, speedup, r.efficiency * 100.0,
                static_cast<unsigned long long>(r.frames), r.airtime * 100.0,
                r.completed ? "" : "   <-- DID NOT FINISH");
}

void usage() {
    std::printf(
        "pot_work - section 7.8's coordinator and workers on a modelled ESP-NOW cell\n"
        "\n"
        "  --workers N        workers, plus one coordinator (default 6, max %zu)\n"
        "  --units N          units of work in the job (default 240)\n"
        "  --unit-ms N        how long one unit occupies a worker (default 200)\n"
        "  --unit-timeout-ms N  when the coordinator takes a unit back (default 10x unit-ms)\n"
        "  --scale            run 1..N workers and print the scaling table\n"
        "  --kill W           kill worker W part way through the job\n"
        "  --at P             ...when P%% of the job is done (default 50)\n"
        "  --link L           bench | 54m_clear | 52m_fringe | 58m_cliff (default bench)\n"
        "  --hb-ms N          heartbeat period, default 100\n"
        "  --probe-ms N       round-robin probe interval, default 1000\n"
        "  --seed N           RNG seed, default 1\n"
        "  --cap-minutes M    give up after M simulated minutes (default 30)\n"
        "\n"
        "The channel is measured (ARCHITECTURE.md section 3). The per-unit compute time is a\n"
        "stand-in and needs a board to measure; everything around it is honest.\n",
        kMaxUnicastPeers);
}

}  // namespace

int main(int argc, char** argv) {
    size_t workers = 6;
    uint32_t units = 240;
    uint32_t unit_ms = 200;
    uint32_t unit_timeout_ms = 0;
    const LinkPoint* link = &kBench;
    uint32_t hb_ms = kHbPeriodWirelessMs;
    uint32_t probe_ms = 1000;
    uint64_t seed = 1;
    bool scale = false;
    long kill_worker = -1;
    double kill_at = 50.0;
    double cap_minutes = 30.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--workers") workers = static_cast<size_t>(std::stoul(next()));
        else if (a == "--units") units = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--unit-ms") unit_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--unit-timeout-ms")
            unit_timeout_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--scale") scale = true;
        else if (a == "--kill") kill_worker = std::stol(next());
        else if (a == "--at") kill_at = std::stod(next());
        else if (a == "--hb-ms") hb_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--probe-ms") probe_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--seed") seed = std::stoull(next());
        else if (a == "--cap-minutes") cap_minutes = std::stod(next());
        else if (a == "--link") {
            const std::string l = next();
            const LinkPoint* p = link_by_name(l);
            if (p == nullptr) { std::printf("unknown link '%s'\n", l.c_str()); return 2; }
            link = p;
        } else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("unknown option '%s'\n\n", a.c_str()); usage(); return 2; }
    }

    if (workers < 1 || workers > kMaxUnicastPeers) {
        std::printf("--workers must be between 1 and %zu (section 3's peer ceiling, less the\n"
                    "coordinator's broadcast entry)\n", kMaxUnicastPeers);
        return 2;
    }

    std::printf("pot_work: %u units of %u ms, link %s (PDR %.4f, mean %.0f us), hb %u ms\n",
                units, unit_ms, link->label, link->pdr, link->mean_us, hb_ms);
    std::printf("  one round trip costs about %.1f ms; one unit occupies a worker for %u ms\n",
                2.0 * link->mean_us / 1000.0, unit_ms);

    if (scale) {
        print_header();
        double base = 0.0;
        for (size_t w = 1; w <= workers; ++w) {
            const Result r = run_once(w, units, unit_ms, *link, hb_ms, probe_ms, seed, -1, 0.0,
                                      cap_minutes, unit_timeout_ms);
            if (w == 1) {
                base = r.throughput;
            }
            print_row(r, base);
        }
        std::printf("\n  speedup is against one worker; efficiency is against a perfect scheduler\n"
                    "  with no dispatch cost at all. The gap between them is what distribution\n"
                    "  costs on this link.\n");
        return 0;
    }

    const bool killing = kill_worker >= 0;
    if (killing) {
        std::printf("  worker %ld will be killed at %.0f%% of the job\n", kill_worker, kill_at);
    }
    print_header();
    const Result r = run_once(workers, units, unit_ms, *link, hb_ms, probe_ms, seed, kill_worker,
                              kill_at, cap_minutes, unit_timeout_ms);
    print_row(r, 0.0);

    std::printf("%s", "\nper worker\n");
    for (const Result::WorkerLine& line : r.per_worker) {
        std::printf("  0x%04x  %5u units, %u accepted, %u refused%s%s\n", line.node_id,
                    line.completed, line.accepted, line.refused,
                    line.busy ? ", STILL HOLDING ONE" : "",
                    line.killed ? "   (killed mid-job)" : "");
    }
    std::printf("%s", "\ncoordinator counters\n");
    std::printf("  calls requested   %u\n", r.calls_requested);
    std::printf("  replies matched   %u\n", r.replies_matched);
    std::printf("  replies unmatched %u   (answers to units already written off)\n",
                r.replies_unmatched);
    std::printf("  units written off %u\n", r.calls_lost);

    std::printf("\nthe job\n");
    std::printf("  units completed     %u of %u%s\n", r.done, units,
                r.completed ? "" : "   <-- WORK WAS LOST");
    std::printf("  units dispatched    %u  (%u more than there are units)\n", r.dispatched,
                r.dispatched - r.done);
    std::printf("  written off         %u  (worker died holding the unit)\n", r.reassigned);
    std::printf("  refused             %u  (worker was busy or did not know the path)\n",
                r.refused);
    std::printf("  handed out twice    %u\n", r.double_computed);
    std::printf("  taken back on time  %u  (the coordinator's own deadline, %u ms)\n", r.timed_out,
                unit_timeout_ms != 0 ? unit_timeout_ms : unit_ms * 10u);

    if (!r.stuck.empty()) {
        std::printf("%s", "\nunits the job ended without\n");
        for (const Result::StuckUnit& su : r.stuck) {
            std::printf("  unit %-4zu %-8s held by 0x%04x, msg 0x%04x, dispatched %u time(s)\n",
                        su.index, su.state == UnitState::Running ? "running" : "waiting",
                        su.holder, su.msg_id, su.dispatches);
        }
    }

    const uint32_t deadline_used = (unit_timeout_ms != 0) ? unit_timeout_ms : unit_ms * 10u;
    if (!r.completed && r.timed_out > units && deadline_used <= unit_ms) {
        std::printf("\n  The deadline (%u ms) is not longer than a unit (%u ms), so every unit was\n",
                    deadline_used, unit_ms);
        std::printf("%s", "  taken back before any worker could finish one, and the retries\n");
        std::printf("%s", "  saturated the channel. That is misconfiguration rather than a defect,\n");
        std::printf("%s", "  but it is a silent livelock: set --unit-timeout-ms well above\n");
        std::printf("%s", "  --unit-ms. A coordinator's deadline is a backstop for a lost frame,\n");
        std::printf("%s", "  not a schedule.\n");
    }

    if (killing) {
        std::printf("\n  A unit handed out twice after a worker died is not waste: the first\n");
        std::printf("  attempt never completed. What would be a defect is the job finishing with\n");
        std::printf("  fewer results than units, and it did not.\n");
    }
    return r.completed ? 0 : 1;
}
