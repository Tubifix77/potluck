// Namespace and typed-value tests — ARCHITECTURE.md §4 rule 2, §7.2, §6.
//
// §4 rule 2 is the central architectural idea, and it is a rule about what a read is *forbidden*
// from doing. So most of these tests assert absences: that a stale value cannot arrive unmarked,
// that a strict resource yields no number at all, that a dead owner outranks a fresh cache, and
// that the formatted output can never lose the age.

#include <cstring>
#include <string>

#include "pot/namespace.hpp"
#include "pot/value.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

constexpr uint16_t kSelf = 0x0101;
constexpr uint16_t kOther = 0x0202;

NsDecl sampled(uint32_t hash, uint16_t owner, uint32_t bound_ms,
               StalenessPolicy policy = StalenessPolicy::Informative) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = owner;
    d.type = ValueType::F32;
    d.unit = Unit::Celsius;
    d.kind = ResourceKind::Sampled;
    d.access = Access::ReadWrite;
    d.latency_class = kClassL3;
    d.staleness_bound_ms = bound_ms;
    d.staleness_policy = policy;
    return d;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// §4 rule 2 — the whole point
// ------------------------------------------------------------------------------------------

TEST(ns, a_fresh_read_is_good_and_carries_the_whole_tuple) {
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-07/adc/1");
    CHECK_EQ(static_cast<int>(ns.declare(sampled(h, kSelf, 500))), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(ns.write_local(h, Value::of_f32(21.5f), 1000)),
             static_cast<int>(NsError::Ok));

    Reading r;
    CHECK_EQ(static_cast<int>(ns.read(h, 1100, true, r)), static_cast<int>(NsError::Ok));
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    CHECK_EQ(r.age_ms, 100u);
    CHECK_EQ(r.timestamp_ms, 1000u);
    CHECK_EQ(static_cast<int>(r.unit), static_cast<int>(Unit::Celsius));
    CHECK_EQ(static_cast<uint32_t>(r.latency_class), static_cast<uint32_t>(kClassL3));

    float v = 0;
    Quality q = Quality::NoData;
    CHECK(r.get_f32(v, q));
    CHECK(v > 21.4f && v < 21.6f);
    CHECK_EQ(static_cast<int>(q), static_cast<int>(Quality::Good));
}

TEST(ns, past_the_bound_the_value_is_delivered_but_marked_stale) {
    // §4: "Past the resource's staleness bound, quality becomes STALE and the value is *still
    // delivered* — an estimator predicting through a dropout legitimately wants the last
    // measurement and its exact age."
    Namespace ns;
    const uint32_t h = path_hash("potluck://home/garden/moisture/0");
    ns.declare(sampled(h, kSelf, 500));
    ns.write_local(h, Value::of_f32(0.42f), 1000);

    Reading r;
    ns.read(h, 1500, true, r);  // exactly at the bound
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));

    ns.read(h, 1501, true, r);  // one millisecond past it
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Stale));
    CHECK_EQ(r.age_ms, 501u);

    // The value is still there, and so is its exact age. That is the contract.
    float v = 0;
    Quality q = Quality::NoData;
    CHECK(r.get_f32(v, q));
    CHECK(v > 0.41f && v < 0.43f);
    CHECK_EQ(static_cast<int>(q), static_cast<int>(Quality::Stale));
}

TEST(ns, a_strict_resource_delivers_no_value_at_all_past_its_bound) {
    // §4: strict is "for feedback where consuming old data is worse than none".
    Namespace ns;
    const uint32_t h = path_hash("potluck://suit/knee-left/current-limit");
    ns.declare(sampled(h, kSelf, 100, StalenessPolicy::Strict));
    ns.write_local(h, Value::of_f32(3.5f), 1000);

    Reading r;
    ns.read(h, 1050, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));

    ns.read(h, 1200, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Unavailable));
    CHECK(!r.usable());
    // No number leaks out, not even a stale one.
    CHECK_EQ(static_cast<int>(r.value.type), static_cast<int>(ValueType::None));
    float v = -1;
    Quality q = Quality::Good;
    CHECK(!r.get_f32(v, q));
}

TEST(ns, a_dead_owner_outranks_a_fresh_cache) {
    // The M1 acceptance test: "Unplug node 2 — the read returns STALE, never a cached number
    // presented as fresh." A value cached a millisecond ago from a node that §8.2 has since
    // declared dead is not evidence about the world now.
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-02/adc/0");
    ns.declare(sampled(h, kOther, 5000));
    ns.apply_remote(h, Value::of_f32(1.0f), 900, 1000, false);

    Reading r;
    ns.read(h, 1001, /*owner_alive=*/true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));

    ns.read(h, 1001, /*owner_alive=*/false, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Unavailable));
    CHECK(!r.usable());
    CHECK_EQ(static_cast<int>(r.value.type), static_cast<int>(ValueType::None));
}

TEST(ns, never_written_is_no_data_not_stale) {
    // Distinct states: the resource is fine, there is simply nothing to be stale about. A caller
    // waiting for first data needs to tell that from a dropout.
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-07/adc/2");
    ns.declare(sampled(h, kSelf, 100));

    Reading r;
    ns.read(h, 999999, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::NoData));
    CHECK_EQ(r.age_ms, 0u);  // no age, because there is no sample to be old
    CHECK(!r.usable());
}

TEST(ns, a_bound_of_zero_never_goes_stale) {
    // A constant or a configuration value has no staleness. Modelled as bound 0 rather than as a
    // separate flag, so there is one code path.
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-07/config/gain");
    ns.declare(sampled(h, kSelf, 0));
    ns.write_local(h, Value::of_f32(2.0f), 0);

    Reading r;
    ns.read(h, 100000000u, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
    CHECK_EQ(r.age_ms, 100000000u);  // ancient, and correct, and not stale
}

TEST(ns, the_formatted_reading_can_never_lose_the_age_or_the_quality) {
    // A formatter that drops the age is the silent stale read §4 exists to prevent, wearing a
    // different hat. Every rendering carries the whole tuple.
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/n2/adc/0");
    ns.declare(sampled(h, kSelf, 100));
    ns.write_local(h, Value::of_f32(3.25f), 1000);

    char buf[128];
    Reading r;
    for (uint32_t at : {1050u, 1500u}) {
        ns.read(h, at, true, r);
        r.format(buf, sizeof(buf));
        const std::string s(buf);
        CHECK(s.find("age=") != std::string::npos);
        CHECK(s.find("ts=") != std::string::npos);
        CHECK(s.find("L3") != std::string::npos);
        CHECK(s.find("C") != std::string::npos);  // the unit
        CHECK(s.find(at == 1050u ? "GOOD" : "STALE") != std::string::npos);
    }

    // And an unusable reading still says so rather than rendering a stale number.
    ns.read(h, 1500, false, r);
    r.format(buf, sizeof(buf));
    CHECK(std::string(buf).find("UNAVAILABLE") != std::string::npos);
}

TEST(ns, classify_is_the_only_place_the_rule_lives) {
    // Exhaustive over the decision, because every other path defers to it.
    CHECK_EQ(static_cast<int>(classify(0, 100, StalenessPolicy::Informative, false, true)),
             static_cast<int>(Quality::NoData));
    CHECK_EQ(static_cast<int>(classify(0, 100, StalenessPolicy::Informative, true, false)),
             static_cast<int>(Quality::Unavailable));
    // A dead owner outranks even "no data".
    CHECK_EQ(static_cast<int>(classify(0, 100, StalenessPolicy::Informative, false, false)),
             static_cast<int>(Quality::Unavailable));
    CHECK_EQ(static_cast<int>(classify(100, 100, StalenessPolicy::Informative, true, true)),
             static_cast<int>(Quality::Good));
    CHECK_EQ(static_cast<int>(classify(101, 100, StalenessPolicy::Informative, true, true)),
             static_cast<int>(Quality::Stale));
    CHECK_EQ(static_cast<int>(classify(101, 100, StalenessPolicy::Strict, true, true)),
             static_cast<int>(Quality::Unavailable));
    CHECK_EQ(static_cast<int>(classify(999999, 0, StalenessPolicy::Strict, true, true)),
             static_cast<int>(Quality::Good));
}

// ------------------------------------------------------------------------------------------
// §7.2 — kinds, access, ownership
// ------------------------------------------------------------------------------------------

TEST(ns, an_event_resource_refuses_a_cached_read) {
    // §7.2: "Conflating the two kinds is how a click gets eaten by a cache."
    Namespace ns;
    NsDecl d = sampled(path_hash("potluck://fun/pi/kbd/events"), kSelf, 0);
    d.kind = ResourceKind::Event;
    d.type = ValueType::U32;
    CHECK_EQ(static_cast<int>(ns.declare(d)), static_cast<int>(NsError::Ok));

    Reading r;
    CHECK_EQ(static_cast<int>(ns.read(d.path_hash, 0, true, r)),
             static_cast<int>(NsError::EventNotCached));
}

TEST(ns, access_is_enforced_in_both_directions) {
    Namespace ns;
    NsDecl ro = sampled(path_hash("potluck://lab/n1/temp"), kSelf, 0);
    ro.access = Access::Read;
    ns.declare(ro);
    CHECK_EQ(static_cast<int>(ns.write_local(ro.path_hash, Value::of_f32(1.0f), 0)),
             static_cast<int>(NsError::NotWritable));

    NsDecl wo = sampled(path_hash("potluck://lab/n1/pwm"), kSelf, 0);
    wo.access = Access::Write;
    ns.declare(wo);
    Reading r;
    CHECK_EQ(static_cast<int>(ns.read(wo.path_hash, 0, true, r)),
             static_cast<int>(NsError::NotReadable));
}

TEST(ns, a_write_of_the_wrong_type_is_refused_not_reinterpreted) {
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/n1/angle");
    ns.declare(sampled(h, kSelf, 0));  // declared F32
    CHECK_EQ(static_cast<int>(ns.write_local(h, Value::of_i32(42), 0)),
             static_cast<int>(NsError::TypeMismatch));
    CHECK_EQ(static_cast<int>(ns.write_local(h, Value::of_f32(42.0f), 0)),
             static_cast<int>(NsError::Ok));
}

TEST(ns, redeclaring_the_same_path_is_idempotent_but_a_type_change_is_not) {
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/n1/x");
    CHECK_EQ(static_cast<int>(ns.declare(sampled(h, kSelf, 100))), static_cast<int>(NsError::Ok));
    ns.write_local(h, Value::of_f32(1.0f), 10);

    NsDecl again = sampled(h, kSelf, 250);
    CHECK_EQ(static_cast<int>(ns.declare(again)), static_cast<int>(NsError::Ok));
    CHECK_EQ(ns.find(h)->staleness_bound_ms, 250u);
    CHECK_EQ(ns.find(h)->update_count, 1u);  // the cached value survived a manifest reload
    CHECK_EQ(ns.count(), static_cast<size_t>(1));

    NsDecl retyped = sampled(h, kSelf, 250);
    retyped.type = ValueType::I32;
    CHECK_EQ(static_cast<int>(ns.declare(retyped)), static_cast<int>(NsError::TypeMismatch));
}

TEST(ns, a_remote_value_ages_from_arrival_not_from_the_owners_clock) {
    // The two clocks are unsynchronised — the same reason M0's RTT machinery reports durations
    // rather than instants. Ageing from the owner's timestamp would subtract one clock from
    // another and produce an offset, not an age. Arrival is conservative: never younger than true.
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-02/imu/0/accel");
    ns.declare(sampled(h, kOther, 1000));

    // The owner's clock reads 500,000; ours reads 1,000. Both are legitimate.
    ns.apply_remote(h, Value::of_f32(9.81f), /*sampled_ms=*/500000, /*local_now_ms=*/1000, false);

    Reading r;
    ns.read(h, 1200, true, r);
    CHECK_EQ(r.age_ms, 200u);           // 200 ms since it reached us
    CHECK_EQ(r.timestamp_ms, 500000u);  // the owner's stamp, carried through untouched
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Good));
}

TEST(ns, a_faulty_sensor_is_reported_as_faulty_not_as_good) {
    Namespace ns;
    const uint32_t h = path_hash("potluck://lab/node-02/temp");
    ns.declare(sampled(h, kOther, 5000));
    ns.apply_remote(h, Value::of_f32(-999.0f), 100, 100, /*faulty=*/true);

    Reading r;
    ns.read(h, 150, true, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Faulty));
    CHECK(!r.usable());
}

// ------------------------------------------------------------------------------------------
// §6 and §14 — the table itself
// ------------------------------------------------------------------------------------------

TEST(ns, the_table_fits_section_6s_allocation) {
    CHECK_EQ(Namespace::capacity(), static_cast<size_t>(128));
    CHECK_EQ(sizeof(NsEntry), static_cast<size_t>(40));
    CHECK(sizeof(NsEntry) * kNamespaceEntries <= 8192);  // §6: 8.0 KB
}

TEST(ns, a_full_table_refuses_rather_than_overwrites) {
    Namespace ns;
    char path[64];
    size_t placed = 0;
    for (size_t i = 0; i < kNamespaceEntries * 2; ++i) {
        std::snprintf(path, sizeof(path), "potluck://lab/n%zu/adc/0", i);
        const NsError e = ns.declare(sampled(path_hash(path), kSelf, 100));
        if (e == NsError::Ok) {
            ++placed;
        } else {
            CHECK_EQ(static_cast<int>(e), static_cast<int>(NsError::Full));
            break;
        }
    }
    CHECK_EQ(placed, kNamespaceEntries);
    CHECK_EQ(ns.count(), kNamespaceEntries);
}

TEST(ns, lookup_cost_is_bounded_at_a_realistic_occupancy) {
    // §14 lists namespace lookup cost as a risk that "reduces node count". §15's open question 2
    // asks whether 128 entries is right. This measures the probe distance rather than assuming it.
    Namespace ns;
    char path[64];
    const size_t n = kNamespaceEntries * 3 / 4;  // 75% load, a realistic manifest
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(path, sizeof(path), "potluck://home/room%zu/vent/position", i);
        ns.declare(sampled(path_hash(path), kSelf, 100));
    }
    CHECK_EQ(ns.count(), n);

    // Every one is still findable, which is the property that matters most.
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(path, sizeof(path), "potluck://home/room%zu/vent/position", i);
        CHECK(ns.find(path_hash(path)) != nullptr);
    }
    // And no probe runs away. A linear-probe table at 75% has an expected worst case in the low
    // tens; anything near the table size would mean the hash is not spreading.
    CHECK(ns.worst_probe() < 32);
}

TEST(ns, a_missing_path_is_not_found_rather_than_a_wrong_answer) {
    Namespace ns;
    ns.declare(sampled(path_hash("potluck://lab/n1/a"), kSelf, 100));
    Reading r;
    CHECK_EQ(static_cast<int>(ns.read(path_hash("potluck://lab/n1/b"), 0, true, r)),
             static_cast<int>(NsError::NotFound));
    CHECK(ns.find(0) == nullptr);  // 0 is the free marker, never a valid path
}

TEST(ns, the_path_hash_is_stable_and_specified) {
    // The host computes this from a string; the node only ever sees the number. If the two ever
    // disagree, every read misses. These are FNV-1a/32 reference values, and
    // host/potluck/tests checks the Python implementation against the same ones.
    CHECK_EQ(path_hash(""), 2166136261u);
    CHECK_EQ(path_hash("a"), 3826002220u);

    // The NUL-terminated and explicit-length overloads must agree. The length is taken from the
    // literal rather than written out: it was once the number 18, which was correct only while the
    // scheme was `dmu://`, and the rename to `potluck://` turned a passing test into a failing one
    // for no reason connected to what it tests.
    constexpr char kPath[] = "potluck://lab/n2/adc/0";
    CHECK_EQ(path_hash(kPath), path_hash_n(kPath, sizeof(kPath) - 1));
    CHECK(path_hash("potluck://lab/n2/adc/0") != path_hash("potluck://lab/n2/adc/1"));
}

// ------------------------------------------------------------------------------------------
// Value
// ------------------------------------------------------------------------------------------

TEST(ns, value_round_trips_every_type_and_refuses_mismatches) {
    Value v = Value::of_f32(1.5f);
    float f = 0;
    int32_t i = 0;
    CHECK(v.as_f32(f));
    CHECK(f > 1.49f && f < 1.51f);
    CHECK(!v.as_i32(i));  // a float read as an integer is a bug, not a cast

    bool b = false;
    CHECK(Value::of_bool(true).as_bool(b));
    CHECK(b);

    int64_t i64 = 0;
    CHECK(Value::of_i64(-9000000000LL).as_i64(i64));
    CHECK_EQ(i64, -9000000000LL);

    uint32_t u = 0;
    CHECK(Value::of_u32(4000000000u).as_u32(u));
    CHECK_EQ(u, 4000000000u);

    CHECK_EQ(sizeof(Value), static_cast<size_t>(10));
}

TEST(ns, value_formats_without_a_locale_or_an_allocation) {
    char buf[40];
    Value::of_i32(-42).format(buf, sizeof(buf));
    CHECK_STR_EQ(std::string(buf), std::string("-42"));
    Value::of_bool(false).format(buf, sizeof(buf));
    CHECK_STR_EQ(std::string(buf), std::string("false"));
    Value::of_u32(7).format(buf, sizeof(buf));
    CHECK_STR_EQ(std::string(buf), std::string("7"));

    const uint8_t bytes[3] = {0xDE, 0xAD, 0x01};
    Value::of_bytes(bytes, 3).format(buf, sizeof(buf));
    CHECK_STR_EQ(std::string(buf), std::string("dead01"));

    // A byte value longer than the inline capacity is truncated, not overflowed.
    const uint8_t big[16] = {};
    CHECK_EQ(static_cast<uint32_t>(Value::of_bytes(big, 16).len),
             static_cast<uint32_t>(kValueBytesMax));
}
