// M8.1: the runtime half of the actor API. See actor.hpp.

#include "pot/actor.hpp"

#include <cstring>

namespace pot {

const ActorKind* find_actor_kind(const ActorKind* table, size_t n, ActorType type) {
    for (size_t i = 0; i < n; ++i) {
        if (table[i].type == type) return &table[i];
    }
    return nullptr;
}

bool ActorRuntime::load(const DeployImage& img, uint16_t node_id, const ActorEnv& env, const char** why) {
    n_decl_ = 0;
    const char* w = nullptr;
    for (uint8_t i = 0; i < img.actor_count && w == nullptr; ++i) {
        const ActorDecl& a = img.actors[i];
        if (a.node_id == kPortableNode) continue;  // the reconciler's
        if (a.node_id != node_id && a.node_id != kEveryNode) continue;
        const ActorKind* k = find_actor_kind(table_, n_kinds_, a.type);
        if (k == nullptr) {
            w = "actor type this build has no row for";
        } else if (n_decl_ >= kMaxPinnedActors) {
            w = "more pinned actors on one node than kMaxPinnedActors";
        } else if (a.cfg_len > kMaxActorCfg) {
            w = "actor config longer than kMaxActorCfg";
        } else if (!k->check(a, env, &w)) {
            if (w == nullptr) w = "actor config invalid";
        } else {
            Held& h = held_[n_decl_++];
            h.kind = k;
            std::memcpy(h.cfg, a.cfg, a.cfg_len);
            h.decl = a;
            h.decl.cfg = h.cfg;  // the image buffer is reused once boot is over; this copy is not
        }
    }
    if (w != nullptr) {
        n_decl_ = 0;
        if (why != nullptr) *why = w;
        return false;
    }
    return true;
}

size_t ActorRuntime::start(const ActorEnv& env, uint32_t now_ms, const char** failed) {
    if (failed != nullptr) *failed = nullptr;
    for (size_t i = 0; i < n_decl_ && n_run_ < kMaxPinnedActors; ++i) {
        const Held& h = held_[i];
        Actor* a = h.kind->create(mem_[n_run_], h.decl, env);
        if (a == nullptr || !a->start(now_ms)) {
            if (a != nullptr) a->~Actor();
            if (failed != nullptr && *failed == nullptr) *failed = h.kind->name;
            continue;
        }
        run_kind_[n_run_] = h.kind;
        run_[n_run_++] = a;
    }
    return n_run_;
}

void ActorRuntime::tick(uint32_t now_ms) {
    for (size_t i = 0; i < n_run_; ++i) run_[i]->tick(now_ms);
}

bool ActorRuntime::on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash,
                                  Node::CallOutcome o, const Value& v) {
    for (size_t i = 0; i < n_run_; ++i) {
        if (run_[i]->on_call_result(from_node, msg_id, path_hash, o, v)) return true;
    }
    return false;
}

bool ActorRuntime::on_console(const char* line, size_t len) {
    for (size_t i = 0; i < n_run_; ++i) {
        if (run_[i]->on_console(line, len)) return true;
    }
    return false;
}

ActorRuntime::~ActorRuntime() {
    for (size_t i = 0; i < n_run_; ++i) run_[i]->~Actor();
}

}  // namespace pot
