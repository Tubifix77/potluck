#include "pot/deploy.hpp"

#include <cstring>

#if defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#include "pot/namespace.hpp"

namespace pot {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

// The common CRC-32 (reflected 0xEDB88320, init and final xor all-ones): the same value as
// Python's zlib.crc32, so the host computes it without a second implementation to keep in step.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t c = ~seed;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

size_t image_extent(const uint8_t* data, size_t len) {
    if (data == nullptr || len < kImageHeaderLen) return 0;
    const uint32_t body = rd32(data + 20);
    if (body > len - kImageHeaderLen) return 0;
    size_t at = kImageHeaderLen + body;
    const uint8_t v = data[4];
    if (v != kImageVersionGuests && v != kImageVersionOutside && v != kImageVersionBrokerCa && v != kImageVersionCells)
        return at;
    if (at >= len) return 0;  // the guest count
    const uint8_t n = data[at++];
    for (uint8_t g = 0; g < n; ++g) {
        if (len - at < 4) return 0;
        const uint32_t gl = rd32(data + at);
        at += 4;
        if (gl > len - at) return 0;
        at += gl;
    }
    if (v == kImageVersionGuests) return at;
    if (at >= len) return 0;  // the outside count
    const uint8_t no = data[at++];
    for (uint8_t i = 0; i < no; ++i) {
        if (len - at < 6) return 0;
        const size_t nl = data[at + 5];
        if (len - at - 6 < nl) return 0;
        at += 6 + nl;
    }
    if (v != kImageVersionBrokerCa && v != kImageVersionCells) return at;
    if (len - at < 2) return 0;  // the CA's length
    const uint16_t cl = rd16(data + at);
    at += 2;
    if (cl > len - at) return 0;
    at += cl;
    if (v != kImageVersionCells) return at;
    if (at >= len) return 0;  // M19: the scope section's count, then two bytes an actor
    const size_t sc = data[at];
    if (len - at - 1 < 2 * sc) return 0;
    return at + 1 + 2 * sc;
}

namespace {
// One declaration, judged by its type alone (the cross-checks against the image's sections are parse_image's).
bool check_decl(const ActorDecl& a, const char*& w) {
    switch (a.type) {
        case ActorType::Led:
            if (a.cfg_len != kLedCfgLen) {
                w = "led config has the wrong length";
                return false;
            }
            break;
        case ActorType::Fault:
            if (a.cfg_len != kFaultCfgLen) {
                w = "fault config has the wrong length";
                return false;
            }
            break;
        case ActorType::Ticker: {
            TickerConfig tc{};
            if (a.node_id != kPortableNode || !ticker_config(a, tc)) {
                w = "ticker must be portable, with 1..8 distinct eligible nodes";
                return false;
            }
            break;
        }
        case ActorType::SvcClient: {
            SvcClientConfig sc{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !svc_client_config(a, sc)) {
                w = "svc_client must be pinned, with a valid service, provider and period";
                return false;
            }
            break;
        }
        case ActorType::DieTemp: {
            DieTempConfig dc{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !die_temp_config(a, dc)) {
                w = "die_temp must be pinned, with an output and a period of 200..60000 ms";
                return false;
            }
            break;
        }
        case ActorType::McLender: {
            McLenderConfig mc{};
            if (a.node_id == kPortableNode || !mc_lender_config(a, mc)) {
                w = "mc_lender must be pinned or on every node, with 1..4 slots";
                return false;
            }
            break;
        }
        case ActorType::McJob: {
            McJobConfig mj{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !mc_job_config(a, mj)) {
                w = "mc_job must be pinned, with an output, 1..4096 units and 1000..100000000 samples";
                return false;
            }
            break;
        }
        case ActorType::FileHash: {
            FileHashConfig fh{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !file_hash_config(a, fh)) {
                w = "file_hash must be pinned, with a service, a provider, its resources and a 1..64-byte name";
                return false;
            }
            break;
        }
        case ActorType::KeyEcho: {
            KeyEchoConfig ke{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !key_echo_config(a, ke)) {
                w = "key_echo must be pinned, with its input and three distinct resources";
                return false;
            }
            break;
        }
        case ActorType::Speak: {
            SpeakConfig sk{};
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || !speak_config(a, sk)) {
                w = "speak must be pinned, with a service, a provider, its two resources and a 1..120-byte phrase";
                return false;
            }
            break;
        }
        case ActorType::Guest: {
            // M7: portable, then the index of its program in the guest section, checked below.
            TickerConfig pc{};
            size_t hl = 0;
            if (a.node_id != kPortableNode || !portable_header(a, pc, hl) || a.cfg_len != hl + 1) {
                w = "guest must be portable: the portable header, then a guest index";
                return false;
            }
            break;
        }
        case ActorType::Present: {
            PresentConfig pc{};
            if (a.node_id != kPortableNode || !present_config(a, pc)) {
                w = "present must be portable: the portable header, then broker address, port and keep-alive";
                return false;
            }
            break;
        }
        case ActorType::LedSwitch:
            // M13: pinned; pot/led_switch.hpp judges the rest (the row's check).
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || a.cfg_len != 8) {
                w = "led_switch must be pinned, with an output, a colour and a blink period";
                return false;
            }
            break;
        case ActorType::Position:
            // M20: pinned; pot/position.hpp judges the rest (the row's check).
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || a.cfg_len != 40) {
                w = "position must be pinned, with its outputs, x, y, a room and its nearest boards";
                return false;
            }
            break;
        case ActorType::Foreman: {
        // M19: portable; pot/foreman.hpp judges the rest (the row's check).
        TickerConfig pc{};
        size_t hl = 0;
        if (a.node_id != kPortableNode || !portable_header(a, pc, hl) || a.cfg_len < hl + 9) {
            w = "foreman must be portable: the portable header, then its cell, parent, boards and exports";
            return false;
        }
        break;
    }
    case ActorType::Inbox:
            // 2026-10-20: pinned; pot/inbox.hpp judges the rest (the row's check).
            if (a.node_id == kPortableNode || a.node_id == kEveryNode || a.cfg_len < 11 || (a.cfg_len - 5) % 6 != 0) {
                w = "inbox must be pinned, with a staleness bound and 1..12 outputs";
                return false;
            }
            break;
    case ActorType::Cluster:
            // M12: every node, a name of 1..31 characters; pot/cluster.hpp judges the characters.
            if (a.node_id != kEveryNode || a.cfg_len == 0 || a.cfg_len > 31) {
                w = "cluster must be on every node, with a name of 1..31 characters";
                return false;
            }
            break;
        default:
            if (is_external_type(a.type)) {
                break;  // M8.2: an external component's; its registration row judges the config
            }
            w = "unknown actor type";
            return false;
    }
    return true;
}
}  // namespace

DeployImage& validation_image() {
    // M19: 64 declarations make an image ~0.6 KB -- in PSRAM on a board that has it, never on a task's stack.
    EXT_RAM_BSS_ATTR static DeployImage img;
    return img;
}

bool parse_image(const uint8_t* data, size_t len, DeployImage& out, const char** why, uint16_t keep_for) {
    const char* dummy = nullptr;
    const char*& w = (why != nullptr) ? *why : dummy;
    if (data == nullptr || len < kImageHeaderLen) {
        w = "shorter than the image header";
        return false;
    }
    if (len > kMaxImageLen) {
        w = "larger than kMaxImageLen";
        return false;
    }
    if (rd32(data) != kImageMagic) {
        w = "bad magic";
        return false;
    }
    if (data[4] != kImageVersion && data[4] != kImageVersionGuests && data[4] != kImageVersionOutside &&
        data[4] != kImageVersionBrokerCa && data[4] != kImageVersionCells) {
        w = "unknown image version";
        return false;
    }
    const bool v5 = data[4] == kImageVersionCells;
    const bool v4 = data[4] == kImageVersionBrokerCa || v5;
    const bool v3 = data[4] == kImageVersionOutside || v4;
    const bool v2 = data[4] == kImageVersionGuests || v3;
    const uint8_t count = data[5];
    if (!v5 && count > kMaxActors) {  // M19: a tree's image lists up to 255; each board keeps its cells' (below)
        w = "more actors than kMaxActors";
        return false;
    }
    const uint32_t body_len = rd32(data + 20);
    if (v2 ? body_len > len - kImageHeaderLen : body_len != len - kImageHeaderLen) {
        w = "body_len disagrees with the image length";
        return false;
    }
    out.guest_count = 0;
    out.outside_count = 0;
    out.outside = nullptr;
    out.broker_ca = nullptr;
    out.broker_ca_len = 0;
    out.scoped = false;
    out.rollback_counter = rd32(data + 8);
    std::memcpy(out.package_digest, data + 12, sizeof(out.package_digest));
    const uint8_t* scope = nullptr;  // M19: version 5's scope section
    out.actor_count = 0;

    size_t at = kImageHeaderLen;
    for (uint8_t i = 0; i < count; ++i) {
        if (len - at < 4) {
            w = "actor declaration truncated";
            return false;
        }
        ActorDecl a{};
        a.node_id = rd16(data + at);
        a.type = static_cast<ActorType>(data[at + 2]);
        a.cfg_len = data[at + 3];
        at += 4;
        if (len - at < a.cfg_len) {
            w = "actor config truncated";
            return false;
        }
        a.cfg = data + at;
        at += a.cfg_len;
        if (at > kImageHeaderLen + body_len) {
            w = "actor declarations overrun body_len";
            return false;
        }
        if (!check_decl(a, w)) return false;
    }
    if (at != kImageHeaderLen + body_len) {
        w = "trailing bytes after the last actor";
        return false;
    }
    if (v2) {
        // M7: the guest section.
        if (len - at < 1) {
            w = "guest section missing";
            return false;
        }
        const uint8_t n = data[at++];
        if (n > kMaxGuests) {
            w = "more guests than kMaxGuests";
            return false;
        }
        for (uint8_t g = 0; g < n; ++g) {
            if (len - at < 4) {
                w = "guest length truncated";
                return false;
            }
            const uint32_t gl = rd32(data + at);
            at += 4;
            if (gl == 0 || len - at < gl) {
                w = "guest truncated";
                return false;
            }
            out.guests[g] = GuestBlob{data + at, gl};
            at += gl;
        }
        out.guest_count = n;
        if (v3) {
            // M13: the outside section. Every rule judged here, so a node never holds a package that lets the
            // outside write a safety-relevant resource, or names one resource twice.
            if (len - at < 1) {
                w = "outside section missing";
                return false;
            }
            const uint8_t no = data[at++];
            if (no > kMaxOutside) {
                w = "more outside rules than kMaxOutside";
                return false;
            }
            const size_t start = at;
            uint32_t seen[kMaxOutside];
            for (uint8_t i = 0; i < no; ++i) {
                if (len - at < 6 || len - at - 6 < data[at + 5]) {
                    w = "outside section truncated";
                    return false;
                }
                const uint32_t h = rd32(data + at);
                const uint8_t f = data[at + 4];
                const size_t nl = data[at + 5];
                if (h == 0 || !outside_flags_valid(f)) {
                    w = "outside rule invalid: a zero hash, an unknown flag, writable but not exposed, or writable and "
                        "safety-relevant";
                    return false;
                }
                if (nl == 0 || nl > kMaxOutsideName) {
                    w = "outside rule's name is not 1..40 characters";
                    return false;
                }
                for (size_t k = 0; k < nl; ++k) {
                    const uint8_t ch = data[at + 6 + k];
                    if (ch < 0x21 || ch > 0x7E || ch == '"' || ch == '\\') {
                        w = "outside rule's name has a character outside printable ASCII (or a quote or backslash)";
                        return false;
                    }
                }
                for (uint8_t k = 0; k < i; ++k) {
                    if (seen[k] == h) {
                        w = "outside section names one resource twice";
                        return false;
                    }
                }
                seen[i] = h;
                at += 6 + nl;
            }
            out.outside_count = no;
            out.outside = data + start;
        }
        if (v4) {
            // M13: the broker's CA. Its contents are judged where it is used, by the TLS library: a board that cannot
            // parse it fails to connect and says so, it does not refuse the package.
            if (len - at < 2) {
                w = "broker CA section missing";
                return false;
            }
            const uint16_t cl = rd16(data + at);
            at += 2;
            if ((cl == 0 && !v5) || cl > kMaxBrokerCa || len - at < cl) {
                w = "broker CA is not 1..1024 bytes, or truncated";
                return false;
            }
            out.broker_ca = cl != 0 ? data + at : nullptr;
            out.broker_ca_len = cl;
            at += cl;
        }
        if (v5) {
            // M19: the scope section -- each actor's cell and a second one, in declaration order.
            if (len - at < 1 || data[at] != count || len - at - 1 < 2u * count) {
                w = "scope section missing, truncated, or not one entry per actor";
                return false;
            }
            scope = data + at + 1;
            at += 1 + 2u * count;
        }
        if (at != len) {
            w = v5   ? "trailing bytes after the scope section"
                : v4 ? "trailing bytes after the broker CA"
                : v3 ? "trailing bytes after the outside section"
                     : "trailing bytes after the last guest";
            return false;
        }
    }
    // The node's cells (M19): those its own position actor names, and the parent of a cell whose foreman role it
    // may hold (a portable actor of its cell, with a second cell, that lists it as eligible).
    uint8_t mine[4] = {};
    size_t n_mine = 0;
    auto in_mine = [&](uint8_t c) {
        for (size_t k = 0; k < n_mine; ++k)
            if (mine[k] == c) return true;
        return false;
    };
    auto decl_at = [&](size_t& p, ActorDecl& a) {
        a.node_id = rd16(data + p);
        a.type = static_cast<ActorType>(data[p + 2]);
        a.cfg_len = data[p + 3];
        a.cfg = data + p + 4;
        p += 4 + a.cfg_len;
    };
    if (scope != nullptr && keep_for != kKeepAll && keep_for != kKeepNone) {
        size_t p = kImageHeaderLen;
        for (uint8_t i = 0; i < count; ++i) {
            ActorDecl a{};
            decl_at(p, a);
            if (a.type == ActorType::Position && a.node_id == keep_for && a.cfg_len >= 38) {
                for (size_t k = 36; k < 38; ++k)
                    if (a.cfg[k] != 0 && !in_mine(a.cfg[k])) mine[n_mine++] = a.cfg[k];
            }
        }
        p = kImageHeaderLen;
        for (uint8_t i = 0; i < count; ++i) {
            ActorDecl a{};
            decl_at(p, a);
            TickerConfig pc{};
            size_t hl = 0;
            const uint8_t cell = scope[2 * i], also = scope[2 * i + 1];
            if (a.node_id != kPortableNode || also == 0 || !in_mine(cell) || in_mine(also) || n_mine >= 4 ||
                !portable_header(a, pc, hl)) {
                continue;
            }
            for (uint8_t k = 0; k < pc.count; ++k)
                if (pc.node[k] == keep_for) {
                    mine[n_mine++] = also;
                    break;
                }
        }
    }
    size_t p = kImageHeaderLen;
    for (uint8_t i = 0; i < count; ++i) {
        ActorDecl a{};
        decl_at(p, a);
        if (a.type == ActorType::Guest && a.cfg[a.cfg_len - 1] >= out.guest_count) {
            w = "guest actor names a guest the image does not carry";
            return false;
        }
        PresentConfig pc{};
        if (a.type == ActorType::Present && present_config(a, pc) && (pc.flags & kPresentTls) != 0 &&
            (out.broker_ca_len == 0 || crc32(out.broker_ca, out.broker_ca_len) != pc.ca_crc)) {
            w = "present asks for TLS and the image carries no broker CA, or another one than it names";
            return false;
        }
        // M19 (Review of M19 #3): a board keeps what is its own, what is every cell's (cell 0), and what belongs to
        // one of its cells; a board in no cell, everything.
        bool keep = keep_for != kKeepNone;
        if (keep && scope != nullptr && keep_for != kKeepAll && n_mine != 0) {
            const uint8_t cell = scope[2 * i], also = scope[2 * i + 1];
            keep = a.node_id == keep_for || cell == 0 || in_mine(cell) || (also != 0 && in_mine(also));
        }
        if (!keep) {
            if (keep_for != kKeepNone) out.scoped = true;
            continue;
        }
        if (out.actor_count >= kMaxActors) {
            w = "more actors for this node than kMaxActors";
            return false;
        }
        out.actors[out.actor_count++] = a;
    }
    return true;
}

void outside_rule(const DeployImage& img, size_t i, uint32_t& path_hash, uint8_t& flags, const char** name,
                  size_t* name_len) {
    const uint8_t* p = img.outside;
    for (size_t k = 0; k < i; ++k) p += 6 + p[5];
    path_hash = rd32(p);
    flags = p[4];
    if (name != nullptr) *name = reinterpret_cast<const char*>(p + 6);
    if (name_len != nullptr) *name_len = p[5];
}

bool apply_outside(const DeployImage& img, Namespace& ns) {
    uint32_t hashes[kMaxOutside];
    uint8_t flags[kMaxOutside];
    const size_t n = img.outside_count <= kMaxOutside ? img.outside_count : 0;
    for (size_t i = 0; i < n; ++i) outside_rule(img, i, hashes[i], flags[i]);
    return ns.set_outside(hashes, flags, n);
}

bool present_config(const ActorDecl& a, PresentConfig& out) {
    size_t hl = 0;
    if (a.type != ActorType::Present || !portable_header(a, out.place, hl) ||
        (a.cfg_len != hl + kPresentOwnCfgLen && a.cfg_len != hl + kPresentOwnCfgLenTls)) {
        return false;
    }
    const uint8_t* p = a.cfg + hl;
    std::memcpy(out.ip, p, 4);
    out.port = rd16(p + 4);
    out.keepalive_s = p[6];
    out.flags = p[7];
    const bool tls = (out.flags & kPresentTls) != 0;
    out.ca_crc = tls && a.cfg_len == hl + kPresentOwnCfgLenTls ? rd32(p + 8) : 0;
    return out.port != 0 && out.keepalive_s >= 5 && out.keepalive_s <= 120 && (out.flags & ~kPresentTls) == 0 &&
           a.cfg_len == hl + (tls ? kPresentOwnCfgLenTls : kPresentOwnCfgLen) &&
           (out.ip[0] | out.ip[1] | out.ip[2] | out.ip[3]) != 0;
}

bool led_config(const ActorDecl& a, LedConfig& out) {
    if (a.type != ActorType::Led || a.cfg_len != kLedCfgLen) {
        return false;
    }
    out.r = a.cfg[0];
    out.g = a.cfg[1];
    out.b = a.cfg[2];
    out.blink = a.cfg[3];
    out.period_ms = rd16(a.cfg + 4);
    if (out.blink > 1 || out.period_ms < 200 || out.period_ms > 10000) {
        return false;
    }
    return true;
}

bool fault_config(const ActorDecl& a, FaultConfig& out) {
    if (a.type != ActorType::Fault || a.cfg_len != kFaultCfgLen) {
        return false;
    }
    out.panic_after_ms = rd32(a.cfg);
    return out.panic_after_ms > 0;
}

bool svc_client_config(const ActorDecl& a, SvcClientConfig& out) {
    if (a.type != ActorType::SvcClient || a.cfg_len != kSvcClientCfgLen) {
        return false;
    }
    out.svc_hash = rd32(a.cfg);
    out.provider = rd16(a.cfg + 4);
    out.out_hash = rd32(a.cfg + 6);
    out.period_ms = rd16(a.cfg + 10);
    out.on_host_loss = a.cfg[12];
    return out.svc_hash != 0 && out.out_hash != 0 && out.svc_hash != out.out_hash && out.provider != 0 &&
           out.provider < kPortableNode && out.period_ms >= 100 && out.period_ms <= 60000 &&
           out.on_host_loss <= 1;
}

bool die_temp_config(const ActorDecl& a, DieTempConfig& out) {
    if (a.type != ActorType::DieTemp || a.cfg_len != kDieTempCfgLen) {
        return false;
    }
    out.out_hash = rd32(a.cfg);
    out.period_ms = rd16(a.cfg + 4);
    return out.out_hash != 0 && out.period_ms >= 200 && out.period_ms <= 60000;
}

bool mc_lender_config(const ActorDecl& a, McLenderConfig& out) {
    if (a.type != ActorType::McLender || a.cfg_len != kMcLenderCfgLen) {
        return false;
    }
    out.slots = a.cfg[0];
    return out.slots >= 1 && out.slots <= 4;
}

bool mc_job_config(const ActorDecl& a, McJobConfig& out) {
    if (a.type != ActorType::McJob || (a.cfg_len != kMcJobCfgLen && a.cfg_len != kMcJobCfgLenCtl)) {
        return false;
    }
    out.out_hash = rd32(a.cfg);
    out.units = rd16(a.cfg + 4);
    out.samples = rd32(a.cfg + 6);
    out.seed = rd32(a.cfg + 10);
    out.lend = a.cfg[14];
    out.start_after_ms = rd32(a.cfg + 15);
    out.ctl_hash = a.cfg_len == kMcJobCfgLenCtl ? rd32(a.cfg + 19) : 0;  // M22: started and stopped by a write
    return out.out_hash != 0 && out.units >= 1 && out.units <= 4096 && out.samples >= 1000 &&
           out.samples <= 100000000u && out.lend <= 1 && out.ctl_hash != out.out_hash;
}

bool speak_config(const ActorDecl& a, SpeakConfig& out) {
    if (a.type != ActorType::Speak || a.cfg == nullptr || a.cfg_len < kSpeakCfgFixed + 1) return false;
    out.svc_hash = rd32(a.cfg);
    out.provider = rd16(a.cfg + 4);
    out.ctl_hash = rd32(a.cfg + 6);
    out.out_hash = rd32(a.cfg + 10);
    out.text_len = a.cfg[14];
    if (out.text_len == 0 || out.text_len > kSpeakTextMax || a.cfg_len != kSpeakCfgFixed + out.text_len) return false;
    std::memcpy(out.text, a.cfg + kSpeakCfgFixed, out.text_len);
    return out.svc_hash != 0 && out.ctl_hash != 0 && out.out_hash != 0 && out.ctl_hash != out.out_hash;
}

bool file_hash_config(const ActorDecl& a, FileHashConfig& out) {
    if (a.type != ActorType::FileHash || a.cfg == nullptr || a.cfg_len < kFileHashCfgFixed + 1) return false;
    out.svc_hash = rd32(a.cfg);
    out.provider = rd16(a.cfg + 4);
    out.in_hash = rd32(a.cfg + 6);
    out.go_hash = rd32(a.cfg + 10);
    out.bytes_hash = rd32(a.cfg + 14);
    out.state_hash = rd32(a.cfg + 18);
    out.digest_hash = rd32(a.cfg + 22);
    out.ms_hash = rd32(a.cfg + 26);
    out.name_len = a.cfg[30];
    if (out.name_len == 0 || out.name_len > kFileNameMax || a.cfg_len != kFileHashCfgFixed + out.name_len) return false;
    std::memcpy(out.name, a.cfg + kFileHashCfgFixed, out.name_len);
    const uint32_t h[7] = {out.svc_hash, out.in_hash, out.go_hash, out.bytes_hash, out.state_hash, out.digest_hash,
                           out.ms_hash};
    for (int i = 0; i < 7; ++i) {
        if (h[i] == 0) return false;
        for (int j = 0; j < i; ++j) {
            if (h[i] == h[j]) return false;
        }
    }
    return true;
}

bool key_echo_config(const ActorDecl& a, KeyEchoConfig& out) {
    if (a.type != ActorType::KeyEcho || a.cfg == nullptr || a.cfg_len != kKeyEchoCfgLen) return false;
    out.in_hash = rd32(a.cfg);
    out.key_hash = rd32(a.cfg + 4);
    out.count_hash = rd32(a.cfg + 8);
    out.gaps_hash = rd32(a.cfg + 12);
    const uint32_t h[4] = {out.in_hash, out.key_hash, out.count_hash, out.gaps_hash};
    for (int i = 0; i < 4; ++i) {
        if (h[i] == 0) return false;
        for (int j = 0; j < i; ++j) {
            if (h[i] == h[j]) return false;
        }
    }
    return true;
}

bool portable_header(const ActorDecl& a, TickerConfig& out, size_t& len) {
    // Every field from scratch: need_caps and avoid_caps are not on the wire, a placement hook sets them, and a hook
    // that does not (the ticker's) kept whatever the reused staging entry held -- on the bench a ticker inherited the
    // presenter's "connected, not busy" rule after live package changes, and two boards ranked it onto C while the
    // third and the placement host ranked it onto the PC (M0-LOG session 42).
    out = TickerConfig{};
    if (a.cfg == nullptr || a.cfg_len < kTickerCfgFixed) {
        return false;
    }
    out.out_hash = rd32(a.cfg);
    out.period_ms = rd16(a.cfg + 4);
    out.count = a.cfg[6];
    len = kTickerCfgFixed + 3u * out.count;
    if (out.out_hash == 0 || out.period_ms < 50 || out.period_ms > 60000 || out.count == 0 ||
        out.count > kMaxEligible || a.cfg_len < len) {
        return false;
    }
    for (uint8_t i = 0; i < out.count; ++i) {
        const uint8_t* e = a.cfg + kTickerCfgFixed + 3u * i;
        out.node[i] = rd16(e);
        out.gravity[i] = e[2];
        if (out.node[i] == 0 || out.node[i] >= kPortableNode) {
            return false;
        }
        for (uint8_t j = 0; j < i; ++j) {
            if (out.node[j] == out.node[i]) {
                return false;  // listed twice: the build tool has a bug, and the rank would be ambiguous
            }
        }
    }
    return true;
}

bool ticker_config(const ActorDecl& a, TickerConfig& out) {
    size_t len = 0;
    return a.type == ActorType::Ticker && portable_header(a, out, len) && a.cfg_len == len;
}

size_t encode_ticker_config(const TickerConfig& c, uint8_t* out, size_t cap) {
    const size_t len = kTickerCfgFixed + 3u * c.count;
    if (c.count > kMaxEligible || cap < len) return 0;
    for (int k = 0; k < 4; ++k) out[k] = static_cast<uint8_t>(c.out_hash >> (8 * k));
    out[4] = static_cast<uint8_t>(c.period_ms);
    out[5] = static_cast<uint8_t>(c.period_ms >> 8);
    out[6] = c.count;
    for (uint8_t i = 0; i < c.count; ++i) {
        uint8_t* e = out + kTickerCfgFixed + 3u * i;
        e[0] = static_cast<uint8_t>(c.node[i]);
        e[1] = static_cast<uint8_t>(c.node[i] >> 8);
        e[2] = c.gravity[i];
    }
    return len;
}

// ---------------------------------------------------------------------------------------------

const char* boot_outcome_str(BootOutcome o) {
    switch (o) {
        case BootOutcome::NoDeployment: return "no_deployment";
        case BootOutcome::Confirmed: return "confirmed";
        case BootOutcome::Trial: return "trial";
        case BootOutcome::Reverted: return "reverted";
    }
    return "?";
}

BootOutcome decide_boot(DeployState& s, uint8_t max_trial_boots) {
    if (s.active == kSlotNone) {
        return BootOutcome::NoDeployment;
    }
    if (s.pending == 0) {
        return BootOutcome::Confirmed;
    }
    if (s.trial_boots >= max_trial_boots) {
        revert_now(s);
        return BootOutcome::Reverted;
    }
    ++s.trial_boots;
    return BootOutcome::Trial;
}

void revert_now(DeployState& s) {
    s.active = s.previous;
    s.previous = kSlotNone;
    s.pending = 0;
    s.trial_boots = 0;
}

void confirm(DeployState& s) {
    s.pending = 0;
    s.trial_boots = 0;
}

// ---------------------------------------------------------------------------------------------

const char* deploy_status_str(DeployStatus s) {
    switch (s) {
        case DeployStatus::Ok: return "OK";
        case DeployStatus::TooLarge: return "TOO_LARGE";
        case DeployStatus::Downgrade: return "DOWNGRADE";
        case DeployStatus::NotStarted: return "NOT_STARTED";
        case DeployStatus::BadOffset: return "BAD_OFFSET";
        case DeployStatus::CrcMismatch: return "CRC_MISMATCH";
        case DeployStatus::BadImage: return "BAD_IMAGE";
        case DeployStatus::TrialInProgress: return "TRIAL_IN_PROGRESS";
        case DeployStatus::StoreFailed: return "STORE_FAILED";
        case DeployStatus::Malformed: return "MALFORMED";
        case DeployStatus::Busy: return "BUSY";
        case DeployStatus::Unsigned: return "UNSIGNED";
        case DeployStatus::BadSignature: return "BAD_SIGNATURE";
    }
    return "?";
}

bool load_deploy_begin(const uint8_t* p, size_t len, DeployBegin& out) {
    if (len < kDeployBeginLen) {
        return false;
    }
    out.image_len = rd32(p);
    out.image_crc = rd32(p + 4);
    out.rollback_counter = rd32(p + 8);
    std::memcpy(out.package_digest, p + 12, sizeof(out.package_digest));
    out.flags = p[20];
    return true;
}

bool load_deploy_chunk(const uint8_t* p, size_t len, DeployChunk& out) {
    if (len < kDeployChunkHeaderLen) {
        return false;
    }
    out.offset = rd32(p);
    out.len = rd16(p + 4);
    if (len - kDeployChunkHeaderLen < out.len || out.len > kDeployChunkMax) {
        return false;
    }
    out.data = p + kDeployChunkHeaderLen;
    return true;
}

bool load_deploy_commit(const uint8_t* p, size_t len, uint32_t& crc) {
    if (len < kDeployCommitLen) {
        return false;
    }
    crc = rd32(p);
    return true;
}

size_t store_deploy_begin(const DeployBegin& b, uint8_t* out, size_t cap) {
    if (cap < kDeployBeginLen) {
        return 0;
    }
    std::memset(out, 0, kDeployBeginLen);
    wr32(out, b.image_len);
    wr32(out + 4, b.image_crc);
    wr32(out + 8, b.rollback_counter);
    std::memcpy(out + 12, b.package_digest, sizeof(b.package_digest));
    out[20] = b.flags;
    return kDeployBeginLen;
}

size_t store_deploy_chunk(uint32_t offset, const uint8_t* data, uint16_t len, uint8_t* out, size_t cap) {
    if (len > kDeployChunkMax || cap < kDeployChunkHeaderLen + len) {
        return 0;
    }
    wr32(out, offset);
    wr16(out + 4, len);
    std::memcpy(out + kDeployChunkHeaderLen, data, len);
    return kDeployChunkHeaderLen + len;
}

size_t store_deploy_commit(uint32_t crc, uint8_t* out, size_t cap) {
    if (cap < kDeployCommitLen) {
        return 0;
    }
    wr32(out, crc);
    return kDeployCommitLen;
}

size_t store_deploy_reply(const DeployReply& r, uint8_t* out, size_t cap) {
    if (cap < kDeployReplyLen) {
        return 0;
    }
    std::memset(out, 0, kDeployReplyLen);
    wr16(out, static_cast<uint16_t>(r.status));
    out[2] = r.slot;
    out[3] = r.pending;
    wr32(out + 4, r.received);
    wr32(out + 8, r.counter);
    out[12] = r.peers_ok;
    out[13] = r.peers_failed;
    return kDeployReplyLen;
}

bool load_deploy_reply(const uint8_t* p, size_t len, DeployReply& out) {
    if (len < kDeployReplyLen) {
        return false;
    }
    out.status = static_cast<DeployStatus>(rd16(p));
    out.slot = p[2];
    out.pending = p[3];
    out.received = rd32(p + 4);
    out.counter = rd32(p + 8);
    out.peers_ok = p[12];
    out.peers_failed = p[13];
    return true;
}

// ---------------------------------------------------------------------------------------------

DeployReply DeployReceiver::reply(DeployStatus s) const {
    DeployReply r{};
    r.status = s;
    r.slot = target_;
    r.pending = committed_ ? 1 : 0;
    r.received = static_cast<uint32_t>(received_);
    r.counter = begin_.rollback_counter;
    return r;
}

DeployReply DeployReceiver::begin(const DeployBegin& b) {
    started_ = false;
    committed_ = false;
    received_ = 0;
    begin_ = b;
    DeployState s{};
    store_.load_state(s);
    if (s.pending != 0) {
        // Writing the inactive slot now would overwrite the trial's only fallback.
        return reply(DeployStatus::TrialInProgress);
    }
    if (b.rollback_counter < s.counter) {
        return reply(DeployStatus::Downgrade);
    }
    if (b.image_len == 0 || b.image_len > cap_ || b.image_len > kMaxImageLen) {
        return reply(DeployStatus::TooLarge);
    }
    target_ = (s.active == kSlotA) ? kSlotB : kSlotA;
    started_ = true;
    return reply(DeployStatus::Ok);
}

DeployReply DeployReceiver::chunk(const DeployChunk& c) {
    if (!started_) {
        return reply(DeployStatus::NotStarted);
    }
    if (c.offset != received_) {
        return reply(DeployStatus::BadOffset);
    }
    if (received_ + c.len > begin_.image_len) {
        return reply(DeployStatus::TooLarge);
    }
    std::memcpy(buf_ + received_, c.data, c.len);
    received_ += c.len;
    return reply(DeployStatus::Ok);
}

DeployReply DeployReceiver::commit(uint32_t crc) {
    if (!started_) {
        return reply(DeployStatus::NotStarted);
    }
    if (received_ != begin_.image_len) {
        return reply(DeployStatus::BadOffset);
    }
    if (crc != begin_.image_crc || crc32(buf_, received_) != crc) {
        return reply(DeployStatus::CrcMismatch);
    }
    // The image's own header says where it ends; anything after it is the M5 signature trailer. M7: in
    // version 2 the end is after the guest section, which body_len does not cover.
    const size_t image_len = image_extent(buf_, received_);
    if (image_len == 0) {
        return reply(DeployStatus::BadImage);
    }
    if (verifier_ != nullptr) {
        const DeployStatus v = verifier_(verifier_ctx_, buf_, image_len, buf_ + image_len, received_ - image_len);
        if (v != DeployStatus::Ok) {
            return reply(v);
        }
    }
    DeployImage& img = validation_image();
    if (!parse_image(buf_, image_len, img, nullptr, kKeepNone) || img.rollback_counter != begin_.rollback_counter) {
        return reply(DeployStatus::BadImage);
    }
    DeployState s{};
    store_.load_state(s);
    if (s.pending != 0) {
        return reply(DeployStatus::TrialInProgress);
    }
    // M12: the slot keeps the stream as signed -- image and trailer -- so this node can pass the package on
    // later, verifiably, to a peer that was off when it was deployed. image_extent() finds the image in it.
    if (!store_.write_slot(target_, buf_, received_)) {
        return reply(DeployStatus::StoreFailed);
    }
    s.previous = s.active;  // confirmed, since a pending one was refused above
    s.active = target_;
    s.pending = 1;
    s.trial_boots = 0;
    if (begin_.rollback_counter > s.counter) {
        s.counter = begin_.rollback_counter;
    }
    if (!store_.save_state(s)) {
        return reply(DeployStatus::StoreFailed);
    }
    started_ = false;
    committed_ = true;
    return reply(DeployStatus::Ok);
}

void DeployReceiver::abort() {
    started_ = false;
    committed_ = false;
    received_ = 0;
}

}  // namespace pot
