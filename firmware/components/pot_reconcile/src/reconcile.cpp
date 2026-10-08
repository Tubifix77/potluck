// The reconciler -- section 7.7. The rules are in reconcile.hpp; this file only carries them out.

#include "pot/reconcile.hpp"

#include "pot/ticker.hpp"

#include <cstring>

namespace pot {

namespace {

uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool better(uint32_t term_a, uint16_t node_a, uint32_t term_b, uint16_t node_b) {
    return term_a > term_b || (term_a == term_b && node_a > node_b);
}

}  // namespace

uint32_t hrw_weight(uint32_t key, uint16_t node) { return fmix32(fmix32(key) ^ node); }

size_t rank_nodes(const TickerConfig& cfg, LiveFn live, void* ctx, uint16_t out[kMaxEligible]) {
    size_t n = 0;
    uint8_t grav[kMaxEligible];
    uint32_t w[kMaxEligible];
    for (uint8_t i = 0; i < cfg.count && i < kMaxEligible; ++i) {
        if (!live(ctx, cfg.node[i])) {
            continue;
        }
        // Insertion sort: at most eight entries.
        const uint16_t id = cfg.node[i];
        const uint8_t g = cfg.gravity[i];
        const uint32_t wt = hrw_weight(cfg.out_hash, id);
        size_t at = n;
        while (at > 0) {
            const size_t j = at - 1;
            const bool ahead = g > grav[j] || (g == grav[j] && (wt > w[j] || (wt == w[j] && id > out[j])));
            if (!ahead) {
                break;
            }
            out[at] = out[j];
            grav[at] = grav[j];
            w[at] = w[j];
            at = j;
        }
        out[at] = id;
        grav[at] = g;
        w[at] = wt;
        ++n;
    }
    return n;
}

Reconciler::Reconciler(Node& node, const ReconcileConfig& cfg) : node_(node), cfg_(cfg) {}

bool Reconciler::load(const PortableSpec* actors, size_t n) {
    if (n > kMaxPortable) {
        return false;
    }
    count_ = 0;
    for (size_t i = 0; i < n; ++i) {
        Slot& s = slots_[count_];
        s = Slot{};
        s.cfg = actors[i].place;
        s.kind = actors[i].kind;
        if (s.kind == nullptr || actors[i].decl.cfg_len > kMaxActorCfg) {
            return false;
        }
        std::memcpy(s.decl_cfg, actors[i].cfg, actors[i].decl.cfg_len);
        s.decl = actors[i].decl;
        s.decl.cfg = s.decl_cfg;
        NsDecl outs[kMaxOutputs];
        size_t n_out = (s.kind->outputs != nullptr) ? s.kind->outputs(s.decl, 0, 0, outs, kMaxOutputs) : 0;
        if (n_out == 0) {
            // The one output at the header's out_hash, as since M6.
            NsDecl& d = outs[0];
            d = NsDecl{};
            d.path_hash = s.cfg.out_hash;
            d.type = ValueType::None;  // whatever the actor publishes (M8.1: any portable type)
            d.unit = Unit::None;
            d.kind = ResourceKind::Sampled;
            d.access = Access::Read;
            d.latency_class = kClassL3;  // section 7.7: portable actors are L3/L4 by construction
            // Five periods: one lost publication is not staleness, a stopped instance soon is.
            d.staleness_bound_ms = 5u * s.cfg.period_ms;
            d.staleness_policy = StalenessPolicy::Informative;
            n_out = 1;
        }
        s.n_out = 0;
        for (size_t j = 0; j < n_out && j < kMaxOutputs; ++j) {
            outs[j].owner_node = 0;  // nobody, until a claim says otherwise
            if (node_.ns().declare(outs[j]) != NsError::Ok) {
                return false;
            }
            s.out_hash[s.n_out++] = outs[j].path_hash;
        }
        ++count_;
    }
    return true;
}

bool Reconciler::load(const TickerConfig* actors, size_t n) {
    if (n > kMaxPortable) {
        return false;
    }
    PortableSpec specs[kMaxPortable];
    for (size_t i = 0; i < n; ++i) {
        specs[i].place = actors[i];
        specs[i].kind = &kTickerKind;
        const size_t len = encode_ticker_config(actors[i], specs[i].cfg, sizeof(specs[i].cfg));
        if (len == 0) return false;
        specs[i].decl = ActorDecl{kPortableNode, ActorType::Ticker, static_cast<uint8_t>(len), specs[i].cfg};
    }
    return load(specs, n);
}

Reconciler::~Reconciler() {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].inst != nullptr) slots_[i].inst->~Actor();
    }
}

bool collect_portable(const DeployImage& img, const ActorKind* table, size_t n_kinds, PortableSpec* out,
                      size_t cap, size_t& count, const char** why) {
    count = 0;
    for (uint8_t i = 0; i < img.actor_count; ++i) {
        const ActorDecl& a = img.actors[i];
        if (a.node_id != kPortableNode) continue;
        const ActorKind* k = find_actor_kind(table, n_kinds, a.type);
        const char* w = nullptr;
        if (k == nullptr || k->placement == nullptr) {
            w = "portable actor of a type this build cannot place";
        } else if (count >= cap) {
            w = "more portable actors than kMaxPortable";
        } else if (a.cfg_len > kMaxActorCfg) {
            w = "actor config longer than kMaxActorCfg";
        } else {
            ActorEnv env;
            PortableSpec& p = out[count];
            if (!k->check(a, env, &w) || !k->placement(a, p.place)) {
                if (w == nullptr) w = "portable actor config invalid";
            } else {
                p.kind = k;
                std::memcpy(p.cfg, a.cfg, a.cfg_len);
                p.decl = a;
                p.decl.cfg = p.cfg;
                ++count;
            }
        }
        if (w != nullptr) {
            count = 0;
            if (why != nullptr) *why = w;
            return false;
        }
    }
    return true;
}

void Reconciler::start(uint32_t now_ms) {
    started_ = true;
    started_ms_ = now_ms;
    settled_ = false;
    next_refresh_ms_ = now_ms + cfg_.refresh_ms;
}

bool Reconciler::live_cb(void* ctx, uint16_t node) {
    const Reconciler* r = static_cast<const Reconciler*>(ctx);
    return node == r->node_.config().node_id || r->peer_live(node);
}

bool Reconciler::peer_live(uint16_t node, uint32_t* epoch) const {
    const PeerLink* p = node_.peers().find_by_node_id(node);
    if (p == nullptr || p->state != PeerState::Alive) {
        return false;
    }
    if (epoch != nullptr) {
        *epoch = p->boot_epoch;
    }
    return true;
}

const Reconciler::PeerClaims* Reconciler::claims_of(uint16_t node) const {
    uint32_t epoch = 0;
    if (!peer_live(node, &epoch)) {
        return nullptr;  // a claim lives exactly as long as its sender's membership
    }
    for (const PeerClaims& c : peers_) {
        if (c.node_id == node) {
            return c.epoch == epoch ? &c : nullptr;
        }
    }
    return nullptr;
}

Reconciler::PeerClaims* Reconciler::claims_slot(uint16_t node, bool create) {
    PeerClaims* free_slot = nullptr;
    for (PeerClaims& c : peers_) {
        if (c.node_id == node) {
            return &c;
        }
        if (c.node_id == 0 && free_slot == nullptr) {
            free_slot = &c;
        }
    }
    if (!create) {
        return nullptr;
    }
    if (free_slot == nullptr) {
        // Full: reuse the slot of a node that is no longer alive. There always is one, because the
        // table is as large as the peer table and this sender is alive.
        for (PeerClaims& c : peers_) {
            if (!peer_live(c.node_id)) {
                free_slot = &c;
                break;
            }
        }
    }
    if (free_slot != nullptr) {
        *free_slot = PeerClaims{};
        free_slot->node_id = node;
    }
    return free_slot;
}

bool Reconciler::on_cast(uint16_t from_node, uint32_t path_hash, const uint8_t* args, uint16_t len) {
    if (path_hash != kReconcilePath) {
        return false;
    }
    if (len < kClaimHeaderLen || args[0] != kClaimVersion || args[2] > kMaxPortable ||
        len != kClaimHeaderLen + static_cast<size_t>(args[2]) * kClaimEntryLen) {
        ++counters_.sets_malformed;
        return true;
    }
    uint32_t epoch = 0;
    if (!peer_live(from_node, &epoch)) {
        // Authenticated, but from a node we do not count as alive yet. Its HELLO will admit it, and it
        // will send again when it sees us: our view-change push.
        ++counters_.sets_unknown_peer;
        return true;
    }
    PeerClaims* c = claims_slot(from_node, true);
    if (c == nullptr) {
        ++counters_.sets_unknown_peer;
        return true;
    }
    ++counters_.sets_received;
    c->epoch = epoch;
    c->settled = (args[1] & kClaimFlagSettled) != 0;
    std::memset(c->term, 0, sizeof(c->term));
    const uint8_t n = args[2];
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t* e = args + kClaimHeaderLen + k * kClaimEntryLen;
        const uint32_t key = rd32(e);
        const uint32_t term = rd32(e + 4);
        for (size_t i = 0; i < count_; ++i) {
            if (slots_[i].cfg.out_hash == key) {
                c->term[i] = term;
                if (term > slots_[i].max_term) {
                    slots_[i].max_term = term;
                }
            }
        }
        // A key we do not have is a peer on another image -- mid-deploy. Its claim cannot concern us.
    }
    return true;
}

// Compare membership with what we last saw. A peer that is new, or back in a new incarnation, gets
// our claim set at once: that is what lets it settle, and what makes its view of who runs what
// correct before it is allowed to start anything.
bool Reconciler::refresh_view(uint32_t now) {
    (void)now;
    Seen cur[kMaxPeers];
    size_t n = 0;
    const PeerTable& pt = node_.peers();
    for (size_t i = 0; i < PeerTable::capacity() && n < kMaxPeers; ++i) {
        const PeerLink& p = pt.slot(i);
        if (p.state == PeerState::Alive && p.node_id != 0) {
            cur[n++] = Seen{p.node_id, p.boot_epoch};
        }
    }
    // A claim dies with its sender's membership, and stays dead: a peer declared dead and then heard
    // again in the same incarnation (a partition healing) must not have its pre-partition claims
    // resurrected. They would be stale -- the other side may have started the actor since -- and a
    // consumer that took them would accept an older term than one it already accepted. It sends a
    // fresh set the moment it sees us again.
    for (PeerClaims& c : peers_) {
        if (c.node_id == 0) {
            continue;
        }
        uint32_t epoch = 0;
        if (!peer_live(c.node_id, &epoch) || epoch != c.epoch) {
            c = PeerClaims{};
        }
    }
    bool changed = n != seen_n_;
    for (size_t i = 0; i < n; ++i) {
        bool known = false;
        for (size_t j = 0; j < seen_n_; ++j) {
            if (seen_[j].node_id == cur[i].node_id && seen_[j].epoch == cur[i].epoch) {
                known = true;
                break;
            }
        }
        if (!known) {
            changed = true;
            send_to(cur[i].node_id);
        }
    }
    if (changed) {
        std::memcpy(seen_, cur, n * sizeof(Seen));
        seen_n_ = n;
        ++counters_.view_changes;
        for (size_t i = 0; i < count_; ++i) {
            slots_[i].released_to = 0;  // a new view may make a handover succeed that failed before
        }
    }
    return changed;
}

void Reconciler::update_settled(uint32_t now) {
    if (settled_) {
        return;
    }
    const uint32_t up = now - started_ms_;
    // Heard from every node the image says could be running something -- not every node we happen to
    // have admitted. The two differ exactly when it matters: a node that has just booted has admitted
    // nobody yet (on the boards a signed HELLO takes 2-3.5 s to arrive and verify, M0-LOG session 26),
    // and "heard from all of nobody" settled it alone, so it started an actor its peers were running.
    // The eligible set is desired state, frozen in the image, so it is known before anyone answers. A
    // node that never answers -- dead, or out of range -- is waited for until settle_max_ms. A node no
    // actor lists can hold no claim, and the host on the serial link never sends one: not waited for.
    const uint16_t self = node_.config().node_id;
    bool all_heard = up >= cfg_.settle_min_ms;
    for (size_t i = 0; all_heard && i < count_; ++i) {
        for (uint8_t k = 0; all_heard && k < slots_[i].cfg.count; ++k) {
            const uint16_t n = slots_[i].cfg.node[k];
            if (n != self && claims_of(n) == nullptr) {
                all_heard = false;
            }
        }
    }
    if (all_heard || up >= cfg_.settle_max_ms) {
        settled_ = true;
        dirty_ = true;  // tell everyone: a holder hands over only to a settled node
    }
}

void Reconciler::resolve_owner(size_t i) {
    Slot& s = slots_[i];
    const uint16_t self = node_.config().node_id;
    uint16_t owner = 0;
    uint32_t term = 0;
    if (s.running) {
        owner = self;
        term = s.term;
    }
    for (size_t j = 0; j < seen_n_; ++j) {
        const PeerClaims* c = claims_of(seen_[j].node_id);
        if (c != nullptr && c->term[i] != 0 && (owner == 0 || better(c->term[i], c->node_id, term, owner))) {
            owner = c->node_id;
            term = c->term[i];
        }
    }
    s.owner_term = term;
    if (s.owner != owner) {
        s.owner = owner;
        for (uint8_t j = 0; j < s.n_out; ++j) node_.ns().set_owner(s.out_hash[j], owner);  // all fenced together
        node_.record_event(EventKind::ActorOwner, owner, s.cfg.out_hash, term);
    }
}

void Reconciler::activate(size_t i, uint32_t now) {
    Slot& s = slots_[i];
    s.running = true;
    s.term = s.max_term + 1;
    s.max_term = s.term;
    s.unclaimed_since = 0;
    // M8.1: a fresh instance per activation -- section 7.7's "re-activation restarts from the actor's
    // declared initial state" by construction.
    ActorEnv env;
    env.node = &node_;
    s.inst = s.kind->create(s.mem, s.decl, env);
    if (s.inst != nullptr && !s.inst->start(now)) {
        s.inst->~Actor();
        s.inst = nullptr;
    }
    ++s.activations;
    dirty_ = true;
    node_.record_event(EventKind::ActorStarted, node_.config().node_id, s.cfg.out_hash, s.term);
}

void Reconciler::stop(size_t i, uint32_t why) {
    slots_[i].running = false;
    if (slots_[i].inst != nullptr) {
        slots_[i].inst->~Actor();
        slots_[i].inst = nullptr;
    }
    dirty_ = true;
    node_.record_event(EventKind::ActorStopped, node_.config().node_id, slots_[i].cfg.out_hash, why);
}

void Reconciler::decide(size_t i, uint32_t now) {
    Slot& s = slots_[i];
    const uint16_t self = node_.config().node_id;
    uint16_t ranked[kMaxEligible];
    const size_t n = rank_nodes(s.cfg, &Reconciler::live_cb, this, ranked);

    // The best live claim other than ours.
    uint16_t other = 0;
    uint32_t other_term = 0;
    for (size_t j = 0; j < seen_n_; ++j) {
        const PeerClaims* c = claims_of(seen_[j].node_id);
        if (c != nullptr && c->term[i] != 0 && (other == 0 || better(c->term[i], c->node_id, other_term, other))) {
            other = c->node_id;
            other_term = c->term[i];
        }
    }

    if (s.running) {
        if (other != 0 && better(other_term, other, s.term, self)) {
            stop(i, 1);
            ++s.fenced;
            return;
        }
        if (n > 0 && ranked[0] != self && s.released_to != ranked[0]) {
            // Only to a node that is not running it. One that is runs a losing instance -- a partition
            // that just healed -- and handing over now would make its older term the only claim left,
            // which every consumer would then have to accept. It hears our higher claim, stops, and the
            // handover follows; it starts again under a fresh term.
            const PeerClaims* c = claims_of(ranked[0]);
            if (c != nullptr && c->settled && c->term[i] == 0) {
                stop(i, 2);
                s.released_to = ranked[0];
                ++s.released;
            }
        }
        return;
    }

    if (other != 0) {
        s.unclaimed_since = 0;
        return;
    }
    if (s.unclaimed_since == 0) {
        s.unclaimed_since = now == 0 ? 1 : now;
    }
    size_t rank = n;
    for (size_t k = 0; k < n; ++k) {
        if (ranked[k] == self) {
            rank = k;
            break;
        }
    }
    if (rank == n) {
        return;  // not eligible here
    }
    if (rank == 0 || now - s.unclaimed_since >= static_cast<uint32_t>(rank) * cfg_.orphan_step_ms) {
        activate(i, now);
    }
}

void Reconciler::run(size_t i, uint32_t now) {
    Slot& s = slots_[i];
    // Only the fenced owner's instance runs: a node that started an actor and has not yet seen that
    // a higher claim exists must not publish over the winner (section 7.7).
    if (!s.running || s.inst == nullptr || s.owner != node_.config().node_id) {
        return;
    }
    s.inst->tick(now);
}

size_t Reconciler::encode(uint8_t* out, size_t cap) const {
    if (cap < kClaimMaxLen) {
        return 0;
    }
    out[0] = kClaimVersion;
    out[1] = settled_ ? kClaimFlagSettled : 0;
    uint8_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].running) {
            uint8_t* e = out + kClaimHeaderLen + n * kClaimEntryLen;
            wr32(e, slots_[i].cfg.out_hash);
            wr32(e + 4, slots_[i].term);
            ++n;
        }
    }
    out[2] = n;
    return kClaimHeaderLen + n * kClaimEntryLen;
}

void Reconciler::send_to(uint16_t node) {
    uint8_t buf[kClaimMaxLen];
    const size_t len = encode(buf, sizeof(buf));
    if (node_.cast(node, kReconcilePath, buf, static_cast<uint16_t>(len))) {
        ++counters_.sets_sent;
    } else {
        ++counters_.send_failures;
    }
}

void Reconciler::send_all() {
    for (size_t j = 0; j < seen_n_; ++j) {
        send_to(seen_[j].node_id);
    }
}

void Reconciler::tick(uint32_t now) {
    if (!started_ || count_ == 0) {
        return;
    }
    if (refresh_view(now)) {
        // Peers that died took their claims with them; the owners must be recomputed before deciding.
    }
    update_settled(now);
    for (size_t i = 0; i < count_; ++i) {
        resolve_owner(i);
        if (settled_) {
            decide(i, now);
            resolve_owner(i);
        }
        run(i, now);
    }
    if (dirty_) {
        dirty_ = false;
        send_all();
        next_refresh_ms_ = now + cfg_.refresh_ms;
    } else if (static_cast<int32_t>(now - next_refresh_ms_) >= 0) {
        next_refresh_ms_ = now + cfg_.refresh_ms;
        if (seen_n_ > 0) {
            refresh_cursor_ = (refresh_cursor_ + 1) % seen_n_;
            send_to(seen_[refresh_cursor_].node_id);
        }
    }
}

Reconciler::ActorView Reconciler::view(size_t i) const {
    ActorView v{};
    if (i >= count_) {
        return v;
    }
    const Slot& s = slots_[i];
    v.key = s.cfg.out_hash;
    v.running = s.running;
    v.term = s.running ? s.term : 0;
    v.owner = s.owner;
    v.owner_term = s.owner_term;
    v.max_term = s.max_term;
    uint16_t ranked[kMaxEligible];
    const size_t n = rank_nodes(s.cfg, &Reconciler::live_cb, const_cast<Reconciler*>(this), ranked);
    v.assigned = n > 0 ? ranked[0] : 0;
    v.activations = s.activations;
    v.fenced = s.fenced;
    v.released = s.released;
    return v;
}

}  // namespace pot
