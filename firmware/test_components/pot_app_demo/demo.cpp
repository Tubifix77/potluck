// M8.2 (PS-1): a stand-in external component. Two actors, one pinned and one portable, written only
// against pot/actor.hpp -- the way an application's component in its own repository is.
//
//   0xF0 demo_counter  pinned or every-node: publishes a u32 count at `out_hash` every `period_ms`
//                      config: out_hash u32, period_ms u16
//   0xF1 demo_roamer   portable: publishes (node id << 16 | count) at the header's out_hash, like
//                      the built-in ticker, with one byte of its own after the header (a step)
//                      config: Potluck's portable header, then step u8

#include <new>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {
namespace {

constexpr ActorType kDemoCounter = static_cast<ActorType>(0xF0);
constexpr ActorType kDemoRoamer = static_cast<ActorType>(0xF1);

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// ---- demo_counter ----------------------------------------------------------------------------------
struct CounterCfg {
    uint32_t out_hash;
    uint16_t period_ms;
};
bool counter_cfg(const ActorDecl& d, CounterCfg& c) {
    if (d.cfg_len != 6) return false;
    c.out_hash = rd32(d.cfg);
    c.period_ms = rd16(d.cfg + 4);
    return c.out_hash != 0 && c.period_ms >= 100;
}
NsDecl counter_decl(uint16_t owner, const CounterCfg& c) {
    NsDecl d;
    d.path_hash = c.out_hash;
    d.owner_node = owner;
    d.type = ValueType::U32;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 3u * c.period_ms;
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

class DemoCounter : public Actor {
  public:
    DemoCounter(Node& n, const CounterCfg& c) : node_(n), cfg_(c) {}
    bool start(uint32_t now) override {
        next_ = now;
        return node_.ns().declare(counter_decl(node_.config().node_id, cfg_)) == NsError::Ok;
    }
    void tick(uint32_t now) override {
        if (static_cast<int32_t>(now - next_) < 0) return;
        next_ = now + cfg_.period_ms;
        node_.publish(cfg_.out_hash, Value::of_u32(++count_));
    }

  private:
    Node& node_;
    CounterCfg cfg_;
    uint32_t next_ = 0;
    uint32_t count_ = 0;
};

bool counter_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    CounterCfg c{};
    if (d.node_id == kPortableNode || !counter_cfg(d, c)) {
        *why = "demo_counter: pinned, with out_hash and period_ms >= 100";
        return false;
    }
    return true;
}
Actor* counter_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(DemoCounter) <= kActorSlotBytes, "DemoCounter fits its slot");
    CounterCfg c{};
    return (env.node != nullptr && counter_cfg(d, c)) ? new (mem) DemoCounter(*env.node, c) : nullptr;
}
size_t counter_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    CounterCfg c{};
    if (peer != 0 || cap == 0 || node != d.node_id || !counter_cfg(d, c)) return 0;
    out[0] = counter_decl(node, c);
    return 1;
}

// ---- demo_roamer -----------------------------------------------------------------------------------
class DemoRoamer : public Actor {
  public:
    DemoRoamer(Node& n, const TickerConfig& h, uint8_t step) : node_(n), h_(h), step_(step) {}
    bool start(uint32_t now) override {
        next_ = now;
        return true;  // its output is the reconciler's to declare and fence
    }
    void tick(uint32_t now) override {
        if (static_cast<int32_t>(now - next_) < 0) return;
        next_ = now + h_.period_ms;
        count_ += step_;
        node_.publish(h_.out_hash, Value::of_u32((static_cast<uint32_t>(node_.config().node_id) << 16) | (count_ & 0xFFFFu)));
    }

  private:
    Node& node_;
    TickerConfig h_;
    uint8_t step_;
    uint32_t next_ = 0;
    uint32_t count_ = 0;
};

bool roamer_parse(const ActorDecl& d, TickerConfig& h, uint8_t& step) {
    size_t len = 0;
    if (!portable_header(d, h, len) || d.cfg_len != len + 1) return false;
    step = d.cfg[len];
    return step != 0;
}
bool roamer_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    TickerConfig h{};
    uint8_t step = 0;
    if (d.node_id != kPortableNode || !roamer_parse(d, h, step)) {
        *why = "demo_roamer: portable, a valid header and a non-zero step";
        return false;
    }
    return true;
}
Actor* roamer_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(DemoRoamer) <= kActorSlotBytes, "DemoRoamer fits its slot");
    TickerConfig h{};
    uint8_t step = 0;
    return (env.node != nullptr && roamer_parse(d, h, step)) ? new (mem) DemoRoamer(*env.node, h, step) : nullptr;
}
bool roamer_placement(const ActorDecl& d, TickerConfig& out) {
    uint8_t step = 0;
    return roamer_parse(d, out, step);
}

const ActorKind kRows[] = {
    {kDemoCounter, "demo_counter", &counter_check, &counter_create, nullptr, &counter_outputs},
    {kDemoRoamer, "demo_roamer", &roamer_check, &roamer_create, &roamer_placement, nullptr},
};

}  // namespace

const ActorKind* app_rows_pot_app_demo(size_t* n) {
    *n = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

}  // namespace pot
