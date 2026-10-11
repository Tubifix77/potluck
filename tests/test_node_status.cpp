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
    uint8_t wire[kNodeStatusMaxLen];
    const size_t n = store_node_status(a, wire, sizeof(wire));
    CHECK_EQ(n, kNodeStatusFixedLen + 1 + 4 + 1 + std::strlen(a.actors));
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
    CHECK(n <= 226);  // fits one ESP-NOW v1 frame's payload
}

TEST(node_status, the_longest_fits_a_v1_frame_and_bad_input_is_refused) {
    NodeStatus a;
    std::memset(a.cluster, 'c', kNodeStatusMaxCluster);
    std::memset(a.actors, 'a', kNodeStatusMaxActors);
    uint8_t wire[kNodeStatusMaxLen];
    const size_t n = store_node_status(a, wire, sizeof(wire));
    CHECK_EQ(n, kNodeStatusMaxLen);
    CHECK(kNodeStatusMaxLen <= 226);
    CHECK_EQ(store_node_status(a, wire, n - 1), static_cast<size_t>(0));
    NodeStatus b;
    CHECK(load_node_status(wire, n, b));
    CHECK(!load_node_status(wire, n - 1, b));  // actors cut short
    wire[0] = 0;  // "unreachable" and unknown versions are not a status
    CHECK(!load_node_status(wire, n, b));
    const uint8_t unreachable[3] = {0, 0x68, 0x73};
    CHECK(!load_node_status(unreachable, sizeof(unreachable), b));
}
