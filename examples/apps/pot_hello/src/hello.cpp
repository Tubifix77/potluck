// pot_hello's one actor. See include/hello/hello.hpp.

#include "hello/hello.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#include "pot/namespace.hpp"

namespace pot {
namespace hello {

namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// The output's declaration -- the same on the actor's own node (owner = itself) and on every other node,
// which holds a replica (owner = the node that publishes). Read-only: nobody may write a count.
NsDecl output_decl(uint32_t system_prefix, uint16_t node_id, uint16_t period_ms) {
    NsDecl d;
    d.path_hash = output_hash(system_prefix, node_id);
    d.owner_node = node_id;
    d.type = ValueType::U32;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 3u * period_ms;  // one late period is not staleness; three are
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

}  // namespace

bool parse(const ActorDecl& d, Config& out) {
    if (d.type != kPeersType || d.cfg_len != kCfgLen || d.node_id == kPortableNode) return false;
    out.system_prefix = rd32(d.cfg);
    out.period_ms = static_cast<uint16_t>(d.cfg[4] | (d.cfg[5] << 8));
    return out.period_ms >= 1000 && out.period_ms <= 60000;
}

uint32_t output_hash(uint32_t system_prefix, uint16_t node_id) {
    char rest[32];
    std::snprintf(rest, sizeof(rest), "node-%04x/hello/peers", static_cast<unsigned>(node_id));
    return path_hash_from(system_prefix, rest);
}

uint32_t PeersActor::alive_peers() const {
    const NodeConfig& c = node_.config();
    uint32_t n = 0;
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& p = node_.peers().slot(i);
        if (p.state != PeerState::Alive) continue;
        if (c.has_trusted_mac && std::memcmp(p.mac, c.trusted_mac, kMacLen) == 0) continue;  // the PC's cable
        ++n;
    }
    return n;
}

bool PeersActor::start(uint32_t now_ms) {
    out_ = output_hash(cfg_.system_prefix, node_.config().node_id);
    if (node_.ns().declare(output_decl(cfg_.system_prefix, node_.config().node_id, cfg_.period_ms)) != NsError::Ok) {
        return false;  // the runtime logs it and drops this actor; the node runs on
    }
    next_ms_ = now_ms;
    return true;
}

void PeersActor::tick(uint32_t now_ms) {
    // tick() runs on the link task with the node's lock held, beside the heartbeat: return promptly.
    if (static_cast<int32_t>(now_ms - next_ms_) < 0) return;
    next_ms_ = now_ms + cfg_.period_ms;
    last_ = alive_peers();
    node_.ns().publish(out_, Value::of_u32(last_), now_ms);
    ++published_;
    // The LED shows an application state only in green's place -- on a board that hears its peers and has
    // no problem -- and lapses 5 s after the last call: "company" is shown for as long as it is true, and a
    // dead link or an update is never hidden. (A board alone is yellow, whatever an app asks.)
    if (board_ != nullptr && board_->led_app != nullptr) {
        board_->led_app(board_->ctx, last_ > 0, 0, 180, 180, 0);
    }
}

size_t PeersActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap, "{\"t\":\"hello\",\"node\":%u,\"peers\":%u,\"published\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(last_),
                                static_cast<unsigned>(published_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {

bool check(const ActorDecl& d, const ActorEnv&, const char** why) {
    Config c;
    if (!parse(d, c)) {
        *why = "hello/peers: every-node or pinned; system_prefix u32, period_ms u16 in 1000..60000";
        return false;
    }
    return true;
}

Actor* create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(PeersActor) <= kActorSlotBytes, "PeersActor outgrew its actor slot");
    Config c;
    if (env.node == nullptr || !parse(d, c)) return nullptr;
    return new (mem) PeersActor(*env.node, env.board, c);
}

// Every other node learns the actor's output from the deploy image, which they all hold, so a read of
// node-<id>/hello/peers anywhere finds a replica to answer from (pot/actor.hpp, ActorKind::outputs).
size_t outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    Config c;
    if (peer != 0 || cap == 0 || node == 0 || !parse(d, c)) return 0;  // per-node outputs only
    out[0] = output_decl(c.system_prefix, node, c.period_ms);
    return 1;
}

}  // namespace

ActorKind peers_row() { return {kPeersType, "hello_peers", &check, &create, nullptr, &outputs}; }

}  // namespace hello
}  // namespace pot
