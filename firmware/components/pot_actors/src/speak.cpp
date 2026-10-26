// M23: a board asks the PC to speak. The behaviour is specified in speak.hpp.

#include "pot/speak.hpp"

#include <cstdio>
#include <new>

namespace pot {

namespace {
NsDecl speak_ctl_decl(const SpeakConfig& c, uint16_t node) {
    NsDecl d;
    d.path_hash = c.ctl_hash;
    d.owner_node = node;
    d.type = ValueType::U32;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;  // written through WRITE: by the cell, or from outside where the package allows it
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 0;      // a request, not a measurement: it does not go stale
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}
NsDecl speak_out_decl(const SpeakConfig& c, uint16_t node) {
    NsDecl d = speak_ctl_decl(c, node);
    d.path_hash = c.out_hash;
    d.access = Access::Read;
    // Published at least every 2 s (tick): a copy on another board is read again past this. With no bound the
    // copies kept their first value for good (found 2026-10-25, as key_echo's were).
    d.staleness_bound_ms = 10000;
    return d;
}
}  // namespace

bool SpeakActor::start(uint32_t now_ms) {
    const uint16_t self = node_.config().node_id;
    if (node_.ns().declare(speak_ctl_decl(cfg_, self)) != NsError::Ok) return false;
    if (node_.ns().declare(speak_out_decl(cfg_, self)) != NsError::Ok) return false;
    const NsEntry* ctl = node_.ns().find(cfg_.ctl_hash);
    if (ctl != nullptr && !ctl->has_data()) node_.ns().publish(cfg_.ctl_hash, Value::of_u32(0), now_ms);
    if (ctl != nullptr) ctl_count_ = ctl->update_count;  // a write before this start is not a request
    node_.ns().publish(cfg_.out_hash, Value::of_u32(result_), now_ms);
    published_at_ = now_ms;
    now_ = now_ms;
    return true;
}

void SpeakActor::finish(uint32_t result, uint32_t now_ms) {
    result_ = result;
    if (result == kSpeakDone) ++stats_.done;
    if (result == kSpeakRefused) ++stats_.refused;
    if (result == kSpeakGone) ++stats_.gone;
    node_.ns().publish(cfg_.out_hash, Value::of_u32(result), now_ms);
    published_at_ = now_ms;
}

void SpeakActor::tick(uint32_t now) {
    now_ = now;
    if (in_flight_ != 0 && now - in_flight_since_ >= kSpeakDeadlineMs) {
        // Alive but silent: written off. cancel_call reports it, once, as Unavailable -- a timeout, not a loss.
        const uint16_t id = in_flight_;
        cancelling_ = id;
        node_.cancel_call(id);
        cancelling_ = 0;
        if (in_flight_ == id) {  // the node no longer knew the call: free the slot anyway
            in_flight_ = 0;
            finish(kSpeakGone, now);
        }
    }
    if (now - published_at_ >= 2000) {  // fresh between results, so copies elsewhere are read again
        node_.ns().publish(cfg_.out_hash, Value::of_u32(result_), now);
        published_at_ = now;
    }
    const NsEntry* ctl = node_.ns().find(cfg_.ctl_hash);
    if (ctl == nullptr || ctl->update_count == ctl_count_) return;
    ctl_count_ = ctl->update_count;
    ++stats_.asked;
    if (in_flight_ != 0) {
        ++stats_.busy;
        return;
    }
    const PeerLink* p = node_.peers().find_by_node_id(cfg_.provider);
    if (p == nullptr || p->state != PeerState::Alive) {
        finish(kSpeakGone, now);  // the PC is off: said at once, never waited for
        return;
    }
    const uint16_t id = node_.request_call(cfg_.provider, cfg_.svc_hash, cfg_.text, cfg_.text_len);
    if (id == 0) {
        finish(kSpeakGone, now);
        return;
    }
    ++stats_.calls;
    in_flight_ = id;
    in_flight_since_ = now;
}

bool SpeakActor::on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                                const Value& v) {
    if (path_hash != cfg_.svc_hash || from_node != cfg_.provider || msg_id == 0 || msg_id != in_flight_) return false;
    in_flight_ = 0;
    uint32_t said = 0;
    switch (o) {
        case Node::CallOutcome::Ok:
            finish(v.as_u32(said) && said != 0 ? kSpeakDone : kSpeakRefused, now_);
            break;
        case Node::CallOutcome::Refused:
            finish(kSpeakRefused, now_);
            break;
        case Node::CallOutcome::Unavailable:
            finish(kSpeakGone, now_);  // died with it, or cancelled at the deadline
            break;
    }
    return true;
}

size_t SpeakActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"speak\",\"node\":%u,\"result\":%u,\"asked\":%u,\"calls\":%u,\"done\":%u,"
                                "\"refused\":%u,\"gone\":%u,\"busy\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(result_),
                                static_cast<unsigned>(stats_.asked), static_cast<unsigned>(stats_.calls),
                                static_cast<unsigned>(stats_.done), static_cast<unsigned>(stats_.refused),
                                static_cast<unsigned>(stats_.gone), static_cast<unsigned>(stats_.busy));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- registration ---------------------------------------------------------------------------------------

bool speak_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    SpeakConfig c{};
    if (!speak_config(d, c)) {
        *why = "speak config invalid";
        return false;
    }
    return true;
}

size_t speak_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    SpeakConfig c{};
    if (peer != 0 || cap < 2 || node != d.node_id || !speak_config(d, c)) return 0;
    out[0] = speak_out_decl(c, node);
    out[1] = speak_ctl_decl(c, node);
    return 2;
}

namespace {
Actor* speak_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(SpeakActor) <= kActorSlotBytes, "SpeakActor outgrew its actor slot");
    SpeakConfig c{};
    if (env.node == nullptr || !speak_config(d, c)) return nullptr;
    return new (mem) SpeakActor(*env.node, c);
}
}  // namespace

const ActorKind kSpeakKind = {ActorType::Speak, "speak", &speak_check, &speak_create, nullptr, &speak_outputs};

}  // namespace pot
