// Built-in node resources — see sys_resources.hpp.

#include "pot/sys_resources.hpp"

#include <cstdio>
#include <cstring>

namespace pot {

namespace {
char g_system[32] = "lab";
}  // namespace

const char* sys_system() { return g_system; }

void set_sys_system(const char* name) {
    if (name == nullptr || name[0] == '\0') name = "lab";
    std::strncpy(g_system, name, sizeof(g_system) - 1);
    g_system[sizeof(g_system) - 1] = '\0';
}

const char* sys_resource_suffix(SysResource r) {
    switch (r) {
        case SysResource::HeapFree: return "sys/heap-free";
        case SysResource::HeapLargest: return "sys/heap-largest";
        case SysResource::UptimeSeconds: return "sys/uptime";
        case SysResource::BootEpoch: return "sys/boot-epoch";
        case SysResource::PeersAlive: return "sys/peers-alive";
        case SysResource::LinkRssi: return "sys/rssi";
        case SysResource::ResetReason: return "sys/reset";
    }
    return "sys/?";
}

const char* sys_resource_path(char* out, size_t cap, uint16_t node_id, SysResource r) {
    std::snprintf(out, cap, "potluck://%s/node-%04x/%s", g_system, static_cast<unsigned>(node_id),
                  sys_resource_suffix(r));
    return out;
}

uint32_t sys_resource_hash(uint16_t node_id, SysResource r) {
    char buf[64];
    return path_hash(sys_resource_path(buf, sizeof(buf), node_id, r));
}

NsDecl sys_resource_decl(uint16_t node_id, SysResource r) {
    NsDecl d;
    d.path_hash = sys_resource_hash(node_id, r);
    d.owner_node = node_id;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;  // all read-only; nothing here is a setpoint
    // L4: these are diagnostics. Nothing with a deadline should be reading a peer's heap.
    d.latency_class = kClassL4;
    d.staleness_policy = StalenessPolicy::Informative;

    switch (r) {
        case SysResource::HeapFree:
        case SysResource::HeapLargest:
            d.type = ValueType::U32;
            d.unit = Unit::Byte;
            // Free memory moves continuously, so a reading much older than the statistics interval
            // is a guess rather than a measurement.
            d.staleness_bound_ms = 30000;
            break;
        case SysResource::UptimeSeconds:
            d.type = ValueType::U32;
            d.unit = Unit::Second;
            d.staleness_bound_ms = 30000;
            break;
        case SysResource::BootEpoch:
            d.type = ValueType::U32;
            d.unit = Unit::Count;
            // Never changes within an incarnation, so it cannot go stale. §4's bound of 0 means
            // exactly that, and using it here rather than an enormous number keeps the intent
            // legible.
            d.staleness_bound_ms = 0;
            break;
        case SysResource::PeersAlive:
            d.type = ValueType::U32;
            d.unit = Unit::Count;
            // Membership changes on §8.2's timescale: a peer can die in 600 ms, so a peer count a
            // minute old is describing a different cluster.
            d.staleness_bound_ms = 5000;
            break;
        case SysResource::LinkRssi:
            d.type = ValueType::I32;
            d.unit = Unit::None;  // dBm; there is no SI unit code for it and inventing one would lie
            d.staleness_bound_ms = 5000;
            break;
        case SysResource::ResetReason:
            d.type = ValueType::U32;
            d.unit = Unit::None;  // a code, not a quantity
            d.staleness_bound_ms = 0;  // fixed for the incarnation, like the boot epoch
            break;
    }
    return d;
}

size_t declare_sys_resources(Namespace& ns, uint16_t node_id) {
    size_t n = 0;
    for (uint8_t i = 0; i < kSysResourceCount; ++i) {
        if (ns.declare(sys_resource_decl(node_id, static_cast<SysResource>(i))) == NsError::Ok) {
            ++n;
        }
    }
    return n;
}

const char* extender_resource_suffix(ExtenderResource r) {
    switch (r) {
        case ExtenderResource::RouterRssi: return "sys/ext/router-rssi";
        case ExtenderResource::Clients: return "sys/ext/clients";
        case ExtenderResource::Reconnects: return "sys/ext/reconnects";
    }
    return "sys/ext/?";
}

uint32_t extender_resource_hash(uint16_t node_id, ExtenderResource r) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "potluck://%s/node-%04x/%s", g_system, static_cast<unsigned>(node_id),
                  extender_resource_suffix(r));
    return path_hash(buf);
}

NsDecl extender_resource_decl(uint16_t node_id, ExtenderResource r) {
    NsDecl d;
    d.path_hash = extender_resource_hash(node_id, r);
    d.owner_node = node_id;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_policy = StalenessPolicy::Informative;
    // Refreshed with the built-ins, once a statistics interval (10 s by default): three intervals before a reading is
    // a guess, as for the heap and the uptime.
    d.staleness_bound_ms = 30000;
    switch (r) {
        case ExtenderResource::RouterRssi:
            d.type = ValueType::I32;
            d.unit = Unit::None;  // dBm, as sys/rssi: no unit code for it
            break;
        case ExtenderResource::Clients:
            d.type = ValueType::U32;
            d.unit = Unit::Count;
            break;
        case ExtenderResource::Reconnects:
            d.type = ValueType::U32;
            d.unit = Unit::Count;
            break;
    }
    return d;
}

size_t declare_extender_resources(Namespace& ns, uint16_t node_id) {
    size_t n = 0;
    for (uint8_t i = 0; i < kExtenderResourceCount; ++i) {
        if (ns.declare(extender_resource_decl(node_id, static_cast<ExtenderResource>(i))) == NsError::Ok) ++n;
    }
    return n;
}

}  // namespace pot
