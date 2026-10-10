// M9: borrowing an idle core. See mc.hpp.

#include "pot/mc.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace pot {

namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// A 32-bit integer mix, so that neighbouring (seed, unit) pairs start unrelated streams.
uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

constexpr uint16_t kMcArgsLen = 12;  // seed u32, unit u32, samples u32

}  // namespace

uint32_t mc_function_path() { return path_hash("potluck:fn/mc_pi/v1"); }

uint32_t mc_hits(uint32_t seed, uint32_t unit, uint32_t samples, void (*yield)(void* ctx), void* yctx) {
    // Marsaglia's xorshift32: fast, and exactly reproducible on any machine with 32-bit unsigned ints.
    uint32_t s = mix32(seed ^ mix32(unit + 0x9e3779b9U));
    if (s == 0) s = 1;  // xorshift's one forbidden state
    constexpr float kScale = 1.0f / 16777216.0f;  // 24 bits: exactly representable, uniform in [0, 1)
    uint32_t hits = 0;
    uint32_t until_yield = kMcYieldEvery;
    for (uint32_t i = 0; i < samples; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const float x = static_cast<float>(s >> 8) * kScale;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const float y = static_cast<float>(s >> 8) * kScale;
        if (x * x + y * y < 1.0f) ++hits;
        if (--until_yield == 0) {
            until_yield = kMcYieldEvery;
            if (yield != nullptr) yield(yctx);
        }
    }
    return hits;
}

void mc_unit_run(void* arg) {
    McUnit* u = static_cast<McUnit*>(arg);
    const BoardServices* b = u->board;
    u->hits = mc_hits(u->seed, u->unit, u->samples, b != nullptr ? b->bg_yield : nullptr,
                      b != nullptr ? b->ctx : nullptr);
    u->state.store(2, std::memory_order_release);
}

// ---- the lender ---------------------------------------------------------------------------------------

bool McLenderActor::start(uint32_t) { return true; }

int McLenderActor::on_call(uint16_t from, uint16_t msg, uint32_t path, const uint8_t* args, uint16_t len) {
    if (path != mc_function_path()) return kCallNotMine;
    const uint32_t samples = (len == kMcArgsLen) ? rd32(args + 8) : 0;
    if (board_ == nullptr || board_->bg_submit == nullptr || samples < 1000 || samples > 100000000u) {
        ++stats_.refused;
        return kCallRefused;
    }
    const uint8_t n = cfg_.slots < 4 ? cfg_.slots : 4;
    for (uint8_t i = 0; i < n; ++i) {
        Slot& s = slots_[i];
        if (s.u.state.load(std::memory_order_acquire) != 0) continue;
        s.u.board = board_;
        s.u.seed = rd32(args);
        s.u.unit = rd32(args + 4);
        s.u.samples = samples;
        s.u.hits = 0;
        s.u.started_ms = now_;
        s.u.state.store(1, std::memory_order_relaxed);
        if (!board_->bg_submit(board_->ctx, &mc_unit_run, &s.u)) {
            s.u.state.store(0, std::memory_order_relaxed);
            break;  // every worker is busy with someone else's work: refuse
        }
        s.from = from;
        s.msg = msg;
        ++stats_.accepted;
        return kCallAccepted;
    }
    ++stats_.refused;  // BUSY: the caller runs it elsewhere rather than waiting here
    return kCallRefused;
}

void McLenderActor::tick(uint32_t now) {
    now_ = now;
    for (Slot& s : slots_) {
        if (s.u.state.load(std::memory_order_acquire) != 2) continue;
        node_.reply_call(s.from, s.msg, mc_function_path(), Value::of_u32(s.u.hits));
        ++stats_.served;
        stats_.unit_ms = now - s.u.started_ms;
        s.u.state.store(0, std::memory_order_relaxed);
    }
}

size_t McLenderActor::stats_json(char* buf, size_t cap, uint32_t) {
    unsigned busy = 0;
    for (const Slot& s : slots_) busy += s.u.state.load(std::memory_order_relaxed) == 1 ? 1u : 0u;
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"mc_lend\",\"node\":%u,\"accepted\":%u,\"refused\":%u,\"served\":%u,"
                                "\"busy\":%u,\"slots\":%u,\"unit_ms\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(stats_.accepted),
                                static_cast<unsigned>(stats_.refused), static_cast<unsigned>(stats_.served), busy,
                                static_cast<unsigned>(cfg_.slots), static_cast<unsigned>(stats_.unit_ms));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- the job --------------------------------------------------------------------------------------------

bool McJobActor::start(uint32_t now_ms) {
    NsDecl d;
    d.path_hash = cfg_.out_hash;
    d.owner_node = node_.config().node_id;
    d.type = ValueType::F32;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 3600000;  // a result, not a stream: it stays the last run's for an hour
    d.staleness_policy = StalenessPolicy::Informative;
    if (node_.ns().declare(d) != NsError::Ok) return false;
    now_ = now_ms;
    return true;
}

bool McJobActor::run(bool lend, uint32_t now_ms) {
    if (running_ || board_ == nullptr || board_->bg_submit == nullptr) return false;
    running_ = true;
    lend_ = lend;
    next_ = 0;
    done_ = 0;
    hits_ = 0;
    redo_n_ = 0;
    t0_ = now_ms;
    for (Lender& l : lenders_) l = Lender{};
    for (Remote& r : remote_) r = Remote{};
    cur_ = Result{};
    cur_.run = last_.run + 1;
    cur_.lend = lend ? 1 : 0;
    return true;
}

bool McJobActor::next_unit(uint16_t& unit) {
    if (redo_n_ > 0) {
        unit = redo_[--redo_n_];
        return true;
    }
    if (next_ < cfg_.units) {
        unit = next_++;
        return true;
    }
    return false;
}

void McJobActor::requeue(uint16_t unit) {
    if (redo_n_ < sizeof(redo_) / sizeof(redo_[0])) {
        redo_[redo_n_++] = unit;
        return;
    }
    // Cannot happen with kMaxLenders <= 8 in flight, but losing a unit silently would be worse than a
    // wrong count: run it from the start of the never-assigned range instead.
    if (next_ > 0) --next_;
}

uint32_t McJobActor::rtt_ms(uint16_t node_id) const {
    const PeerLink* p = node_.peers().find_by_node_id(node_id);
    if (p == nullptr) return 1000;
    const size_t i = node_.peers().index_of(p);
    uint32_t lo = 0, hi = 0;
    // The 99th percentile's upper bucket edge: pessimistic, so a unit is lent only when lending clearly
    // pays. No samples yet (or the overflow bucket): the largest round trip seen, or a second.
    if (i < PeerTable::capacity() && node_.peers().histogram(i).percentile(99, lo, hi) && hi != 0xFFFFFFFFu) {
        return hi / 1000u + 1u;
    }
    return p->rtt_max_us != 0 ? p->rtt_max_us / 1000u + 1u : 1000u;
}

McJobActor::Lender* McJobActor::lender(uint16_t node_id) {
    Lender* free_slot = nullptr;
    for (Lender& l : lenders_) {
        if (l.node_id == node_id) return &l;
        if (l.node_id == 0 && free_slot == nullptr) free_slot = &l;
    }
    if (free_slot != nullptr) {
        *free_slot = Lender{};
        free_slot->node_id = node_id;
    }
    return free_slot;
}

void McJobActor::dispatch(uint32_t now) {
    // Local first: every free background worker gets a unit.
    const uint8_t workers = board_->bg_workers < 2 ? board_->bg_workers : 2;
    for (uint8_t i = 0; i < workers; ++i) {
        McUnit& u = local_[i];
        if (u.state.load(std::memory_order_acquire) != 0) continue;
        uint16_t unit = 0;
        if (!next_unit(unit)) break;
        u.board = board_;
        u.seed = cfg_.seed;
        u.unit = unit;
        u.samples = cfg_.samples;
        u.hits = 0;
        u.started_ms = now;
        u.state.store(1, std::memory_order_relaxed);
        if (!board_->bg_submit(board_->ctx, &mc_unit_run, &u)) {
            u.state.store(0, std::memory_order_relaxed);
            requeue(unit);
            break;
        }
        local_unit_[i] = unit;
    }
    // Then lending, if allowed and once this node knows its own unit time.
    if (!lend_ || local_ms_ == 0 || workers == 0) return;
    const uint16_t self = node_.config().node_id;
    for (size_t k = 0; k < PeerTable::capacity(); ++k) {
        const PeerLink& p = node_.peers().slot(k);
        if (p.state != PeerState::Alive || p.node_id == self || p.node_id >= kPortableNode) continue;
        Lender* l = lender(p.node_id);
        if (l == nullptr || l->not_a_lender || static_cast<int32_t>(now - l->until_ms) < 0) continue;
        size_t in_flight = 0;
        Remote* free_r = nullptr;
        for (Remote& r : remote_) {
            if (r.msg != 0 && r.peer == p.node_id) ++in_flight;
            if (r.msg == 0 && free_r == nullptr) free_r = &r;
        }
        if (free_r == nullptr) return;  // every remote slot in use
        if (in_flight >= 2) continue;   // two per lender: one running, one queued behind it
        // Units not yet started anywhere, and how long the next one would wait for a local worker.
        const uint32_t waiting = static_cast<uint32_t>(cfg_.units - next_) + redo_n_;
        if (waiting == 0) return;
        const uint32_t local_wait = local_ms_ * ((waiting + workers - 1u) / workers);
        const uint32_t remote_cost = rtt_ms(p.node_id) + (l->compute_ms != 0 ? l->compute_ms : local_ms_);
        if (local_wait <= remote_cost) continue;  // it would finish sooner here: keep it
        uint16_t unit = 0;
        if (!next_unit(unit)) return;
        uint8_t args[kMcArgsLen];
        wr32(args, cfg_.seed);
        wr32(args + 4, unit);
        wr32(args + 8, cfg_.samples);
        const uint16_t id = node_.request_call(p.node_id, mc_function_path(), args, kMcArgsLen);
        if (id == 0) {
            requeue(unit);  // nothing was sent: the unit is still ours
            continue;
        }
        free_r->peer = p.node_id;
        free_r->msg = id;
        free_r->unit = unit;
        free_r->sent_ms = now;
    }
}

void McJobActor::tick(uint32_t now) {
    now_ = now;
    if (!auto_started_ && cfg_.start_after_ms != 0 && static_cast<int32_t>(now - cfg_.start_after_ms) >= 0) {
        auto_started_ = true;
        run(cfg_.lend != 0, now);
    }
    if (!running_) return;
    const uint8_t workers = board_->bg_workers < 2 ? board_->bg_workers : 2;
    for (uint8_t i = 0; i < workers; ++i) {
        McUnit& u = local_[i];
        if (u.state.load(std::memory_order_acquire) != 2) continue;
        hits_ += u.hits;
        ++done_;
        ++cur_.local_units;
        const uint32_t took = now - u.started_ms;
        local_last_ms_[i] = took;
        local_ms_ = local_ms_ == 0 ? took : (local_ms_ * 3u + took) / 4u;
        u.state.store(0, std::memory_order_relaxed);
    }
    if (done_ >= cfg_.units) {
        finish(now);
        return;
    }
    dispatch(now);
}

bool McJobActor::on_call_result(uint16_t from, uint16_t msg, uint32_t path, Node::CallOutcome o, const Value& v) {
    if (path != mc_function_path()) return false;
    Remote* r = nullptr;
    for (Remote& x : remote_) {
        if (x.msg == msg && x.peer == from) r = &x;
    }
    if (r == nullptr) return false;  // not one of ours (or from a run that has ended)
    Lender* l = lender(from);
    uint32_t hits = 0;
    if (o == Node::CallOutcome::Ok && v.as_u32(hits)) {
        hits_ += hits;
        ++done_;
        ++cur_.lent_units;
        if (l != nullptr) {
            ++l->answered;
            const uint32_t took = now_ - r->sent_ms;
            const uint32_t rtt = rtt_ms(from);
            const uint32_t net = took > rtt ? took - rtt : 1u;
            l->compute_ms = l->compute_ms == 0 ? net : (l->compute_ms * 3u + net) / 4u;
        }
    } else if (o == Node::CallOutcome::Unavailable) {
        requeue(r->unit);  // the lender died with it: run it again
        ++cur_.lost;
        if (l != nullptr) l->until_ms = now_ + 2000;
    } else {
        requeue(r->unit);  // BUSY, or no lender there at all
        ++cur_.refused;
        if (l != nullptr) {
            l->until_ms = now_ + 500;
            if (l->answered == 0 && cur_.refused > 8) l->not_a_lender = true;  // never answered: stop asking
        }
    }
    *r = Remote{};
    return true;
}

void McJobActor::finish(uint32_t now) {
    running_ = false;
    cur_.elapsed_ms = now - t0_;
    cur_.hits = hits_;
    const double total = static_cast<double>(cfg_.units) * static_cast<double>(cfg_.samples);
    cur_.pi = static_cast<float>(4.0 * static_cast<double>(hits_) / total);
    last_ = cur_;
    node_.ns().publish(cfg_.out_hash, Value::of_f32(cur_.pi), now);
    // Once per run, so the result is on the console the moment it exists.
    std::printf("{\"t\":\"mc_done\",\"node\":%u,\"run\":%u,\"lend\":%u,\"units\":%u,\"samples\":%u,"
                "\"elapsed_ms\":%u,\"hits\":%llu,\"pi\":%.7f,\"local\":%u,\"lent\":%u,\"refused\":%u,\"lost\":%u}\n",
                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(last_.run),
                static_cast<unsigned>(last_.lend), static_cast<unsigned>(cfg_.units),
                static_cast<unsigned>(cfg_.samples), static_cast<unsigned>(last_.elapsed_ms),
                static_cast<unsigned long long>(last_.hits), static_cast<double>(last_.pi),
                static_cast<unsigned>(last_.local_units), static_cast<unsigned>(last_.lent_units),
                static_cast<unsigned>(last_.refused), static_cast<unsigned>(last_.lost));
}

uint16_t McJobActor::lent_to(uint16_t node_id) const {
    for (const Lender& l : lenders_) {
        if (l.node_id == node_id) return l.answered;
    }
    return 0;
}

bool McJobActor::on_console(const char* line, size_t len) {
    static const char kCmd[] = "POT! mc run ";
    constexpr size_t kLen = sizeof(kCmd) - 1;
    if (len <= kLen || std::strncmp(line, kCmd, kLen) != 0) return false;
    const bool lend = line[kLen] == '1';
    const bool ok = run(lend, now_);
    std::printf("{\"t\":\"test\",\"cmd\":\"mc_run\",\"lend\":%d,\"started\":%d}\n", lend ? 1 : 0, ok ? 1 : 0);
    return true;
}

size_t McJobActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"mc\",\"node\":%u,\"running\":%d,\"run\":%u,\"lend\":%u,\"done\":%u,"
                                "\"units\":%u,\"local_ms\":%u,\"w0_ms\":%u,\"w1_ms\":%u,\"lender_ms\":[%u,%u],"
                                "\"last_elapsed_ms\":%u,\"last_pi\":%.7f}",
                                static_cast<unsigned>(node_.config().node_id), running_ ? 1 : 0,
                                static_cast<unsigned>(running_ ? cur_.run : last_.run),
                                static_cast<unsigned>(running_ ? cur_.lend : last_.lend),
                                static_cast<unsigned>(done_), static_cast<unsigned>(cfg_.units),
                                static_cast<unsigned>(local_ms_), static_cast<unsigned>(local_last_ms_[0]),
                                static_cast<unsigned>(local_last_ms_[1]), static_cast<unsigned>(lenders_[0].compute_ms),
                                static_cast<unsigned>(lenders_[1].compute_ms), static_cast<unsigned>(last_.elapsed_ms),
                                static_cast<double>(last_.pi));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- registration ---------------------------------------------------------------------------------------

bool mc_lender_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    McLenderConfig c{};
    if (!mc_lender_config(d, c)) {
        *why = "mc_lender config invalid";
        return false;
    }
    return true;
}

bool mc_job_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    McJobConfig c{};
    if (!mc_job_config(d, c)) {
        *why = "mc_job config invalid";
        return false;
    }
    return true;
}

size_t mc_job_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    McJobConfig c{};
    if (peer != 0 || cap == 0 || node != d.node_id || !mc_job_config(d, c)) return 0;
    NsDecl x;
    x.path_hash = c.out_hash;
    x.owner_node = node;
    x.type = ValueType::F32;
    x.unit = Unit::None;
    x.kind = ResourceKind::Sampled;
    x.access = Access::Read;
    x.latency_class = kClassL4;
    x.staleness_bound_ms = 3600000;
    x.staleness_policy = StalenessPolicy::Informative;
    out[0] = x;
    return 1;
}

namespace {
Actor* mc_lender_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(McLenderActor) <= kActorSlotBytes, "McLenderActor outgrew its actor slot");
    McLenderConfig c{};
    if (env.node == nullptr || !mc_lender_config(d, c)) return nullptr;
    return new (mem) McLenderActor(*env.node, env.board, c);
}
Actor* mc_job_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(McJobActor) <= kActorSlotBytes, "McJobActor outgrew its actor slot");
    McJobConfig c{};
    if (env.node == nullptr || !mc_job_config(d, c)) return nullptr;
    return new (mem) McJobActor(*env.node, env.board, c);
}
}  // namespace

const ActorKind kMcLenderKind = {ActorType::McLender, "mc_lender", &mc_lender_check, &mc_lender_create};
const ActorKind kMcJobKind = {ActorType::McJob, "mc_job", &mc_job_check, &mc_job_create, nullptr, &mc_job_outputs};

}  // namespace pot
