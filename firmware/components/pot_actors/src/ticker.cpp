// M8.1: the ticker as an actor. See ticker.hpp.

#include "pot/ticker.hpp"

#include <new>

namespace pot {

void TickerActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + cfg_.period_ms;
    ++count_;
    const uint32_t v = (static_cast<uint32_t>(node_.config().node_id) << 16) | (count_ & 0xFFFFu);
    node_.publish(cfg_.out_hash, Value::of_u32(v));
}

namespace {
bool ticker_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    TickerConfig c{};
    if (d.node_id != kPortableNode || !ticker_config(d, c)) {
        *why = "ticker must be portable, with 1..8 distinct eligible nodes";
        return false;
    }
    return true;
}
Actor* ticker_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(TickerActor) <= kActorSlotBytes, "TickerActor outgrew its actor slot");
    TickerConfig c{};
    if (env.node == nullptr || !ticker_config(d, c)) return nullptr;
    return new (mem) TickerActor(*env.node, c);
}
bool ticker_placement(const ActorDecl& d, TickerConfig& out) { return ticker_config(d, out); }
}  // namespace

const ActorKind kTickerKind = {ActorType::Ticker, "ticker", &ticker_check, &ticker_create, &ticker_placement};

}  // namespace pot
