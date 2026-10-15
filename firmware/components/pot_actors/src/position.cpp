// M20: a board's position on the floor plan. See position.hpp.

#include "pot/position.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace pot {

namespace {
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
float rdf32(const uint8_t* p) {
    const uint32_t u = rd32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
bool in_plan(float v) { return std::isfinite(v) && v >= 0.0f && v <= kPositionMaxM; }
}  // namespace

bool position_config(const ActorDecl& a, PositionConfig& out) {
    if (a.type != ActorType::Position || a.cfg_len != kPositionCfgLen) return false;
    out.x_hash = rd32(a.cfg);
    out.y_hash = rd32(a.cfg + 4);
    out.x = rdf32(a.cfg + 8);
    out.y = rdf32(a.cfg + 12);
    return out.x_hash != 0 && out.y_hash != 0 && out.x_hash != out.y_hash && in_plan(out.x) && in_plan(out.y);
}

NsDecl position_decl(uint16_t node, uint32_t hash) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = node;
    d.type = ValueType::F32;
    d.unit = Unit::Metre;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;  // the owner moves a board by hand; nothing binds to it tightly
    d.staleness_bound_ms = 3u * kPositionPeriodMs;
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

bool PositionActor::start(uint32_t now_ms) {
    const uint16_t me = node_.config().node_id;
    if (node_.ns().declare(position_decl(me, cfg_.x_hash)) != NsError::Ok) return false;
    if (node_.ns().declare(position_decl(me, cfg_.y_hash)) != NsError::Ok) return false;
    next_ms_ = now_ms;
    return true;
}

void PositionActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + kPositionPeriodMs;
    node_.publish(cfg_.x_hash, Value::of_f32(cfg_.x));
    node_.publish(cfg_.y_hash, Value::of_f32(cfg_.y));
}

size_t PositionActor::stats_json(char* buf, size_t cap, uint32_t now_ms) {
    const int n = std::snprintf(buf, cap, "{\"t\":\"position\",\"node\":%u,\"up_ms\":%u,\"x\":%.2f,\"y\":%.2f}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(now_ms),
                                static_cast<double>(cfg_.x), static_cast<double>(cfg_.y));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {
bool position_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    PositionConfig c{};
    if (!position_config(d, c)) {
        *why = "position config invalid";
        return false;
    }
    return true;
}
Actor* position_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(PositionActor) <= kActorSlotBytes, "PositionActor outgrew its actor slot");
    PositionConfig c{};
    if (env.node == nullptr || !position_config(d, c)) return nullptr;
    return new (mem) PositionActor(*env.node, c);
}
size_t position_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    PositionConfig c{};
    if (peer != 0 || cap < 2 || node != d.node_id || !position_config(d, c)) return 0;
    out[0] = position_decl(node, c.x_hash);
    out[1] = position_decl(node, c.y_hash);
    return 2;
}
}  // namespace

const ActorKind kPositionKind = {ActorType::Position, "position", &position_check, &position_create, nullptr,
                                 &position_outputs};

}  // namespace pot
