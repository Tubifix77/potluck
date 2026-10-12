// M13: the status LED as a writable resource. See pot/led_switch.hpp.

#include "pot/led_switch.hpp"

#include <cstdio>
#include <new>

namespace pot {

bool led_switch_config(const ActorDecl& a, LedSwitchConfig& out) {
    if (a.type != ActorType::LedSwitch || a.cfg_len != kLedSwitchCfgLen) return false;
    const uint8_t* p = a.cfg;
    out.out_hash = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    out.r = p[4];
    out.g = p[5];
    out.b = p[6];
    out.blink_ds = p[7];
    return out.out_hash != 0 && (out.blink_ds == 0 || out.blink_ds >= 2);
}

NsDecl led_switch_decl(uint16_t node, const LedSwitchConfig& c) {
    NsDecl d;
    d.path_hash = c.out_hash;
    d.owner_node = node;
    d.type = ValueType::Bool;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;  // written through WRITE: by the cell, or from outside where the package allows it
    d.latency_class = kClassL3;
    d.staleness_bound_ms = 0;      // a setting, not a measurement: it does not go stale
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

bool LedSwitchActor::start(uint32_t now_ms) {
    if (node_.ns().declare(led_switch_decl(node_.config().node_id, cfg_)) != NsError::Ok) return false;
    const NsEntry* e = node_.ns().find(cfg_.out_hash);
    if (e != nullptr && !e->has_data()) node_.ns().publish(cfg_.out_hash, Value::of_bool(false), now_ms);
    next_ms_ = now_ms;
    return true;
}

void LedSwitchActor::tick(uint32_t now) {
    const NsEntry* e = node_.ns().find(cfg_.out_hash);
    if (e == nullptr) return;
    bool v = false;
    e->value.as_bool(v);
    if (e->update_count != last_count_) {
        last_count_ = e->update_count;
        if (v != on_) ++changes_;
    }
    const bool changed = v != on_;
    on_ = v;
    // PS-5's light lapses 5 s after the last call: refresh every 2 s while on, and at once on a change.
    if (board_ == nullptr || board_->led_app == nullptr) return;
    if (!changed && static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + 2000;
    board_->led_app(board_->ctx, on_, cfg_.r, cfg_.g, cfg_.b, static_cast<uint16_t>(cfg_.blink_ds * 100u));
}

LedSwitchActor::~LedSwitchActor() {
    if (board_ != nullptr && board_->led_app != nullptr) board_->led_app(board_->ctx, false, 0, 0, 0, 0);
}

size_t LedSwitchActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap, "{\"t\":\"led_switch\",\"node\":%u,\"on\":%d,\"changes\":%lu}",
                                static_cast<unsigned>(node_.config().node_id), on_ ? 1 : 0,
                                static_cast<unsigned long>(changes_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {
bool led_switch_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    LedSwitchConfig c{};
    if (d.node_id == kPortableNode || d.node_id == kEveryNode || !led_switch_config(d, c)) {
        *why = "led_switch must be pinned, with an output and a blink period of 0 or 0.2..25.5 s";
        return false;
    }
    return true;
}
Actor* led_switch_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(LedSwitchActor) <= kActorSlotBytes, "LedSwitchActor outgrew its actor slot");
    LedSwitchConfig c{};
    if (env.node == nullptr || !led_switch_config(d, c)) return nullptr;
    return new (mem) LedSwitchActor(*env.node, c, env.board);
}
size_t led_switch_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    LedSwitchConfig c{};
    if (peer != 0 || cap == 0 || node != d.node_id || !led_switch_config(d, c)) return 0;
    out[0] = led_switch_decl(node, c);
    return 1;
}
}  // namespace

const ActorKind kLedSwitchKind = {ActorType::LedSwitch, "led_switch", &led_switch_check, &led_switch_create, nullptr,
                                  &led_switch_outputs};

}  // namespace pot
