// M12: the inventory reply. See pot/node_status.hpp.

#include "pot/node_status.hpp"

#include <algorithm>
#include <cstring>

namespace pot {

namespace {

void put16(uint8_t*& p, uint16_t v) {
    *p++ = static_cast<uint8_t>(v);
    *p++ = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t*& p, uint32_t v) {
    for (int i = 0; i < 4; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t*& p) {
    const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
    p += 2;
    return v;
}
uint32_t get32(const uint8_t*& p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    p += 4;
    return v;
}

}  // namespace

size_t store_node_status(const NodeStatus& s, uint8_t* out, size_t cap) {
    const size_t cl = std::min(std::strlen(s.cluster), kNodeStatusMaxCluster);
    const size_t al = std::min(std::strlen(s.actors), kNodeStatusMaxActors);
    const size_t mc = std::min<size_t>(s.module_count, kNodeStatusMaxModules);
    const size_t total = kNodeStatusFixedLen + 1 + cl + 1 + al + 1 + 3 * mc + kNodeStatusMacLen;
    if (cap < total) return 0;
    uint8_t* p = out;
    *p++ = kNodeStatusVersion;
    put16(p, s.node);
    put32(p, s.fw_running);
    put32(p, s.fw_floor);
    *p++ = s.fw_state;
    std::memset(p, 0, kNodeStatusVersionLen);
    std::memcpy(p, s.fw_version, std::min(std::strlen(s.fw_version), kNodeStatusVersionLen));
    p += kNodeStatusVersionLen;
    std::memcpy(p, s.elf_sha, 8);
    p += 8;
    *p++ = s.slot;
    *p++ = s.pending;
    *p++ = s.trial_boots;
    put32(p, s.pkg_counter);
    put32(p, s.pkg_floor);
    std::memcpy(p, s.pkg_digest, 8);
    p += 8;
    *p++ = s.roles;
    put32(p, s.live_applies);
    put32(p, s.ck_writes);
    put32(p, s.uptime_s);
    *p++ = s.pinned_running;
    *p++ = s.portable_total;
    *p++ = s.portable_here;
    *p++ = static_cast<uint8_t>(cl);
    std::memcpy(p, s.cluster, cl);
    p += cl;
    *p++ = static_cast<uint8_t>(al);
    std::memcpy(p, s.actors, al);
    p += al;
    *p++ = static_cast<uint8_t>(mc);
    for (size_t i = 0; i < mc; ++i) {
        *p++ = static_cast<uint8_t>(s.modules[i].id);
        *p++ = static_cast<uint8_t>(s.modules[i].state);
        *p++ = static_cast<uint8_t>(s.modules[i].reason);
    }
    std::memcpy(p, s.mac, kNodeStatusMacLen);
    p += kNodeStatusMacLen;
    return static_cast<size_t>(p - out);
}

bool load_node_status(const uint8_t* in, size_t len, NodeStatus& s) {
    if (in == nullptr || len < kNodeStatusFixedLen + 2 ||
        (in[0] != kNodeStatusVersion && in[0] != kNodeStatusVersion2 && in[0] != kNodeStatusVersion1)) {
        return false;
    }
    const bool v2 = in[0] >= kNodeStatusVersion2;  // the modules, and from version 3 the MAC after them
    const size_t tail = in[0] >= kNodeStatusVersion ? kNodeStatusMacLen : 0;
    s = NodeStatus{};
    const uint8_t* p = in + 1;
    s.node = get16(p);
    s.fw_running = get32(p);
    s.fw_floor = get32(p);
    s.fw_state = *p++;
    std::memcpy(s.fw_version, p, kNodeStatusVersionLen);
    s.fw_version[kNodeStatusVersionLen] = 0;
    p += kNodeStatusVersionLen;
    std::memcpy(s.elf_sha, p, 8);
    p += 8;
    s.slot = *p++;
    s.pending = *p++;
    s.trial_boots = *p++;
    s.pkg_counter = get32(p);
    s.pkg_floor = get32(p);
    std::memcpy(s.pkg_digest, p, 8);
    p += 8;
    s.roles = *p++;
    s.live_applies = get32(p);
    s.ck_writes = get32(p);
    s.uptime_s = get32(p);
    s.pinned_running = *p++;
    s.portable_total = *p++;
    s.portable_here = *p++;
    const uint8_t* end = in + len;
    const size_t cl = *p++;
    if (cl > kNodeStatusMaxCluster || static_cast<size_t>(end - p) < cl + 1) return false;
    std::memcpy(s.cluster, p, cl);
    p += cl;
    const size_t al = *p++;
    // Version 1 allowed 88 bytes of names; this struct holds kNodeStatusMaxActors, so a longer list is cut.
    if (al > (v2 ? kNodeStatusMaxActors : size_t{88})) return false;
    if (v2 ? static_cast<size_t>(end - p) < al + 1 : static_cast<size_t>(end - p) != al) return false;
    std::memcpy(s.actors, p, std::min(al, kNodeStatusMaxActors));
    p += al;
    if (!v2) return true;
    const size_t mc = *p++;
    if (mc > kNodeStatusMaxModules || static_cast<size_t>(end - p) != 3 * mc + tail) return false;
    for (size_t i = 0; i < mc; ++i) {
        s.modules[i] = ModuleStatus{static_cast<ModuleId>(p[0]), static_cast<ModuleState>(p[1]),
                                    static_cast<ModuleReason>(p[2])};
        p += 3;
    }
    s.module_count = static_cast<uint8_t>(mc);
    if (tail != 0) std::memcpy(s.mac, p, kNodeStatusMacLen);
    return true;
}

}  // namespace pot
