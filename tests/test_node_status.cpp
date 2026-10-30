// M12: the inventory reply round-trips, and refuses what is not one.

#include <cstring>

#include "pot/node_status.hpp"
#include "test_harness.hpp"

using namespace pot;

TEST(node_status, round_trips_every_field) {
    NodeStatus a;
    a.node = 0x7368;
    a.fw_running = 10;
    a.fw_floor = 9;
    a.fw_state = 2;
    std::strcpy(a.fw_version, "6235e98-dirty");
    for (int i = 0; i < 8; ++i) {
        a.elf_sha[i] = static_cast<uint8_t>(0xA0 + i);
        a.pkg_digest[i] = static_cast<uint8_t>(i);
    }
    a.slot = 2;
    a.pending = 1;
    a.trial_boots = 1;
    a.pkg_counter = 31;
    a.pkg_floor = 31;
    a.roles = kRoleExtenderBuild | kRoleSandbox | kRoleReconciler | kRoleEnrolled;
    a.live_applies = 3;
    a.ck_writes = 12;
    a.uptime_s = 3600;
    a.pinned_running = 2;
    a.portable_total = 4;
    a.portable_here = 1;
    std::strcpy(a.cluster, "home");
    std::strcpy(a.actors, "die_temp,rf_link,guest");
    a.module_count = 2;
    a.modules[0] = ModuleStatus{ModuleId::Sandbox, ModuleState::Active, ModuleReason::None};
    a.modules[1] = ModuleStatus{ModuleId::Extender, ModuleState::Idle, ModuleReason::NoCredentials};
    const uint8_t mac[6] = {0xb8, 0x1f, 0x3f, 0xda, 0x73, 0x68};
    std::memcpy(a.mac, mac, 6);
    uint8_t wire[kNodeStatusMaxLen];
    const size_t n = store_node_status(a, wire, sizeof(wire));
    CHECK_EQ(n, kNodeStatusFixedLen + 1 + 4 + 1 + std::strlen(a.actors) + 1 + 6 + kNodeStatusMacLen);
    NodeStatus b;
    CHECK(load_node_status(wire, n, b));
    CHECK_EQ(b.node, a.node);
    CHECK_EQ(b.fw_running, 10u);
    CHECK_EQ(b.fw_floor, 9u);
    CHECK_EQ(static_cast<int>(b.fw_state), 2);
    CHECK_STR_EQ(static_cast<const char*>(b.fw_version), "6235e98-dirty");
    CHECK_BYTES_EQ(b.elf_sha, 8, a.elf_sha, 8);
    CHECK_EQ(static_cast<int>(b.slot), 2);
    CHECK_EQ(static_cast<int>(b.pending), 1);
    CHECK_EQ(static_cast<int>(b.trial_boots), 1);
    CHECK_EQ(b.pkg_counter, 31u);
    CHECK_EQ(b.pkg_floor, 31u);
    CHECK_BYTES_EQ(b.pkg_digest, 8, a.pkg_digest, 8);
    CHECK_EQ(static_cast<int>(b.roles), static_cast<int>(a.roles));
    CHECK_EQ(b.live_applies, 3u);
    CHECK_EQ(b.ck_writes, 12u);
    CHECK_EQ(b.uptime_s, 3600u);
    CHECK_EQ(static_cast<int>(b.pinned_running), 2);
    CHECK_EQ(static_cast<int>(b.portable_total), 4);
    CHECK_EQ(static_cast<int>(b.portable_here), 1);
    CHECK_STR_EQ(static_cast<const char*>(b.cluster), "home");
    CHECK_STR_EQ(static_cast<const char*>(b.actors), "die_temp,rf_link,guest");
    CHECK_EQ(static_cast<int>(b.module_count), 2);
    CHECK_EQ(static_cast<int>(b.modules[1].id), static_cast<int>(ModuleId::Extender));
    CHECK_EQ(static_cast<int>(b.modules[1].state), static_cast<int>(ModuleState::Idle));
    CHECK_EQ(static_cast<int>(b.modules[1].reason), static_cast<int>(ModuleReason::NoCredentials));
    CHECK_BYTES_EQ(b.mac, 6, mac, 6);
    CHECK(n <= 226);  // fits one ESP-NOW v1 frame's payload

    // A board from before version 3 answers version 2: no MAC, read as all zero.
    wire[0] = kNodeStatusVersion2;
    NodeStatus c;
    CHECK(load_node_status(wire, n - kNodeStatusMacLen, c));
    CHECK_EQ(static_cast<int>(c.module_count), 2);
    const uint8_t zero[6] = {};
    CHECK_BYTES_EQ(c.mac, 6, zero, 6);
    CHECK(!load_node_status(wire, n, c));  // version 2 with six bytes too many
}

TEST(node_status, the_longest_fits_a_v1_frame_and_bad_input_is_refused) {
    NodeStatus a;
    std::memset(a.cluster, 'c', kNodeStatusMaxCluster);
    std::memset(a.actors, 'a', kNodeStatusMaxActors);
    a.module_count = kNodeStatusMaxModules;
    for (auto& m : a.modules) m = ModuleStatus{ModuleId::Present, ModuleState::Idle, ModuleReason::Standby};
    uint8_t wire[kNodeStatusMaxLen];
    const size_t n = store_node_status(a, wire, sizeof(wire));
    CHECK_EQ(n, kNodeStatusMaxLen);
    CHECK(kNodeStatusMaxLen <= 226);
    CHECK_EQ(store_node_status(a, wire, n - 1), static_cast<size_t>(0));
    NodeStatus b;
    CHECK(load_node_status(wire, n, b));
    CHECK(!load_node_status(wire, n - 1, b));  // the MAC cut short
    wire[0] = 0;  // "unreachable" and unknown versions are not a status
    CHECK(!load_node_status(wire, n, b));
    const uint8_t unreachable[3] = {0, 0x68, 0x73};
    CHECK(!load_node_status(unreachable, sizeof(unreachable), b));
}

TEST(node_status, a_version_1_reply_still_reads_with_no_modules) {
    // M12's boards answer version 1: the fields above, the actors last, nothing after them.
    NodeStatus a;
    std::strcpy(a.cluster, "lab");
    std::strcpy(a.actors, "die_temp");
    a.module_count = 1;
    a.modules[0] = ModuleStatus{ModuleId::Sandbox, ModuleState::Active, ModuleReason::None};
    uint8_t wire[kNodeStatusMaxLen];
    size_t n = store_node_status(a, wire, sizeof(wire));
    wire[0] = kNodeStatusVersion1;
    n -= 1 + 3 + kNodeStatusMacLen;  // drop the module section and the MAC after it
    NodeStatus b;
    CHECK(load_node_status(wire, n, b));
    CHECK_STR_EQ(static_cast<const char*>(b.actors), "die_temp");
    CHECK_EQ(static_cast<int>(b.module_count), 0);
}
