// M14: the cell's leases. See pot/lease.hpp.

#include "pot/lease.hpp"

#include <cstring>

#include "pot/link_stats.hpp"
#include "pot/membership.hpp"

namespace pot {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool printable_name(const char* s, size_t n) {
    if (n == 0 || n > kLeaseNameMax) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

}  // namespace

const char* lease_status_str(LeaseStatus s) {
    switch (s) {
        case LeaseStatus::Ok: return "ok";
        case LeaseStatus::Held: return "held";
        case LeaseStatus::Malformed: return "malformed";
        case LeaseStatus::Unreachable: return "unreachable";
    }
    return "?";
}

const char* lease_kind_str(uint8_t kind) {
    return kind == kLeaseDeploy ? "deploy" : kind == kLeaseHub ? "hub" : "?";
}

bool load_lease_request(const uint8_t* p, size_t len, LeaseRequest& out) {
    if (p == nullptr || len < 11) return false;
    out = LeaseRequest{};
    out.target = rd16(p);
    out.kind = p[2];
    out.op = p[3];
    out.ttl_s = rd16(p + 4);
    out.token = rd32(p + 6);
    const size_t n = p[10];
    if (n > kLeaseNameMax || len - 11 < n) {
        out.name_len = 0xFF;  // judged malformed by handle()
        return true;
    }
    out.name_len = static_cast<uint8_t>(n);
    std::memcpy(out.name, p + 11, n);
    out.name[n] = '\0';
    return true;
}

size_t store_lease_request(const LeaseRequest& r, uint8_t* out, size_t cap) {
    const size_t n = r.name_len <= kLeaseNameMax ? r.name_len : 0;
    if (out == nullptr || cap < 11 + n) return 0;
    wr16(out, r.target);
    out[2] = r.kind;
    out[3] = r.op;
    wr16(out + 4, r.ttl_s);
    wr32(out + 6, r.token);
    out[10] = static_cast<uint8_t>(n);
    std::memcpy(out + 11, r.name, n);
    return 11 + n;
}

bool load_lease_reply(const uint8_t* p, size_t len, LeaseReply& out) {
    if (p == nullptr || len < 13) return false;
    out = LeaseReply{};
    out.status = static_cast<LeaseStatus>(p[0]);
    out.kind = p[1];
    out.arbiter = rd16(p + 2);
    out.remaining_ms = rd32(p + 4);
    out.token = rd32(p + 8);
    const size_t n = p[12];
    if (n > kLeaseNameMax || len - 13 < n) return false;
    out.name_len = static_cast<uint8_t>(n);
    std::memcpy(out.name, p + 13, n);
    out.name[n] = '\0';
    return true;
}

size_t store_lease_reply(const LeaseReply& r, uint8_t* out, size_t cap) {
    const size_t n = r.name_len <= kLeaseNameMax ? r.name_len : 0;
    if (out == nullptr || cap < 13 + n) return 0;
    out[0] = static_cast<uint8_t>(r.status);
    out[1] = r.kind;
    wr16(out + 2, r.arbiter);
    wr32(out + 4, r.remaining_ms);
    wr32(out + 8, r.token);
    out[12] = static_cast<uint8_t>(n);
    std::memcpy(out + 13, r.name, n);
    return 13 + n;
}

LeaseReply LeaseTable::handle(const LeaseRequest& req, uint32_t now_ms, uint16_t self) {
    LeaseReply rep;
    rep.kind = req.kind;
    rep.arbiter = self;
    if (req.kind == 0 || req.kind > kLeaseKinds || req.token == 0 ||
        (req.op != kLeaseTake && req.op != kLeaseRelease && req.op != kLeaseQuery) ||
        (req.op == kLeaseTake && (req.ttl_s == 0 || req.ttl_s > kLeaseMaxTtlS || !printable_name(req.name, req.name_len)))) {
        rep.status = LeaseStatus::Malformed;
        return rep;
    }
    Entry& e = e_[req.kind - 1];
    if (!live(e, now_ms)) e = Entry{};
    const bool mine = e.token == req.token;
    if (req.op == kLeaseTake && (e.token == 0 || mine)) {
        e.token = req.token;
        e.expires_ms = now_ms + static_cast<uint32_t>(req.ttl_s) * 1000u;
        e.name_len = req.name_len;
        std::memcpy(e.name, req.name, req.name_len);
        e.name[req.name_len] = '\0';
        rep.status = LeaseStatus::Ok;
    } else if (req.op == kLeaseRelease && (e.token == 0 || mine)) {
        e = Entry{};
        rep.status = LeaseStatus::Ok;
    } else if (req.op == kLeaseQuery) {
        rep.status = LeaseStatus::Ok;
    } else {
        rep.status = LeaseStatus::Held;
    }
    if (e.token != 0) {
        rep.token = e.token;
        rep.remaining_ms = e.expires_ms - now_ms;
        rep.name_len = e.name_len;
        std::memcpy(rep.name, e.name, e.name_len + 1);
    }
    return rep;
}

const char* LeaseTable::holder(uint8_t kind, uint32_t now_ms) const {
    if (kind == 0 || kind > kLeaseKinds || !live(e_[kind - 1], now_ms)) return "";
    return e_[kind - 1].name;
}

uint16_t lease_arbiter(uint16_t self, const uint16_t* alive, size_t n) {
    uint16_t best = self >= kLeaseMinBoardId ? self : 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        if (alive[i] >= kLeaseMinBoardId && alive[i] < best) best = alive[i];
    }
    return best;
}

uint16_t lease_arbiter_of(const PeerTable& peers, uint16_t self, const uint8_t* host_mac) {
    uint16_t alive[kMaxPeers];
    size_t n = 0;
    for (size_t i = 0; i < PeerTable::capacity() && n < kMaxPeers; ++i) {
        const PeerLink& q = peers.slot(i);
        if (q.state == PeerState::Alive && (host_mac == nullptr || std::memcmp(q.mac, host_mac, kMacLen) != 0)) {
            alive[n++] = q.node_id;
        }
    }
    return lease_arbiter(self, alive, n);
}

}  // namespace pot
