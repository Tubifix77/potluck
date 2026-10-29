// M28: a board asks the PC to play a pattern of notes. The behaviour is specified in tone.hpp.

#include "pot/tone.hpp"

#include <cstdio>
#include <new>

namespace pot {

namespace {
NsDecl tone_play_decl(const ToneConfig& c, uint16_t node) {
    NsDecl d;
    d.path_hash = c.play_hash;
    d.owner_node = node;
    d.type = ValueType::I32;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;  // written through WRITE: by the cell, or from outside where the package allows it
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 0;      // a request, not a measurement: it does not go stale
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}
NsDecl tone_out_decl(const ToneConfig& c, uint16_t node) {
    NsDecl d = tone_play_decl(c, node);
    d.path_hash = c.out_hash;
    d.type = ValueType::U32;
    d.access = Access::Read;
    d.staleness_bound_ms = 10000;  // republished every 2 s (tick): a copy on another board is read again past this
    return d;
}

// Any number a source may publish -- an alarm's i32 or u32, a bool, a float -- as a pattern request.
bool as_number(const Value& v, int32_t& out) {
    int32_t i = 0;
    uint32_t u = 0;
    bool b = false;
    float f = 0;
    if (v.as_i32(i)) {
        out = i;
    } else if (v.as_u32(u)) {
        out = u > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int32_t>(u);
    } else if (v.as_bool(b)) {
        out = b ? 1 : 0;
    } else if (v.as_f32(f)) {
        out = f != 0.0f ? 1 : 0;
    } else {
        return false;
    }
    return true;
}
}  // namespace

bool ToneActor::start(uint32_t now_ms) {
    const uint16_t self = node_.config().node_id;
    if (node_.ns().declare(tone_play_decl(cfg_, self)) != NsError::Ok) return false;
    if (node_.ns().declare(tone_out_decl(cfg_, self)) != NsError::Ok) return false;
    const NsEntry* play = node_.ns().find(cfg_.play_hash);
    if (play != nullptr && !play->has_data()) node_.ns().publish(cfg_.play_hash, Value::of_i32(0), now_ms);
    if (play != nullptr) play_count_ = play->update_count;  // a write before this start is not a request
    node_.ns().publish(cfg_.out_hash, Value::of_u32(result_), now_ms);
    published_at_ = now_ms;
    next_source_ms_ = now_ms;
    now_ = now_ms;
    return true;
}

void ToneActor::finish(uint32_t result, uint32_t now_ms) {
    result_ = result;
    if (result == kTonePlaying) ++stats_.playing;
    if (result == kToneRefused) ++stats_.refused;
    if (result == kToneGone) ++stats_.gone;
    node_.ns().publish(cfg_.out_hash, Value::of_u32(result), now_ms);
    published_at_ = now_ms;
}

void ToneActor::ask(int32_t pattern, uint32_t now) {
    ++stats_.asked;
    if (in_flight_ != 0) {
        if (has_waiting_) ++stats_.replaced;
        has_waiting_ = true;  // the newest waits for the answer; "stop" after "play" is never lost
        waiting_ = pattern;
        return;
    }
    const PeerLink* p = node_.peers().find_by_node_id(cfg_.provider);
    if (p == nullptr || p->state != PeerState::Alive) {
        finish(kToneGone, now);  // the PC is off: said at once, never waited for
        return;
    }
    uint8_t args[4];
    for (int i = 0; i < 4; ++i) args[i] = static_cast<uint8_t>(static_cast<uint32_t>(pattern) >> (8 * i));
    const uint16_t id = node_.request_call(cfg_.provider, cfg_.svc_hash, args, sizeof(args));
    if (id == 0) {
        finish(kToneGone, now);
        return;
    }
    ++stats_.calls;
    in_flight_ = id;
    in_flight_since_ = now;
}

void ToneActor::follow(uint32_t now) {
    if (cfg_.source_hash == 0 || static_cast<int32_t>(now - next_source_ms_) < 0) return;
    next_source_ms_ = now + kToneSourcePeriodMs;
    Reading r;
    bool local = true;
    if (node_.read(cfg_.source_hash, r, &local) != NsError::Ok) return;  // not declared yet: next period
    if (!local) {
        const NsEntry* e = node_.ns().find(cfg_.source_hash);
        if (e != nullptr && e->owner_node != 0) node_.request_read(e->owner_node, cfg_.source_hash);
    }
    int32_t v = 0;
    if (r.quality != Quality::Good || !as_number(r.value, v)) return;  // an old value does not start a sound
    if (has_source_ && v == source_) return;
    const bool first = !has_source_;
    has_source_ = true;
    source_ = v;
    if (first && v == 0) return;  // quiet at start: nothing to stop
    ask(v == 0 ? 0 : (cfg_.pattern != 0 ? static_cast<int32_t>(cfg_.pattern) : v), now);
}

void ToneActor::tick(uint32_t now) {
    now_ = now;
    if (in_flight_ != 0 && now - in_flight_since_ >= kToneDeadlineMs) {
        // Alive but silent: written off. cancel_call reports it, once, as Unavailable -- a timeout, not a loss.
        const uint16_t id = in_flight_;
        cancelling_ = id;
        node_.cancel_call(id);
        cancelling_ = 0;
        if (in_flight_ == id) {  // the node no longer knew the call: free the slot anyway
            in_flight_ = 0;
            finish(kToneGone, now);
        }
        if (in_flight_ == 0 && has_waiting_) {  // a request that waited on the written-off call goes now
            has_waiting_ = false;
            --stats_.asked;
            ask(waiting_, now);
        }
    }
    if (now - published_at_ >= 2000) {  // fresh between results, so copies elsewhere are read again
        node_.ns().publish(cfg_.out_hash, Value::of_u32(result_), now);
        published_at_ = now;
    }
    const NsEntry* play = node_.ns().find(cfg_.play_hash);
    if (play != nullptr && play->update_count != play_count_) {
        play_count_ = play->update_count;
        int32_t v = 0;
        if (!play->value.as_i32(v)) v = 0;
        ask(v, now);
    }
    follow(now);
}

bool ToneActor::on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                               const Value& v) {
    if (path_hash != cfg_.svc_hash || from_node != cfg_.provider || msg_id == 0 || msg_id != in_flight_) return false;
    in_flight_ = 0;
    uint32_t took = 0;
    switch (o) {
        case Node::CallOutcome::Ok:
            finish(v.as_u32(took) && took != 0 ? kTonePlaying : kToneRefused, now_);
            break;
        case Node::CallOutcome::Refused:
            finish(kToneRefused, now_);
            break;
        case Node::CallOutcome::Unavailable:
            finish(kToneGone, now_);  // died with it, or cancelled at the deadline
            break;
    }
    if (has_waiting_ && cancelling_ == 0) {
        has_waiting_ = false;
        --stats_.asked;  // counted when it arrived
        ask(waiting_, now_);
    }
    return true;
}

size_t ToneActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"tone\",\"node\":%u,\"result\":%u,\"asked\":%u,\"calls\":%u,\"playing\":%u,"
                                "\"refused\":%u,\"gone\":%u,\"replaced\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(result_),
                                static_cast<unsigned>(stats_.asked), static_cast<unsigned>(stats_.calls),
                                static_cast<unsigned>(stats_.playing), static_cast<unsigned>(stats_.refused),
                                static_cast<unsigned>(stats_.gone), static_cast<unsigned>(stats_.replaced));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- registration ---------------------------------------------------------------------------------------

bool tone_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    ToneConfig c{};
    if (!tone_config(d, c)) {
        *why = "tone config invalid";
        return false;
    }
    return true;
}

size_t tone_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    ToneConfig c{};
    if (peer != 0 || cap < 2 || node != d.node_id || !tone_config(d, c)) return 0;
    out[0] = tone_out_decl(c, node);
    out[1] = tone_play_decl(c, node);
    return 2;
}

namespace {
Actor* tone_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(ToneActor) <= kActorSlotBytes, "ToneActor outgrew its actor slot");
    ToneConfig c{};
    if (env.node == nullptr || !tone_config(d, c)) return nullptr;
    return new (mem) ToneActor(*env.node, c);
}
}  // namespace

const ActorKind kToneKind = {ActorType::Tone, "tone", &tone_check, &tone_create, nullptr, &tone_outputs};

}  // namespace pot
