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

bool ActorRuntime::collect(const DeployImage& img, uint16_t node_id, const ActorEnv& env, Held* held,
                           size_t& n_held, Held* others, size_t& n_others, const char** why) const {
    n_held = 0;
    n_others = 0;
    const char* w = nullptr;
    for (uint8_t i = 0; i < img.actor_count && w == nullptr; ++i) {
        const ActorDecl& a = img.actors[i];
        if (a.node_id == kPortableNode) continue;  // the reconciler's
        if (a.node_id != node_id || a.node_id == kEveryNode) {
            // It runs (also) on other nodes: not ours to run there, but its outputs are ours to know.
            const ActorKind* k = find_actor_kind(table_, n_kinds_, a.type);
            if (k != nullptr && k->outputs != nullptr && n_others < kMaxActors && a.cfg_len <= kMaxActorCfg) {
                Held& o = others[n_others++];
                o.kind = k;
                std::memcpy(o.cfg, a.cfg, a.cfg_len);
                o.decl = a;
                o.decl.cfg = o.cfg;
            }
            if (a.node_id != kEveryNode) continue;
        }
        const ActorKind* k = find_actor_kind(table_, n_kinds_, a.type);
        if (k == nullptr) {
            w = "actor type this build has no row for";
        } else if (n_held >= kMaxPinnedActors) {
            w = "more pinned actors on one node than kMaxPinnedActors";
        } else if (a.cfg_len > kMaxActorCfg) {
            w = "actor config longer than kMaxActorCfg";
        } else if (!k->check(a, env, &w)) {
            if (w == nullptr) w = "actor config invalid";
        } else {
            Held& h = held[n_held++];
            h.kind = k;
            std::memcpy(h.cfg, a.cfg, a.cfg_len);
            h.decl = a;
            h.decl.cfg = h.cfg;  // the image buffer is reused once boot is over; this copy is not
        }
    }
    if (w != nullptr) {
        n_held = 0;
        n_others = 0;
        if (why != nullptr) *why = w;
        return false;
    }
    return true;
}

bool ActorRuntime::load(const DeployImage& img, uint16_t node_id, const ActorEnv& env, const char** why) {
    n_remote_ = 0;
    return collect(img, node_id, env, held_, n_decl_, others_, n_others_, why);
}

bool ActorRuntime::same(const Held& a, const Held& b) {
    return a.decl.type == b.decl.type && a.decl.node_id == b.decl.node_id && a.decl.cfg_len == b.decl.cfg_len &&
           std::memcmp(a.cfg, b.cfg, a.decl.cfg_len) == 0;
}

bool ActorRuntime::start_one(const Held& h, const ActorEnv& env, uint32_t now_ms) {
    if (n_run_ >= kMaxPinnedActors) return false;
    size_t slot = 0;
    while (slot < kMaxPinnedActors && slot_used_[slot]) ++slot;
    if (slot >= kMaxPinnedActors) return false;
    run_held_[n_run_] = h;
    run_held_[n_run_].decl.cfg = run_held_[n_run_].cfg;
    Actor* a = h.kind->create(mem_[slot], run_held_[n_run_].decl, env);
    if (a == nullptr || !a->start(now_ms)) {
        if (a != nullptr) a->~Actor();
        return false;
    }
    slot_used_[slot] = true;
    run_slot_[n_run_] = static_cast<uint8_t>(slot);
    run_kind_[n_run_] = h.kind;
    run_[n_run_++] = a;
    return true;
}

size_t ActorRuntime::start(const ActorEnv& env, uint32_t now_ms, const char** failed) {
    if (failed != nullptr) *failed = nullptr;
    if (env.node != nullptr) {
        // Pinned elsewhere: its per-node outputs are known now, so declare their replicas at boot.
        for (size_t i = 0; i < n_others_; ++i) {
            const Held& o = others_[i];
            if (o.decl.node_id == kEveryNode) continue;  // adopted on demand: the members are not known yet
            if (o.kind->outputs_on_demand) continue;      // M20: adopted when first read (a position, a room)
            NsDecl out[kMaxOutputs];
            const size_t n = o.kind->outputs(o.decl, o.decl.node_id, 0, out, kMaxOutputs);
            for (size_t j = 0; j < n && j < kMaxOutputs; ++j) {
                if (env.node->ns().declare(out[j]) == NsError::Ok) ++n_remote_;
            }
        }
    }
    for (size_t i = 0; i < n_decl_ && n_run_ < kMaxPinnedActors; ++i) {
        if (!start_one(held_[i], env, now_ms) && failed != nullptr && *failed == nullptr) *failed = held_[i].kind->name;
    }
    return n_run_;
}

size_t ActorRuntime::remove_outputs(Namespace& ns, const Held& h, bool own, uint16_t self, const uint16_t* nodes,
                                    size_t n_nodes) {
    if (h.kind->outputs == nullptr) return 0;
    size_t removed = 0;
    NsDecl out[kMaxOutputs];
    const bool every = h.decl.node_id == kEveryNode;
    // Our own instance publishes on this node only; a replica's outputs may sit on any node it ran on.
    const size_t n_on = own ? 1 : (every ? n_nodes : 1);
    for (size_t a = 0; a < n_on; ++a) {
        const uint16_t on = own ? self : (every ? nodes[a] : h.decl.node_id);
        for (size_t b = 0; b <= n_nodes; ++b) {
            const uint16_t peer = (b == 0) ? 0 : nodes[b - 1];
            if (peer == on) continue;
            const size_t n = h.kind->outputs(h.decl, on, peer, out, kMaxOutputs);
            for (size_t j = 0; j < n && j < kMaxOutputs; ++j) {
                if (ns.remove(out[j].path_hash) == NsError::Ok) ++removed;
            }
        }
    }
    return removed;
}

bool ActorRuntime::reload(const DeployImage& img, uint16_t node_id, const ActorEnv& env, uint32_t now_ms,
                          const uint16_t* nodes, size_t n_nodes, const char** why, ReloadStats* stats) {
    ReloadStats st;
    Held* held = stage_held_;
    Held* others = stage_others_;
    size_t n_held = 0, n_others = 0;
    if (!collect(img, node_id, env, held, n_held, others, n_others, why)) return false;  // nothing changed

    // Match running actors to the new declarations: identical ones are kept, each at most once.
    bool matched[kMaxPinnedActors] = {};
    bool keep[kMaxPinnedActors] = {};
    for (size_t r = 0; r < n_run_; ++r) {
        for (size_t i = 0; i < n_held; ++i) {
            if (!matched[i] && same(run_held_[r], held[i])) {
                matched[i] = keep[r] = true;
                break;
            }
        }
    }
    // Stop the rest, and take what they published out of the namespace.
    size_t w = 0;
    for (size_t r = 0; r < n_run_; ++r) {
        if (keep[r]) {
            if (w != r) {
                run_[w] = run_[r];
                run_kind_[w] = run_kind_[r];
                run_held_[w] = run_held_[r];
                run_held_[w].decl.cfg = run_held_[w].cfg;
                run_slot_[w] = run_slot_[r];
            }
            ++w;
            ++st.kept;
            continue;
        }
        run_[r]->~Actor();
        slot_used_[run_slot_[r]] = false;
        if (env.node != nullptr) st.outputs_removed += remove_outputs(env.node->ns(), run_held_[r], true, node_id, nodes, n_nodes);
        ++st.stopped;
    }
    n_run_ = w;
    // Replicas of other nodes' actors that the new image no longer has.
    if (env.node != nullptr) {
        for (size_t i = 0; i < n_others_; ++i) {
            bool still = false;
            for (size_t j = 0; j < n_others && !still; ++j) still = same(others_[i], others[j]);
            if (!still) st.outputs_removed += remove_outputs(env.node->ns(), others_[i], false, node_id, nodes, n_nodes);
        }
    }
    // The new image's declarations become ours, then the new actors start.
    for (size_t i = 0; i < n_held; ++i) {
        held_[i] = held[i];
        held_[i].decl.cfg = held_[i].cfg;
    }
    n_decl_ = n_held;
    for (size_t i = 0; i < n_others; ++i) {
        others_[i] = others[i];
        others_[i].decl.cfg = others_[i].cfg;
    }
    n_others_ = n_others;
    if (env.node != nullptr) {
        for (size_t i = 0; i < n_others_; ++i) {
            const Held& o = others_[i];
            if (o.decl.node_id == kEveryNode) continue;  // adopted on demand, as at boot
            if (o.kind->outputs_on_demand) continue;      // M20: likewise (a position, a room), as at boot
            NsDecl out[kMaxOutputs];
            const size_t n = o.kind->outputs(o.decl, o.decl.node_id, 0, out, kMaxOutputs);
            for (size_t j = 0; j < n && j < kMaxOutputs; ++j) env.node->ns().declare(out[j]);
        }
    }
    for (size_t i = 0; i < n_decl_; ++i) {
        if (matched[i]) continue;
        if (start_one(held_[i], env, now_ms)) {
            ++st.started;
        } else if (st.failed == nullptr) {
            st.failed = held_[i].kind->name;
        }
    }
    if (stats != nullptr) *stats = st;
    return true;
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

int ActorRuntime::on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args,
                          uint16_t len) {
    for (size_t i = 0; i < n_run_; ++i) {
        const int r = run_[i]->on_call(from_node, msg_id, path_hash, args, len);
        if (r != Actor::kCallNotMine) return r;
    }
    return Actor::kCallNotMine;
}

bool ActorRuntime::adopt(Namespace& ns, uint32_t path_hash, const uint16_t* nodes, size_t n_nodes) {
    NsDecl out[kMaxOutputs];
    for (size_t i = 0; i < n_others_; ++i) {
        const Held& o = others_[i];
        const bool every = o.decl.node_id == kEveryNode;
        const size_t n_on = every ? n_nodes : 1;
        for (size_t a = 0; a < n_on; ++a) {
            const uint16_t on = every ? nodes[a] : o.decl.node_id;
            for (size_t b = 0; b <= n_nodes; ++b) {
                const uint16_t peer = (b == 0) ? 0 : nodes[b - 1];
                if (peer == on) continue;  // no link to itself
                const size_t n = o.kind->outputs(o.decl, on, peer, out, kMaxOutputs);
                for (size_t j = 0; j < n && j < kMaxOutputs; ++j) {
                    if (out[j].path_hash == path_hash) {
                        return ns.declare(out[j]) == NsError::Ok;
                    }
                }
            }
        }
    }
    return false;
}

void ActorRuntime::on_rx_sample(const RxSample& s) {
    for (size_t i = 0; i < n_run_; ++i) run_[i]->on_rx_sample(s);
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
