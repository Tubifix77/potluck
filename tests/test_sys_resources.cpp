// The built-in node resources — §7.2, and the thing that makes M1 demonstrable on real boards.

#include <cstring>
#include <set>
#include <string>

#include "pot/sys_resources.hpp"
#include "test_harness.hpp"

using namespace pot;

TEST(sysres, every_resource_has_a_distinct_path_and_hash) {
    char a[64], b[64];
    std::set<std::string> paths;
    std::set<uint32_t> hashes;
    for (uint8_t i = 0; i < kSysResourceCount; ++i) {
        const SysResource r = static_cast<SysResource>(i);
        sys_resource_path(a, sizeof(a), 0x1a2b, r);
        paths.insert(a);
        hashes.insert(sys_resource_hash(0x1a2b, r));
        // No resource may fall through to the "?" default — an unnamed one would be unreadable
        // by any host tool.
        CHECK(std::strstr(sys_resource_suffix(r), "?") == nullptr);
    }
    CHECK_EQ(paths.size(), static_cast<size_t>(kSysResourceCount));
    CHECK_EQ(hashes.size(), static_cast<size_t>(kSysResourceCount));

    sys_resource_path(a, sizeof(a), 0x1a2b, SysResource::HeapFree);
    CHECK_STR_EQ(std::string(a), std::string("potluck://lab/node-1a2b/sys/heap-free"));

    // Two different nodes own different resources at the same suffix.
    sys_resource_path(b, sizeof(b), 0x0002, SysResource::HeapFree);
    CHECK(std::string(a) != std::string(b));
    CHECK(sys_resource_hash(0x1a2b, SysResource::HeapFree) !=
          sys_resource_hash(0x0002, SysResource::HeapFree));
}

TEST(sysres, they_declare_cleanly_and_leave_room_for_the_application) {
    Namespace ns;
    CHECK_EQ(declare_sys_resources(ns, 0x0101), static_cast<size_t>(kSysResourceCount));
    CHECK_EQ(ns.count(), static_cast<size_t>(kSysResourceCount));
    // §6 caps the table at 128; the built-ins must not crowd out what the node is actually for.
    CHECK(kSysResourceCount < Namespace::capacity() / 8);

    // Declaring twice is idempotent, so a restart or a manifest reload is not a special case.
    CHECK_EQ(declare_sys_resources(ns, 0x0101), static_cast<size_t>(kSysResourceCount));
    CHECK_EQ(ns.count(), static_cast<size_t>(kSysResourceCount));
}

TEST(sysres, a_boot_epoch_never_goes_stale_but_free_heap_does) {
    // §4 rule 2 is only as good as the bounds: a resource whose bound far exceeds its true rate of
    // change reports GOOD long after it stopped being true.
    Namespace ns;
    const uint16_t me = 0x0101;
    declare_sys_resources(ns, me);

    const uint32_t epoch_h = sys_resource_hash(me, SysResource::BootEpoch);
    const uint32_t heap_h = sys_resource_hash(me, SysResource::HeapFree);
    CHECK_EQ(ns.find(epoch_h)->staleness_bound_ms, 0u);
    CHECK(ns.find(heap_h)->staleness_bound_ms > 0u);

    // publish(), not write_local(): the owner is not a client of its own access policy.
    CHECK_EQ(static_cast<int>(ns.publish(epoch_h, Value::of_u32(7), 0)),
             static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(ns.publish(heap_h, Value::of_u32(200000), 0)),
             static_cast<int>(NsError::Ok));

    Reading r;
    ns.read(epoch_h, 3600u * 1000u, true, r);  // an hour later
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    ns.read(heap_h, 3600u * 1000u, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Stale));
}

TEST(sysres, all_are_read_only_and_L4) {
    // Nothing here is a setpoint, and nothing with a deadline should be reading a peer's heap.
    Namespace ns;
    declare_sys_resources(ns, 0x0101);
    for (uint8_t i = 0; i < kSysResourceCount; ++i) {
        const NsDecl d = sys_resource_decl(0x0101, static_cast<SysResource>(i));
        CHECK_EQ(static_cast<int>(d.access), static_cast<int>(Access::Read));
        CHECK_EQ(static_cast<uint32_t>(d.latency_class), static_cast<uint32_t>(kClassL4));
        CHECK_EQ(static_cast<int>(d.kind), static_cast<int>(ResourceKind::Sampled));
        CHECK(d.type != ValueType::None);
    }
    // And a write is refused, not silently accepted.
    CHECK_EQ(static_cast<int>(ns.write_local(sys_resource_hash(0x0101, SysResource::HeapFree),
                                             Value::of_u32(1), 0)),
             static_cast<int>(NsError::NotWritable));
}

TEST(sysres, uptime_is_seconds_so_it_survives_a_long_deployment) {
    // uint32 milliseconds wraps in 49.7 days and §1.2 expects nodes to run indefinitely. Seconds
    // gives 136 years, which is long enough.
    const NsDecl d = sys_resource_decl(1, SysResource::UptimeSeconds);
    CHECK_EQ(static_cast<int>(d.unit), static_cast<int>(Unit::Second));
    const uint64_t wrap_years = (uint64_t{1} << 32) / (365ull * 86400ull);
    CHECK(wrap_years > 100);
}
