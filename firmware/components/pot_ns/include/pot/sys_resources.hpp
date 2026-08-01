// The resources every Potluck node owns, whatever else it does — §7.2, §1.1.
//
// A node with an empty namespace is a node nothing can be asked about, and M1's acceptance test
// needs *something* real to read across a link. These are the resources any board has by virtue of
// being a board: how much memory is left, how long it has been up, how well it hears its peers.
//
// They are declared here rather than in the application because the paths are a contract. The host
// resolves a hash back to a name using this same list (host/potluck/potluck/sys_paths.py),
// and a node that invented its own spelling would be unreadable to any tool.
//
// The `%s` is the node's own id in lowercase hex, so `potluck://lab/node-1a2b/sys/heap-free`. The
// cluster segment is fixed at `lab` until M3 introduces manifests that can name one.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/namespace.hpp"
#include "pot/value.hpp"

namespace pot {

// How many built-in resources a node declares. Kept small deliberately: §6 caps the table at 128
// entries and these must not crowd out the application's.
constexpr size_t kSysResourceCount = 6;

enum class SysResource : uint8_t {
    HeapFree = 0,      // bytes of free internal DRAM — §6's [MEASURE] item, live
    HeapLargest = 1,   // largest allocatable internal block; free space alone hides fragmentation
    UptimeSeconds = 2, // seconds, not milliseconds: a uint32 of ms wraps in 49.7 days and §1.2
                       // expects nodes to run indefinitely
    BootEpoch = 3,     // which incarnation this is
    PeersAlive = 4,    // how many peers this node currently believes are up
    LinkRssi = 5,      // dBm of the most recently received frame, worst across peers
};

// The path suffix for a resource, e.g. "sys/heap-free". The full path is built by
// sys_resource_path().
const char* sys_resource_suffix(SysResource r);

// Build the canonical path for one of these on a given node into `out`. Returns `out`.
// `out` needs 64 bytes.
const char* sys_resource_path(char* out, size_t cap, uint16_t node_id, SysResource r);

// The hash of that path — what the namespace actually stores.
uint32_t sys_resource_hash(uint16_t node_id, SysResource r);

// The declaration for one of these: type, unit, class and staleness bound.
//
// The staleness bounds are chosen from what the quantity actually means rather than from a round
// number. Free heap changes continuously, so a reading older than a few seconds is a guess; a boot
// epoch never changes at all, so it has no staleness (bound 0). Getting these wrong is how §4
// rule 2 becomes decoration — a resource whose bound is far larger than its true rate of change
// reports Good long after it has stopped being true.
NsDecl sys_resource_decl(uint16_t node_id, SysResource r);

// Declare all of them into `ns` for this node. Returns the number successfully declared.
size_t declare_sys_resources(Namespace& ns, uint16_t node_id);

}  // namespace pot
