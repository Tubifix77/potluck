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

bool merge_actor_tables(const ActorKind* builtins, size_t n_builtins, const AppRows* apps, size_t n_apps,
                        ActorKind* out, size_t cap, size_t& n_out, const char** why, const char** a,
                        const char** b) {
    const char* owner[kMaxActorKinds] = {};
    n_out = 0;
    auto refuse = [&](const char* w, const char* x, const char* y) {
        if (why != nullptr) *why = w;
        if (a != nullptr) *a = x;
        if (b != nullptr) *b = y;
        n_out = n_builtins < cap ? n_builtins : cap;
        for (size_t i = 0; i < n_out; ++i) out[i] = builtins[i];
        return false;
    };
    for (size_t i = 0; i < n_builtins; ++i) {
        if (n_out >= cap) return refuse("more actor types than the table holds", "potluck", nullptr);
        owner[n_out] = "potluck";
        out[n_out++] = builtins[i];
    }
    for (size_t c = 0; c < n_apps; ++c) {
        size_t n = 0;
        const ActorKind* rows = apps[c].rows != nullptr ? apps[c].rows(&n) : nullptr;
        for (size_t i = 0; rows != nullptr && i < n; ++i) {
            if (!is_external_type(rows[i].type)) {
                return refuse("an external component's actor type below 0x80 (Potluck's range)",
                              apps[c].component, nullptr);
            }
            for (size_t j = 0; j < n_out; ++j) {
                if (out[j].type == rows[i].type) {
                    return refuse("two rows for one actor type", owner[j], apps[c].component);
                }
            }
            if (n_out >= cap) return refuse("more actor types than the table holds", apps[c].component, nullptr);
            owner[n_out] = apps[c].component;
            out[n_out++] = rows[i];
        }
    }
    return true;
}

bool ActorRuntime::load(const DeployImage& img, uint16_t node_id, const ActorEnv& env, const char** why) {
    n_decl_ = 0;
    n_remote_ = 0;
    const char* w = nullptr;
    for (uint8_t i = 0; i < img.actor_count && w == nullptr; ++i) {
        const ActorDecl& a = img.actors[i];
        if (a.node_id == kPortableNode) continue;  // the reconciler's
        if (a.node_id != node_id && a.node_id != kEveryNode) {
            // Another node's actor: not ours to run, but its output is ours to know about.
            const ActorKind* k = find_actor_kind(table_, n_kinds_, a.type);
            NsDecl d;
            if (k != nullptr && k->output != nullptr && n_remote_ < kMaxActors && k->output(a, d)) {
                remote_[n_remote_++] = d;
            }
            continue;
        }
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
        n_remote_ = 0;
        if (why != nullptr) *why = w;
        return false;
    }
    return true;
}

size_t ActorRuntime::start(const ActorEnv& env, uint32_t now_ms, const char** failed) {
    if (failed != nullptr) *failed = nullptr;
    if (env.node != nullptr) {
        for (size_t i = 0; i < n_remote_; ++i) env.node->ns().declare(remote_[i]);
    }
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
