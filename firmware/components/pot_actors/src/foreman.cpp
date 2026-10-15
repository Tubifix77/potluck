#include "pot/foreman.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace pot {

namespace {
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
// The length of the export record at p (len bytes left), 0 if it is malformed.
size_t export_len(const uint8_t* p, size_t len) {
    if (len < 7) return 0;
    const uint8_t kind = p[4];
    if (kind == static_cast<uint8_t>(ExportKind::Alive)) return 7;
    if (kind == static_cast<uint8_t>(ExportKind::Copy)) return len >= 12 ? 12 : 0;
    if (kind > static_cast<uint8_t>(ExportKind::Avg) || len < 8) return 0;
    const size_t sl = p[7];
    if (sl == 0 || sl > kForemanMaxSuffix || len < 8 + sl) return 0;
    for (size_t k = 0; k < sl; ++k) {
        const uint8_t ch = p[8 + k];
        if (ch < 0x21 || ch > 0x7E) return 0;
    }
    return 8 + sl;
}
}  // namespace

bool foreman_config(const ActorDecl& a, ForemanConfig& out) {
    size_t hl = 0;
    if (a.type != ActorType::Foreman || a.node_id != kPortableNode || !portable_header(a, out.place, hl)) return false;
    const uint8_t* p = a.cfg + hl;
    size_t left = a.cfg_len - hl;
    if (left < 8) return false;
    out.cell = p[0];
    out.parent = p[1];
    out.parent_window = p[2];
    out.n_members = p[3];
    out.prefix = rd32(p + 4);
    p += 8;
    left -= 8;
    if (out.cell == 0 || out.parent == 0 || out.cell == out.parent || out.n_members == 0 ||
        out.n_members > kForemanMaxMembers || left < 2u * out.n_members + 1) {
        return false;
    }
    for (uint8_t i = 0; i < out.n_members; ++i) out.members[i] = rd16(p + 2 * i);
    p += 2 * out.n_members;
    left -= 2 * out.n_members;
    out.n_exports = *p++;
    --left;
    out.exports = p;
    if (out.n_exports > kForemanMaxExports || out.n_exports > kMaxOutputs) return false;
    for (uint8_t i = 0; i < out.n_exports; ++i) {
        const size_t n = export_len(p, left);
        if (n == 0 || rd32(p) == 0) return false;
        p += n;
        left -= n;
    }
    // Every board that may hold the role is one of the cell's.
    for (uint8_t k = 0; k < out.place.count; ++k)
        if (!out.lists(out.place.node[k])) return false;
    return left == 0;
}

ForemanExport ForemanConfig::export_at(size_t i) const {
    const uint8_t* p = exports;
    for (size_t k = 0; k < i; ++k) p += export_len(p, 0xFFFF);
    ForemanExport e{};
    e.out_hash = rd32(p);
    e.kind = static_cast<ExportKind>(p[4]);
    e.unit = static_cast<Unit>(rd16(p + 5));
    if (e.kind == ExportKind::Copy) {
        e.type = static_cast<ValueType>(p[7]);
        e.src_hash = rd32(p + 8);
    } else if (e.kind != ExportKind::Alive) {
        e.suffix_len = p[7];
        e.suffix = p + 8;
    }
    return e;
}

uint32_t ForemanConfig::input_hash(const ForemanExport& e, uint16_t member) const {
    char rest[5 + 1 + kForemanMaxSuffix + 1];
    const int n = std::snprintf(rest, sizeof rest, "%04x/", static_cast<unsigned>(member));
    std::memcpy(rest + n, e.suffix, e.suffix_len);
    rest[n + e.suffix_len] = '\0';
    return path_hash_from(prefix, rest);
}

namespace {
NsDecl export_decl(const ForemanConfig& c, const ForemanExport& e) {
    NsDecl d;
    d.path_hash = e.out_hash;
    d.owner_node = 0;  // the reconciler's: whoever holds the role
    d.type = e.kind == ExportKind::Copy ? e.type
             : e.kind == ExportKind::Alive || e.kind == ExportKind::Count ? ValueType::U32
                                                                           : ValueType::F32;
    d.unit = e.unit;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;  // Review of M19 #5: every binding across cells is best effort
    d.staleness_bound_ms = 3u * c.place.period_ms;
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}
}  // namespace

bool ForemanActor::start(uint32_t now_ms) {
    next_ms_ = now_ms;
    return true;
}

void ForemanActor::publish_export(const ForemanExport& e, uint32_t now) {
    const uint16_t me = node_.config().node_id;
    if (e.kind == ExportKind::Alive) {
        node_.publish(e.out_hash, Value::of_u32(alive_));
        return;
    }
    // The inputs: one resource (copy), or each board of the cell's node-<id>/<suffix>. A board that is dead is no
    // input; a remote input is read from the replica here, and asked for again for the next round.
    float acc = 0;
    uint32_t used = 0, oldest = 0;
    bool faulty = false;
    Value copied{};
    const uint8_t n = e.kind == ExportKind::Copy ? 1 : cfg_.n_members;
    for (uint8_t i = 0; i < n; ++i) {
        const uint16_t m = e.kind == ExportKind::Copy ? 0 : cfg_.members[i];
        if (m != 0 && m != me) {
            const PeerLink* p = node_.peers().find_by_node_id(m);
            if (p == nullptr || p->state != PeerState::Alive) continue;
        }
        const uint32_t h = e.kind == ExportKind::Copy ? e.src_hash : cfg_.input_hash(e, m);
        Reading r;
        bool local = true;
        if (node_.read(h, r, &local) != NsError::Ok) continue;
        if (!local) {
            const NsEntry* en = node_.ns().find(h);
            if (en != nullptr && en->owner_node != 0 && en->owner_node != me) node_.request_read(en->owner_node, h);
        }
        if (r.quality == Quality::Faulty) {
            faulty = true;
            continue;
        }
        if (!r.usable()) continue;
        float v = 0;
        if (e.kind == ExportKind::Copy) {
            copied = r.value;
        } else if (e.kind != ExportKind::Count) {
            Quality q;
            if (!r.get_f32(v, q)) {
                int32_t iv = 0;
                if (!r.get_i32(iv, q)) continue;
                v = static_cast<float>(iv);
            }
        }
        if (e.kind == ExportKind::Max) acc = used == 0 || v > acc ? v : acc;
        else if (e.kind == ExportKind::Min) acc = used == 0 || v < acc ? v : acc;
        else acc += v;
        oldest = r.age_ms > oldest ? r.age_ms : oldest;
        ++used;
    }
    if (faulty) {
        node_.publish_faulty(e.out_hash);  // the worst input's quality
        return;
    }
    if (e.kind == ExportKind::Count) {
        node_.ns().publish(e.out_hash, Value::of_u32(used), now - oldest);
        return;
    }
    if (used == 0) return;  // nothing to say: the entry ages, and its readers see it
    const Value v = e.kind == ExportKind::Copy ? copied
                    : e.kind == ExportKind::Avg ? Value::of_f32(acc / static_cast<float>(used))
                                                : Value::of_f32(acc);
    node_.ns().publish(e.out_hash, v, now - oldest);  // the oldest input's age (Review of M19 #8)
}

void ForemanActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + cfg_.place.period_ms;
    const uint16_t me = node_.config().node_id;
    uint8_t alive = 0;
    for (uint8_t i = 0; i < cfg_.n_members; ++i) {
        const uint16_t m = cfg_.members[i];
        const PeerLink* p = m == me ? nullptr : node_.peers().find_by_node_id(m);
        if (m == me || (p != nullptr && p->state == PeerState::Alive)) ++alive;
    }
    alive_ = alive;
    for (uint8_t i = 0; i < cfg_.n_exports; ++i) publish_export(cfg_.export_at(i), now);
    ++rounds_;
}

size_t ForemanActor::stats_json(char* buf, size_t cap, uint32_t now_ms) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"foreman\",\"node\":%u,\"up_ms\":%u,\"cell\":%u,\"parent\":%u,\"alive\":%u,"
                                "\"of\":%u,\"exports\":%u,\"rounds\":%u,\"groups\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(now_ms),
                                static_cast<unsigned>(cfg_.cell), static_cast<unsigned>(cfg_.parent),
                                static_cast<unsigned>(alive_), static_cast<unsigned>(cfg_.n_members),
                                static_cast<unsigned>(cfg_.n_exports), static_cast<unsigned>(rounds_),
                                static_cast<unsigned>(node_.groups()));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {
bool foreman_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    ForemanConfig c{};
    if (!foreman_config(d, c)) {
        *why = "foreman config invalid";
        return false;
    }
    return true;
}
Actor* foreman_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(ForemanActor) <= kActorSlotBytes, "ForemanActor outgrew its actor slot");
    ForemanConfig c{};
    if (env.node == nullptr || !foreman_config(d, c)) return nullptr;
    return new (mem) ForemanActor(*env.node, c);
}
bool foreman_placement(const ActorDecl& d, TickerConfig& out) {
    ForemanConfig c{};
    if (!foreman_config(d, c)) return false;
    out = c.place;
    return true;
}
size_t foreman_outputs(const ActorDecl& d, uint16_t, uint16_t peer, NsDecl* out, size_t cap) {
    ForemanConfig c{};
    if (peer != 0 || !foreman_config(d, c) || cap < c.n_exports) return 0;
    for (uint8_t i = 0; i < c.n_exports; ++i) out[i] = export_decl(c, c.export_at(i));
    return c.n_exports;
}
// Review of M19 #4 and #6: the board that holds the role, and the standby, are members of the parent cell.
void foreman_membership(const ActorDecl& d, Node& node, bool member) {
    ForemanConfig c{};
    if (!foreman_config(d, c)) return;
    if (member) node.set_role_group(c.parent, c.parent_window * 100u);
    else if (node.role_group() == c.parent) node.set_role_group(0);
}
}  // namespace

const ActorKind kForemanKind = {ActorType::Foreman, "foreman", &foreman_check, &foreman_create, &foreman_placement,
                                &foreman_outputs, false, &foreman_membership};

}  // namespace pot
