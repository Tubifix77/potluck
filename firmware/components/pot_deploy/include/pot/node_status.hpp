// M12: what one board is running, in one reply -- the cell inventory `potluck apply` plans from.
//
// The host asks the board on its cable (NODE_STATUS, 0x38, inside the deploy range) with a target node id;
// a target other than the cabled board is passed on over the radio, the way FW_STATUS is (M11). The reply
// says which firmware runs (counter, floor, state, version, the ELF's SHA-256 prefix), which package runs
// (slot, trial, counter, digest), what the board is (its roles), what runs on it, and the cluster's name.
//
// Wire, little-endian, version 1:
//   u8 version, u16 node,
//   u32 fw_running, u32 fw_floor, u8 fw_state (0 factory, 1 confirmed, 2 on trial),
//   char[32] fw_version (NUL-padded), u8[8] elf_sha,
//   u8 slot, u8 pending, u8 trial_boots, u32 pkg_counter (running; 0 = built-in behaviour), u32 pkg_floor,
//   u8[8] pkg_digest, u8 roles, u32 live_applies, u32 ck_writes, u32 uptime_s,
//   u8 pinned_running, u8 portable_total, u8 portable_here,
//   u8 cluster_len, cluster bytes, u8 actors_len, actors bytes (names, comma-separated; may be cut short)
// Version 2 (M13) adds, last:
//   u8 module_count, module_count x (u8 id, u8 state, u8 reason)  -- Potluck's optional modules this image
//   carries (ModuleId), each with what it is doing now. A module the image does not carry is not listed. App
//   components are not listed: which an image carries follows from its ELF, and their actors are in `actors`.
// Both versions are read; a board answers with version 2.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kNodeStatusVersion = 2;
constexpr uint8_t kNodeStatusVersion1 = 1;
constexpr size_t kNodeStatusVersionLen = 32;  // esp_app_desc_t::version
constexpr size_t kNodeStatusFixedLen = 87;          // through portable_here
constexpr size_t kNodeStatusMaxCluster = 31;
constexpr size_t kNodeStatusMaxActors = 64;  // 88 in version 1; cut to leave room for the modules
constexpr size_t kNodeStatusMaxModules = 8;
constexpr size_t kNodeStatusMaxLen =
    kNodeStatusFixedLen + 1 + kNodeStatusMaxCluster + 1 + kNodeStatusMaxActors + 1 + 3 * kNodeStatusMaxModules;  // 209

enum NodeRole : uint8_t {
    kRoleExtenderBuild = 0x01,  // the image carries the extender
    kRoleExtenderActive = 0x02, // ... and credentials are stored, so it is one
    kRoleSandbox = 0x04,        // M7's guests can run here
    kRoleReconciler = 0x08,     // portable actors can run here
    kRoleRelay = 0x10,          // relays for its peers (ADR-009)
    kRoleEnrolled = 0x20,       // holds a certificate under the cluster CA
    kRoleCabled = 0x40,         // the board that answered over the cable (set by the board the host asked)
};

// M13: Potluck's optional modules (ARCHITECTURE section 13's table) -- each one in a board's image only when the
// cluster file names it, so the inventory and the dashboard can show it as on and active, on and idle, or not there.
enum class ModuleId : uint8_t {
    Sandbox = 1,   // M7's guests (CONFIG_POT_WASM)
    Extender = 2,  // the Wi-Fi extender app's hotspot (poor-mans-extender, an external component)
    Wifi = 3,      // M13: Potluck's own Wi-Fi station
    Present = 4,   // M13: the presenting role
    Pocket = 5,    // M13.1: the pocket page
};
enum class ModuleState : uint8_t { Active = 1, Idle = 2, Failed = 3 };
// Why a module is idle or failed. Kept small and stable: the host prints the words.
enum class ModuleReason : uint8_t {
    None = 0,
    NoCredentials = 1,  // nothing to connect with: the owner has not typed them
    NoNetwork = 2,      // credentials, but not connected (yet)
    Standby = 3,        // a role another board holds now
    NothingHere = 4,    // nothing placed on this board uses it
    NoMemory = 5,
    Refused = 6,        // its peer refused it (a broker's login, say)
    Error = 7,
    OtherConnection = 8,  // another module is the board's network connection (the extender's station)
};
struct ModuleStatus {
    ModuleId id;
    ModuleState state;
    ModuleReason reason;
};

struct NodeStatus {
    uint16_t node = 0;
    uint32_t fw_running = 0, fw_floor = 0;
    uint8_t fw_state = 0;
    char fw_version[kNodeStatusVersionLen + 1] = {};
    uint8_t elf_sha[8] = {};
    uint8_t slot = 0, pending = 0, trial_boots = 0;
    uint32_t pkg_counter = 0, pkg_floor = 0;
    uint8_t pkg_digest[8] = {};
    uint8_t roles = 0;
    uint32_t live_applies = 0, ck_writes = 0, uptime_s = 0;
    uint8_t pinned_running = 0, portable_total = 0, portable_here = 0;
    char cluster[kNodeStatusMaxCluster + 1] = {};
    char actors[kNodeStatusMaxActors + 1] = {};
    uint8_t module_count = 0;  // version 2
    ModuleStatus modules[kNodeStatusMaxModules] = {};
};

// Returns the bytes written, 0 if `cap` is short. Strings longer than their field are cut.
size_t store_node_status(const NodeStatus& s, uint8_t* out, size_t cap);
bool load_node_status(const uint8_t* in, size_t len, NodeStatus& s);

}  // namespace pot
