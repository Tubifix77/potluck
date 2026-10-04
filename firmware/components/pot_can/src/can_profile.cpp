#include "pot/can_profile.hpp"

#include <cstring>

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"

namespace pot {

namespace {

constexpr uint8_t kSegFirst = 0x80;
constexpr uint8_t kSegLast = 0x40;
constexpr uint8_t kSegIndexMask = 0x3F;

}  // namespace

uint8_t can_alias_of(uint16_t node_id) { return static_cast<uint8_t>(node_id % 63u); }

void can_mac_of(uint16_t node_id, uint8_t mac[6]) {
    mac[0] = 0x02;
    mac[1] = 0xCA;
    mac[2] = 0x00;
    mac[3] = 0x00;
    mac[4] = static_cast<uint8_t>(node_id >> 8);
    mac[5] = static_cast<uint8_t>(node_id);
}

bool can_mac_node(const uint8_t mac[6], uint16_t& node_id) {
    if (mac[0] != 0x02 || mac[1] != 0xCA || mac[2] != 0x00 || mac[3] != 0x00) {
        return false;
    }
    node_id = static_cast<uint16_t>((mac[4] << 8) | mac[5]);
    return true;
}

uint32_t can_id_pack(const CanId& id) {
    const uint32_t pri = 31u - (id.priority & 0x1Fu);
    return (pri << 24) | (static_cast<uint32_t>(id.opcode & 0x7Fu) << 17) |
           (static_cast<uint32_t>(id.src & 0x3Fu) << 11) | (static_cast<uint32_t>(id.dst & 0x3Fu) << 5) |
           (id.single ? (1u << 4) : 0u) | (id.index & 0x0Fu);
}

CanId can_id_unpack(uint32_t raw) {
    CanId id{};
    id.priority = static_cast<uint8_t>(31u - ((raw >> 24) & 0x1Fu));
    id.opcode = static_cast<uint8_t>((raw >> 17) & 0x7Fu);
    id.src = static_cast<uint8_t>((raw >> 11) & 0x3Fu);
    id.dst = static_cast<uint8_t>((raw >> 5) & 0x3Fu);
    id.single = ((raw >> 4) & 1u) != 0;
    id.index = static_cast<uint8_t>(raw & 0x0Fu);
    return id;
}

bool can_single_frame_class(uint8_t opcode, uint8_t& lclass) {
    switch (opcode) {
        case kOpHeartbeat: lclass = kClassL3; return true;   // the 8-byte beacon, as Node sends it
        case kOpSafeState: lclass = kClassL0; return true;
        default: return false;
    }
}

size_t can_encode(const uint8_t* raw, size_t len, uint8_t src_alias, uint8_t dst_alias, CanFrame* out,
                  size_t max_out) {
    Frame f;
    if (raw == nullptr || parse(raw, len, f) != FrameError::Ok) {
        return 0;
    }
    uint8_t implied = 0;
    const bool single = can_single_frame_class(f.hdr.opcode, implied) && f.payload_len <= 8 &&
                        f.hdr.seq == 0 && f.hdr.msg_id == 0 && !f.is_frag() && !f.wants_ack() &&
                        !f.has_auth() && f.lclass() == implied;
    if (single) {
        if (max_out < 1) {
            return 0;
        }
        CanId id{f.priority(), f.hdr.opcode, src_alias, dst_alias, true, 0};
        out[0].id = can_id_pack(id);
        out[0].dlc = static_cast<uint8_t>(f.payload_len);
        std::memset(out[0].data, 0, sizeof(out[0].data));
        std::memcpy(out[0].data, f.payload, f.payload_len);
        return 1;
    }
    if (len > kCanMaxMessage) {
        return 0;
    }
    const size_t n = (len + kCanSegData - 1) / kCanSegData;
    if (n > max_out) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t off = i * kCanSegData;
        const size_t chunk = (len - off < kCanSegData) ? len - off : kCanSegData;
        CanId id{f.priority(), f.hdr.opcode, src_alias, dst_alias, false, static_cast<uint8_t>(i & 0x0Fu)};
        CanFrame& c = out[i];
        c.id = can_id_pack(id);
        std::memset(c.data, 0, sizeof(c.data));
        c.data[0] = static_cast<uint8_t>((i == 0 ? kSegFirst : 0) | (i + 1 == n ? kSegLast : 0) |
                                         (i & kSegIndexMask));
        std::memcpy(c.data + 1, raw + off, chunk);
        c.dlc = static_cast<uint8_t>(1 + chunk);
    }
    return n;
}

bool CanAliasTable::learn(uint8_t alias, uint16_t node_id) {
    alias &= 0x3F;
    if (known_[alias] && node_[alias] != node_id) {
        ++conflicts_;
        return false;
    }
    known_[alias] = true;
    node_[alias] = node_id;
    return true;
}

bool CanAliasTable::lookup(uint8_t alias, uint16_t& node_id) const {
    alias &= 0x3F;
    if (!known_[alias]) {
        return false;
    }
    node_id = node_[alias];
    return true;
}

size_t CanReassembler::single(const CanFrame& f, const CanId& id, uint8_t* out, size_t cap) {
    uint8_t lclass = 0;
    if (!can_single_frame_class(id.opcode, lclass) || f.dlc > 8) {
        ++c_.malformed;
        return 0;
    }
    uint16_t src = 0;
    if (id.opcode == kOpHeartbeat && f.dlc == 8) {
        // A beacon names its sender: learn the alias before resolving it.
        const uint16_t named = static_cast<uint16_t>(f.data[0] | (f.data[1] << 8));
        if (can_alias_of(named) != id.src) {
            ++c_.malformed;  // a beacon whose node id does not derive its own alias
            return 0;
        }
        aliases_.learn(id.src, named);
    }
    if (!aliases_.lookup(id.src, src)) {
        ++c_.unknown_alias;
        return 0;
    }
    EncodeSpec spec;
    spec.src = src;
    spec.dst = kNodeBroadcast;
    if (id.dst != kCanBroadcastAlias) {
        uint16_t dst = 0;
        if (!aliases_.lookup(id.dst, dst)) {
            ++c_.unknown_alias;
            return 0;
        }
        spec.dst = dst;
    }
    spec.opcode = id.opcode;
    spec.lclass = lclass;
    spec.priority = id.priority;
    size_t written = 0;
    if (encode(spec, f.data, f.dlc, out, cap, written) != FrameError::Ok) {
        ++c_.malformed;
        return 0;
    }
    ++c_.singles;
    ++c_.messages;
    return written;
}

size_t CanReassembler::feed(const CanFrame& f, uint32_t now_ms, uint8_t* out, size_t cap) {
    ++c_.frames;
    expire(now_ms);
    const CanId id = can_id_unpack(f.id);
    if (id.dst != kCanBroadcastAlias && id.dst != my_alias_) {
        ++c_.not_for_us;
        return 0;
    }
    if (id.single) {
        return single(f, id, out, cap);
    }
    if (f.dlc < 2 || f.dlc > 8) {
        ++c_.malformed;
        return 0;
    }
    const uint8_t seg = f.data[0];
    const uint8_t index = seg & kSegIndexMask;
    const size_t chunk = static_cast<size_t>(f.dlc - 1);

    Slot* s = nullptr;
    for (Slot& q : slots_) {
        if (q.used && q.src == id.src) {
            s = &q;
            break;
        }
    }
    if ((seg & kSegFirst) != 0) {
        if (index != 0) {
            ++c_.malformed;
            return 0;
        }
        if (s != nullptr) {
            ++c_.out_of_order;  // a new message while the last was unfinished: the last is lost
        } else {
            for (Slot& q : slots_) {
                if (!q.used) {
                    s = &q;
                    break;
                }
            }
            if (s == nullptr) {
                ++c_.no_slot;
                return 0;
            }
        }
        s->used = true;
        s->src = id.src;
        s->next = 0;
        s->len = 0;
        s->started_ms = now_ms;
    } else if (s == nullptr || index != s->next) {
        ++c_.out_of_order;
        if (s != nullptr) {
            s->used = false;
        }
        return 0;
    }
    if (s->len + chunk > kCanMaxMessage) {
        ++c_.malformed;
        s->used = false;
        return 0;
    }
    std::memcpy(s->buf + s->len, f.data + 1, chunk);
    s->len += chunk;
    s->next = static_cast<uint8_t>(index + 1);
    if ((seg & kSegLast) == 0) {
        return 0;
    }
    s->used = false;
    if (s->len > cap) {
        ++c_.malformed;
        return 0;
    }
    std::memcpy(out, s->buf, s->len);
    ++c_.messages;
    return s->len;
}

void CanReassembler::expire(uint32_t now_ms) {
    for (Slot& q : slots_) {
        if (q.used && (now_ms - q.started_ms) > kCanReassemblyTimeoutMs) {
            q.used = false;
            ++c_.timeouts;
        }
    }
}

}  // namespace pot
