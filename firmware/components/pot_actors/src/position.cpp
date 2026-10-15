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
    out.room_hash = rd32(a.cfg + 8);
    out.x = rdf32(a.cfg + 12);
    out.y = rdf32(a.cfg + 16);
    out.room = rd32(a.cfg + 20);
    out.near_hash = rd32(a.cfg + 24);
    for (size_t i = 0; i < kPositionNearMax; ++i) {
        out.near[i] = static_cast<uint16_t>(a.cfg[28 + 2 * i] | a.cfg[29 + 2 * i] << 8);
    }
    out.cells[0] = a.cfg[36];
    out.cells[1] = a.cfg[37];
    const bool placed = !(out.x == -1.0f && out.y == -1.0f);  // -1, -1: a room and no place
    if (placed && !(in_plan(out.x) && in_plan(out.y))) return false;
    if (out.cells[1] != 0 && (out.cells[0] == 0 || out.cells[1] == out.cells[0])) return false;  // first, then a second
    if (!placed && out.room == 0 && out.near_count() == 0 && out.cells[0] == 0) return false;  // nothing to hold
    for (size_t i = out.near_count(); i < kPositionNearMax; ++i) {
        if (out.near[i] != 0) return false;  // the ids come first, then zeros
    }
    return out.x_hash != 0 && out.y_hash != 0 && out.room_hash != 0 && out.near_hash != 0 && out.x_hash != out.y_hash &&
           out.room <= 0xFFFF;
}

NsDecl position_decl(uint16_t node, uint32_t hash, Unit unit, ValueType type) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = node;
    d.type = type;  // a coordinate (f32), the room's number (u32), the nearest boards (bytes)
    d.unit = unit;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;  // the owner moves a board by hand; nothing binds to it tightly
    d.staleness_bound_ms = 3u * kPositionPeriodMs;
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

PositionActor::~PositionActor() { node_.set_groups(0, 0); }  // the package no longer names its cells

bool PositionActor::start(uint32_t now_ms) {
    node_.set_groups(cfg_.cells[0], cfg_.cells[1]);  // M19: admission follows the package
    const uint16_t me = node_.config().node_id;
    if (cfg_.placed()) {
        if (node_.ns().declare(position_decl(me, cfg_.x_hash)) != NsError::Ok) return false;
        if (node_.ns().declare(position_decl(me, cfg_.y_hash)) != NsError::Ok) return false;
    }
    if (cfg_.room != 0 &&
        node_.ns().declare(position_decl(me, cfg_.room_hash, Unit::None, ValueType::U32)) != NsError::Ok) {
        return false;
    }
    if (cfg_.near_count() != 0 &&
        node_.ns().declare(position_decl(me, cfg_.near_hash, Unit::None, ValueType::Bytes)) != NsError::Ok) {
        return false;
    }
    next_ms_ = now_ms;
    return true;
}

void PositionActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + kPositionPeriodMs;
    if (cfg_.placed()) {
        node_.publish(cfg_.x_hash, Value::of_f32(cfg_.x));
        node_.publish(cfg_.y_hash, Value::of_f32(cfg_.y));
    }
    if (cfg_.room != 0) node_.publish(cfg_.room_hash, Value::of_u32(cfg_.room));
    const uint8_t n = cfg_.near_count();
    if (n != 0) {
        uint8_t b[2 * kPositionNearMax] = {};
        for (uint8_t i = 0; i < n; ++i) {
            b[2 * i] = static_cast<uint8_t>(cfg_.near[i]);
            b[2 * i + 1] = static_cast<uint8_t>(cfg_.near[i] >> 8);
        }
        node_.publish(cfg_.near_hash, Value::of_bytes(b, static_cast<uint8_t>(2 * kPositionNearMax)));
    }
}

size_t PositionActor::stats_json(char* buf, size_t cap, uint32_t now_ms) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"position\",\"node\":%u,\"up_ms\":%u,\"x\":%.2f,\"y\":%.2f,\"room\":%u,"
                                "\"cells\":[%u,%u],\"cell_refusals\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(now_ms),
                                static_cast<double>(cfg_.x), static_cast<double>(cfg_.y),
                                static_cast<unsigned>(cfg_.room), static_cast<unsigned>(cfg_.cells[0]),
                                static_cast<unsigned>(cfg_.cells[1]), static_cast<unsigned>(node_.group_refusals()));
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
    if (peer != 0 || cap < 4 || node != d.node_id || !position_config(d, c)) return 0;
    size_t n = 0;
    if (c.placed()) {
        out[n++] = position_decl(node, c.x_hash);
        out[n++] = position_decl(node, c.y_hash);
    }
    if (c.room != 0) out[n++] = position_decl(node, c.room_hash, Unit::None, ValueType::U32);
    if (c.near_count() != 0) out[n++] = position_decl(node, c.near_hash, Unit::None, ValueType::Bytes);
    return n;
}
}  // namespace

const ActorKind kPositionKind = {ActorType::Position, "position", &position_check, &position_create, nullptr,
                                 &position_outputs, true};  // others adopt them when they read them

}  // namespace pot
