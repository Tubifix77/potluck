// Moving packages through the cell. See pot/package_agent.hpp.

#include "pot/package_agent.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "pot/node.hpp"
#include "pot/opcodes.hpp"

namespace pot {

namespace {
constexpr uint32_t kRebootDelayMs = 1500;  // long enough for the REPLY to leave first
}  // namespace

PackageAgent::PackageAgent(Node& node, SlotStore& store, DeployReceiver& rx, uint8_t* buf, size_t cap,
                           DeployState& state, const PackageAgentHooks& hooks, const uint8_t* host_mac)
    : node_(node), store_(store), rx_(rx), buf_(buf), cap_(cap), state_(state), hooks_(hooks) {
    std::memset(host_mac_, 0, sizeof(host_mac_));
    if (host_mac != nullptr) std::memcpy(host_mac_, host_mac, kMacLen);
}

void PackageAgent::logf(const char* fmt, ...) {
    if (hooks_.log == nullptr) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    hooks_.log(hooks_.ctx, line);
}

void PackageAgent::set_running(uint32_t counter, const uint8_t digest[8]) {
    running_counter_ = counter;
    if (digest != nullptr) {
        std::memcpy(running_digest_, digest, sizeof(running_digest_));
    } else {
        std::memset(running_digest_, 0, sizeof(running_digest_));
    }
}

void PackageAgent::schedule_reboot(uint32_t now) { reboot_at_ms_ = now + kRebootDelayMs; }

bool PackageAgent::idle() const {
    const bool ready = hooks_.ready == nullptr || hooks_.ready(hooks_.ctx);
    const bool busy = hooks_.busy != nullptr && hooks_.busy(hooks_.ctx);
    return ready && !push_.active && reboot_at_ms_ == 0 && !busy && !rx_.in_progress();
}

// At this board's floor, running the package that set it, and `digest` sorts below the one running.
bool PackageAgent::lower_at_floor(const uint8_t digest[8]) const {
    return running_counter_ != 0 && running_counter_ == state_.counter &&
           std::memcmp(digest, running_digest_, sizeof(running_digest_)) < 0;
}

// ---- passing a committed image on to the peers (the cabled board's half of "one artifact per system") ----------

void PackageAgent::push_send(uint8_t op, const uint8_t* payload, size_t len, uint32_t now) {
    const uint16_t msg = node_.send_deploy(push_.peers[push_.idx], op, payload, static_cast<uint16_t>(len));
    if (msg == 0) {
        logf("deploy: could not send to peer 0x%04x", push_.peers[push_.idx]);
        ++push_.failed;
        ++push_.idx;
        push_next_peer(now);
        return;
    }
    push_.awaiting = msg;
}

void PackageAgent::push_begin(uint32_t now) {
    uint8_t wire[kDeployBeginLen];
    const size_t n = store_deploy_begin(push_.begin, wire, sizeof(wire));
    push_.offset = 0;
    push_send(kOpDeployBegin, wire, n, now);
}

void PackageAgent::push_chunk(uint32_t now) {
    const uint32_t len = push_.begin.image_len;
    const uint16_t n = static_cast<uint16_t>(std::min<uint32_t>(kDeployChunkMax, len - push_.offset));
    static uint8_t wire[kDeployChunkHeaderLen + kDeployChunkMax];  // off the link task's stack
    const size_t w = store_deploy_chunk(push_.offset, rx_.image() + push_.offset, n, wire, sizeof(wire));
    push_.offset += n;
    push_send(kOpDeployChunk, wire, w, now);
}

void PackageAgent::push_finish(uint32_t now) {
    push_.active = false;
    if (push_.catchup) {
        logf("deploy: catch-up of counter %u to peer 0x%04x %s", static_cast<unsigned>(push_.begin.rollback_counter),
             push_.peers[0], push_.ok ? "committed" : "failed");
        push_.catchup = false;
        return;
    }
    DeployReply r{};
    r.status = DeployStatus::Ok;
    r.slot = state_.active;
    r.pending = 1;
    r.received = push_.begin.image_len;
    r.counter = push_.begin.rollback_counter;
    r.peers_ok = push_.ok;
    r.peers_failed = push_.failed;
    uint8_t wire[kDeployReplyLen];
    store_deploy_reply(r, wire, sizeof(wire));
    node_.send_reply_raw(push_.requester, push_.requester_msg, wire, sizeof(wire));
    logf("deploy: passed on to %u peer(s), %u failed; %s", static_cast<unsigned>(push_.ok),
         static_cast<unsigned>(push_.failed), live_ ? "running, live, on trial" : "rebooting into it");
    if (!live_) schedule_reboot(now);
}

void PackageAgent::push_next_peer(uint32_t now) {
    if (push_.idx >= push_.count) {
        push_finish(now);
        return;
    }
    push_begin(now);
}

bool PackageAgent::on_result(uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p, uint16_t len,
                             uint32_t now) {
    if (!push_.active || push_.idx >= push_.count || peer != push_.peers[push_.idx] || msg != push_.awaiting) {
        return false;
    }
    DeployReply r{};
    const bool ok = !timed_out && load_deploy_reply(p, len, r) && r.status == DeployStatus::Ok;
    if (!ok) {
        logf("deploy: peer 0x%04x %s at step 0x%02x", peer, timed_out ? "timed out" : deploy_status_str(r.status), op);
        ++push_.failed;
        ++push_.idx;
        push_next_peer(now);
        return true;
    }
    if (op == kOpDeployBegin || (op == kOpDeployChunk && push_.offset < push_.begin.image_len)) {
        push_chunk(now);
    } else if (op == kOpDeployChunk) {
        uint8_t wire[kDeployCommitLen];
        push_send(kOpDeployCommit, wire, store_deploy_commit(push_.begin.image_crc, wire, sizeof(wire)), now);
    } else if (op == kOpDeployCommit) {
        logf("deploy: peer 0x%04x committed", peer);
        ++push_.ok;
        ++push_.idx;
        push_next_peer(now);
    }
    return true;
}

// ---- the server, called from Node::on_rx ---------------------------------------------------------------------

size_t PackageAgent::serve(uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len,
                           uint8_t* reply, size_t cap, uint32_t now) {
    DeployReply r{};
    r.status = DeployStatus::Malformed;
    const bool ready = hooks_.ready == nullptr || hooks_.ready(hooks_.ctx);
    const bool busy = hooks_.busy != nullptr && hooks_.busy(hooks_.ctx);
    if (!ready) {
        r.status = DeployStatus::StoreFailed;
    } else if (push_.active || reboot_at_ms_ != 0 || busy) {
        r.status = DeployStatus::Busy;
    } else if (op == kOpDeployBegin) {
        DeployBegin b{};
        if (load_deploy_begin(p, len, b)) {
            r = rx_.begin(b);
            rx_last_ms_ = now;
        }
    } else if (op == kOpDeployChunk) {
        DeployChunk c{};
        if (load_deploy_chunk(p, len, c)) {
            r = rx_.chunk(c);
            rx_last_ms_ = now;
        }
    } else if (op == kOpDeployCommit) {
        uint32_t crc = 0;
        if (load_deploy_commit(p, len, crc)) {
            // One counter, two packages: the higher digest wins (see the header). Judged on the image's own header,
            // which the signature covers, before anything is written.
            if (rx_.in_progress() && rx_.image_len() >= kImageHeaderLen &&
                rx_.header().rollback_counter == state_.counter && lower_at_floor(rx_.image() + 12)) {
                logf("deploy: counter %u with a lower digest than the package running at it - refused",
                     static_cast<unsigned>(state_.counter));
                ++stats_.refused_lower;
                rx_.abort();
                r = DeployReply{};
                r.status = DeployStatus::Downgrade;
                r.counter = state_.counter;
                return store_deploy_reply(r, reply, cap);
            }
            r = rx_.commit(crc);
        }
        if (r.status == DeployStatus::Ok) {
            store_.load_state(state_);
            // Taken in place when it can be; else on trial from the next boot.
            DeployStatus refused = DeployStatus::Ok;
            live_ = hooks_.committed != nullptr && hooks_.committed(hooks_.ctx, &refused);
            if (refused != DeployStatus::Ok) {
                r.status = refused;
                r.pending = 0;
                return store_deploy_reply(r, reply, cap);
            }
            logf("deploy: committed counter %u, on trial %s", static_cast<unsigned>(rx_.header().rollback_counter),
                 live_ ? "now, without a reboot" : "from the next boot");
            if (rx_.distribute()) {
                push_ = Push{};
                push_.begin = rx_.header();
                push_.begin.flags &= static_cast<uint8_t>(~kDeployFlagDistribute);  // one hop, no echo
                push_.requester = from;
                push_.requester_msg = msg_id;
                for (size_t i = 0; i < PeerTable::capacity() && push_.count < kMaxPeers; ++i) {
                    const PeerLink& q = node_.peers().slot(i);
                    if (q.state == PeerState::Alive && q.node_id != from) push_.peers[push_.count++] = q.node_id;
                }
                push_.active = true;
                logf("deploy: passing it on to %u peer(s)", static_cast<unsigned>(push_.count));
                push_next_peer(now);
                return 0;  // answered by push_finish(), once the peers have committed
            }
            if (!live_) schedule_reboot(now);
        }
    } else if (op == kOpDeployAbort) {
        rx_.abort();
        r.status = DeployStatus::Ok;
    }
    return store_deploy_reply(r, reply, cap);
}

// ---- catch-up ---------------------------------------------------------------------------------------------------

void PackageAgent::soon(uint32_t now) {
    const uint32_t t = now + kPkgSoonMs;
    if (static_cast<int32_t>(next_announce_ms_ - t) > 0) next_announce_ms_ = t;
}

void PackageAgent::announce(uint32_t now) {
    if (static_cast<int32_t>(now - next_announce_ms_) < 0) return;
    next_announce_ms_ = now + kPkgAnnounceMs;
    if (state_.active == kSlotNone || state_.pending != 0 || running_counter_ == 0) return;
    uint8_t args[kPkgAnnounceLen] = {kPkgAnnounce};
    std::memcpy(args + 1, &running_counter_, 4);
    std::memcpy(args + 5, running_digest_, 8);
    for (size_t i = 0; i < PeerTable::capacity(); ++i) {
        const PeerLink& q = node_.peers().slot(i);
        if (q.state == PeerState::Alive && std::memcmp(q.mac, host_mac_, kMacLen) != 0) {
            node_.cast(q.node_id, kPkgPath, args, sizeof(args));
        }
    }
    ++stats_.announces;
}

void PackageAgent::catchup_start(uint16_t peer, uint32_t now) {
    size_t len = 0;
    if (!store_.read_slot(state_.active, buf_, cap_, len)) return;
    static DeployImage img;  // off the link task's stack; validated, nothing kept
    const size_t extent = image_extent(buf_, len);
    if (extent == 0 || extent > len || !parse_image(buf_, extent, img, nullptr, kKeepNone)) return;
    push_ = Push{};
    push_.catchup = true;
    push_.begin.image_len = static_cast<uint32_t>(len);
    push_.begin.image_crc = crc32(buf_, len);
    push_.begin.rollback_counter = img.rollback_counter;
    std::memcpy(push_.begin.package_digest, img.package_digest, sizeof(img.package_digest));
    push_.begin.flags = 0;  // one hop
    push_.peers[push_.count++] = peer;
    push_.active = true;
    ++stats_.served;
    logf("deploy: peer 0x%04x was off for counter %u - sending it (%u B)", peer,
         static_cast<unsigned>(img.rollback_counter), static_cast<unsigned>(len));
    push_next_peer(now);
}

bool PackageAgent::on_cast(uint16_t from, uint32_t path, const uint8_t* args, uint16_t len, uint32_t now) {
    if (path != kPkgPath) return false;
    if (args == nullptr || len < 5) return true;
    uint32_t counter = 0;
    std::memcpy(&counter, args + 1, 4);
    if (args[0] == kPkgAnnounce) {
        // Newer, or the same counter as this board's floor with a higher digest (a board before 2026-10-14 sends no
        // digest, and is never asked for one).
        const bool newer = counter > state_.counter;
        const bool tie = !newer && len >= kPkgAnnounceLen && counter == state_.counter && running_counter_ == counter &&
                         std::memcmp(args + 5, running_digest_, 8) > 0;
        if ((!newer && !tie) || state_.pending != 0 || !idle()) return true;
        if (asked_ever_ && now - last_ask_ms_ < kPkgAskCooldownMs) return true;
        last_ask_ms_ = now;
        asked_ever_ = true;
        uint8_t ask[5] = {kPkgAsk};
        std::memcpy(ask + 1, &counter, 4);
        if (node_.cast(from, kPkgPath, ask, sizeof(ask))) {
            ++stats_.asked;
            if (tie) ++stats_.asked_tie;
            logf("deploy: peer 0x%04x runs counter %u%s, %s %u here - asking for it", from,
                 static_cast<unsigned>(counter), tie ? " with a higher digest" : "", tie ? "the same as" : "newer than",
                 static_cast<unsigned>(state_.counter));
        }
    } else if (args[0] == kPkgAsk) {
        if (counter != running_counter_ || state_.pending != 0) return true;
        if (idle()) {
            catchup_start(from, now);
            return true;
        }
        for (size_t i = 0; i < asks_n_; ++i) {
            if (asks_[i].peer == from) {
                asks_[i] = Ask{from, counter, now};
                return true;
            }
        }
        if (asks_n_ < kMaxPeers) {
            asks_[asks_n_++] = Ask{from, counter, now};
            ++stats_.queued;
        }
    }
    return true;
}

void PackageAgent::tick(uint32_t now) {
    if (rx_.in_progress() && !push_.active && now - rx_last_ms_ >= kRxStaleMs) {
        logf("deploy: a receive of counter %u stopped %u ms ago at %u of %u B - abandoned",
             static_cast<unsigned>(rx_.header().rollback_counter), static_cast<unsigned>(now - rx_last_ms_),
             static_cast<unsigned>(rx_.image_len()), static_cast<unsigned>(rx_.header().image_len));
        rx_.abort();
        ++stats_.rx_abandoned;
    }
    // Asks kept while busy: the oldest still current, once free. One that has outlived the asker's cooldown is dropped
    // (it has asked again by then, maybe someone else), and so is one for a package this board no longer runs.
    while (asks_n_ > 0 && idle() && state_.pending == 0) {
        const Ask a = asks_[0];
        for (size_t i = 1; i < asks_n_; ++i) asks_[i - 1] = asks_[i];
        --asks_n_;
        if (a.counter == running_counter_ && now - a.at_ms < kPkgAskCooldownMs) {
            catchup_start(a.peer, now);
            break;
        }
    }
    announce(now);
}

}  // namespace pot
