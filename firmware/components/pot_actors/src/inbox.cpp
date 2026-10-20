// 2026-10-20: an inbox -- values a program outside the cell writes. See pot/inbox.hpp.

#include "pot/inbox.hpp"

#include <cstdio>
#include <new>

namespace pot {

namespace {
uint32_t u32_at(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
bool scalar(ValueType t) { return t != ValueType::None && t != ValueType::Bytes && value_type_size(t) != 0; }
}  // namespace

bool inbox_config(const ActorDecl& a, InboxConfig& out) {
    if (a.type != ActorType::Inbox || a.cfg_len < 5) return false;
    const uint8_t* p = a.cfg;
    out.stale_ms = u32_at(p);
    out.n = p[4];
    if (out.n == 0 || out.n > kMaxOutputs || a.cfg_len != 5u + 6u * out.n) return false;
    for (size_t i = 0; i < out.n; ++i) {
        const uint8_t* q = p + 5 + 6 * i;
        out.hash[i] = u32_at(q);
        out.type[i] = static_cast<ValueType>(q[4]);
        out.unit[i] = static_cast<Unit>(q[5]);
        if (out.hash[i] == 0 || !scalar(out.type[i])) return false;
        for (size_t k = 0; k < i; ++k) {
            if (out.hash[k] == out.hash[i]) return false;
        }
    }
    return true;
}

NsDecl inbox_decl(uint16_t node, const InboxConfig& c, size_t i) {
    NsDecl d;
    d.path_hash = c.hash[i];
    d.owner_node = node;
    d.type = c.type[i];
    d.unit = c.unit[i];
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;  // written by the program through the hub, where the package's outside allows it
    d.latency_class = kClassL4;
    d.staleness_bound_ms = c.stale_ms;
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

bool InboxActor::start(uint32_t) {
    for (size_t i = 0; i < cfg_.n; ++i) {
        if (node_.ns().declare(inbox_decl(node_.config().node_id, cfg_, i)) != NsError::Ok) return false;
        const NsEntry* e = node_.ns().find(cfg_.hash[i]);
        last_count_[i] = e != nullptr ? e->update_count : 0;
    }
    return true;
}

void InboxActor::tick(uint32_t) {
    for (size_t i = 0; i < cfg_.n; ++i) {
        const NsEntry* e = node_.ns().find(cfg_.hash[i]);
        if (e != nullptr && e->update_count != last_count_[i]) {
            writes_ += e->update_count - last_count_[i];
            last_count_[i] = e->update_count;
        }
    }
}

size_t InboxActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap, "{\"t\":\"inbox\",\"node\":%u,\"outputs\":%u,\"writes\":%lu}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(cfg_.n),
                                static_cast<unsigned long>(writes_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {
bool inbox_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    InboxConfig c{};
    if (d.node_id == kPortableNode || d.node_id == kEveryNode || !inbox_config(d, c)) {
        *why = "inbox must be pinned, with 1..12 distinct outputs of a scalar type";
        return false;
    }
    return true;
}
Actor* inbox_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(InboxActor) <= kActorSlotBytes, "InboxActor outgrew its actor slot");
    InboxConfig c{};
    if (env.node == nullptr || !inbox_config(d, c)) return nullptr;
    return new (mem) InboxActor(*env.node, c);
}
size_t inbox_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    InboxConfig c{};
    if (peer != 0 || node != d.node_id || !inbox_config(d, c)) return 0;
    size_t n = 0;
    for (; n < c.n && n < cap; ++n) out[n] = inbox_decl(node, c, n);
    return n;
}
}  // namespace

const ActorKind kInboxKind = {ActorType::Inbox, "inbox", &inbox_check, &inbox_create, nullptr, &inbox_outputs};

}  // namespace pot
