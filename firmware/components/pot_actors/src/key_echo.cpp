// M24: the test app for events from the PC. The behaviour is specified in key_echo.hpp.

#include "pot/key_echo.hpp"

#include <cstdio>
#include <new>

namespace pot {

namespace {
NsDecl key_echo_decl(uint32_t hash, uint16_t node) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = node;
    d.type = ValueType::U32;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 0;  // changes when an event arrives, not on a schedule: it does not go stale
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}
}  // namespace

bool KeyEchoActor::start(uint32_t now_ms) {
    const uint16_t self = node_.config().node_id;
    const uint32_t outs[3] = {cfg_.key_hash, cfg_.count_hash, cfg_.gaps_hash};
    for (uint32_t h : outs) {
        if (node_.ns().declare(key_echo_decl(h, self)) != NsError::Ok) return false;
        node_.ns().publish(h, Value::of_u32(0), now_ms);
    }
    now_ = now_ms;
    return true;
}

int KeyEchoActor::on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args,
                          uint16_t len) {
    if (path_hash != cfg_.in_hash) return kCallNotMine;
    InputEvent ev[kInputBatchMax];
    const int n = parse_input(args, len, ev, kInputBatchMax);
    if (n < 0) {
        ++bad_;
        return kCallRefused;
    }
    ++batches_;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        if (!order_.take(ev[i].seq)) continue;
        any = true;
        if (ev[i].kind == kInputKey) {
            key_ = static_cast<uint16_t>(ev[i].a) | (static_cast<uint32_t>(ev[i].flags & 1u) << 16);
        }
    }
    if (any) {
        node_.ns().publish(cfg_.key_hash, Value::of_u32(key_), now_);
        node_.ns().publish(cfg_.count_hash, Value::of_u32(order_.taken), now_);
        node_.ns().publish(cfg_.gaps_hash, Value::of_u32(order_.missed), now_);
    }
    node_.reply_call(from_node, msg_id, path_hash, Value::of_u32(order_.last));
    return kCallAccepted;
}

size_t KeyEchoActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"key_echo\",\"node\":%u,\"batches\":%u,\"taken\":%u,\"dropped\":%u,"
                                "\"missed\":%u,\"bad\":%u,\"key\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(batches_),
                                static_cast<unsigned>(order_.taken), static_cast<unsigned>(order_.dropped),
                                static_cast<unsigned>(order_.missed), static_cast<unsigned>(bad_),
                                static_cast<unsigned>(key_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- registration ---------------------------------------------------------------------------------------

bool key_echo_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    KeyEchoConfig c{};
    if (!key_echo_config(d, c)) {
        *why = "key_echo config invalid";
        return false;
    }
    return true;
}

size_t key_echo_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    KeyEchoConfig c{};
    if (peer != 0 || cap < 3 || node != d.node_id || !key_echo_config(d, c)) return 0;
    out[0] = key_echo_decl(c.key_hash, node);
    out[1] = key_echo_decl(c.count_hash, node);
    out[2] = key_echo_decl(c.gaps_hash, node);
    return 3;
}

namespace {
Actor* key_echo_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(KeyEchoActor) <= kActorSlotBytes, "KeyEchoActor outgrew its actor slot");
    KeyEchoConfig c{};
    if (env.node == nullptr || !key_echo_config(d, c)) return nullptr;
    return new (mem) KeyEchoActor(*env.node, c);
}
}  // namespace

const ActorKind kKeyEchoKind = {ActorType::KeyEcho, "key_echo", &key_echo_check, &key_echo_create, nullptr,
                                &key_echo_outputs};

}  // namespace pot
