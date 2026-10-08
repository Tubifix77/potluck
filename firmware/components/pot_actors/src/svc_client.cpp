// M8's service client. The behaviour is specified in svc_client.hpp.

#include "pot/svc_client.hpp"

namespace pot {

SvcClient::SvcClient(Node& node, const SvcClientConfig& cfg) : node_(node), cfg_(cfg) {}

bool SvcClient::start(uint32_t now_ms) {
    NsDecl d;
    d.path_hash = cfg_.out_hash;
    d.owner_node = node_.config().node_id;
    d.type = ValueType::None;  // whatever the service answers
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;  // section 7.5: "the class is L4 by definition"
    // Three periods: one late answer is not staleness; a host that has stopped answering soon is.
    d.staleness_bound_ms = 3u * cfg_.period_ms;
    d.staleness_policy = StalenessPolicy::Informative;
    if (node_.ns().declare(d) != NsError::Ok) return false;
    next_call_ms_ = now_ms;
    started_ = true;
    return true;
}

void SvcClient::degrade() {
    if (state_ != State::Degraded) {
        state_ = State::Degraded;
        ++stats_.degradations;
    }
}

void SvcClient::tick(uint32_t now) {
    if (!started_) return;
    now_ = now;
    // A provider that is alive but silent: give up on the call after one period (at least a second),
    // so a hung service cannot hold the slot. cancel_call reports it, once, as Unavailable.
    if (in_flight_ != 0) {
        const uint32_t deadline = cfg_.period_ms < 1000 ? 1000u : cfg_.period_ms;
        if (now - in_flight_since_ >= deadline) {
            const uint16_t id = in_flight_;
            cancelling_ = id;  // the Unavailable that cancel_call reports is a timeout, not a loss
            node_.cancel_call(id);
            cancelling_ = 0;
            if (in_flight_ == id) {
                in_flight_ = 0;  // cancel refused (already resolved): free the slot anyway
                ++stats_.timed_out;
                degrade();
            }
        }
        return;
    }
    if (static_cast<int32_t>(now - next_call_ms_) < 0) return;
    next_call_ms_ = now + cfg_.period_ms;
    const PeerLink* p = node_.peers().find_by_node_id(cfg_.provider);
    if (p == nullptr || p->state != PeerState::Alive) {
        ++stats_.not_sent;
        if (state_ == State::Serving) degrade();
        return;
    }
    const uint16_t id = node_.request_call(cfg_.provider, cfg_.svc_hash, nullptr, 0);
    if (id == 0) {
        ++stats_.not_sent;
        return;
    }
    ++stats_.calls;
    in_flight_ = id;
    in_flight_since_ = now;
}

bool SvcClient::on_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome outcome,
                          const Value& v) {
    if (path_hash != cfg_.svc_hash || from_node != cfg_.provider) return false;
    if (msg_id == in_flight_) in_flight_ = 0;
    switch (outcome) {
        case Node::CallOutcome::Ok:
            if (node_.publish(cfg_.out_hash, v) == NsError::Ok) {
                ++stats_.answered;
                stats_.last_answer_ms = now_ == 0 ? 1 : now_;
                if (state_ == State::Degraded) ++stats_.recoveries;
                state_ = State::Serving;
            }
            break;
        case Node::CallOutcome::Refused:
            ++stats_.refused;
            degrade();
            break;
        case Node::CallOutcome::Unavailable:
            if (msg_id != 0 && msg_id == cancelling_) {
                ++stats_.timed_out;
            } else {
                ++stats_.lost;
            }
            degrade();
            break;
    }
    return true;
}

}  // namespace pot
