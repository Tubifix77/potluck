// The typed value a read returns — ARCHITECTURE.md §4 rule 2.
//
// §4 rule 2 is the central idea in the whole system, so it gets its own type rather than being a
// convention callers are trusted to follow:
//
//   "A read never returns a bare value — and never a silent one. Every read yields
//    (value, unit, timestamp, age, class, quality). Past the resource's staleness bound, quality
//    becomes STALE and the value is *still delivered* — an estimator predicting through a dropout
//    legitimately wants the last measurement and its exact age. Each resource declares a
//    staleness_policy: informative (default: deliver, marked) or strict (deliver an error, no
//    value — for feedback where consuming old data is worse than none). The one banned act is
//    handing back old data *unmarked*: a location-transparent read that silently serves a 400 ms-old
//    sensor value is how distributed control systems hurt people."
//
// There is deliberately no way to get the number on its own. `Reading` has no implicit conversion,
// no `operator T()`, and no `value()` that does not also hand back the quality — because the failure
// this section exists to prevent is exactly someone writing `float x = read(path);` and losing the
// age. If that feels inconvenient, that is the design working.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/frame.hpp"

namespace pot {

// ---------------------------------------------------------------------------------------------
// Types and units. Both are on the wire, so both are fixed numbers rather than strings.
// ---------------------------------------------------------------------------------------------

enum class ValueType : uint8_t {
    None = 0,
    Bool = 1,
    I32 = 2,
    U32 = 3,
    F32 = 4,
    I64 = 5,
    U64 = 6,
    F64 = 7,
    Bytes = 8,  // opaque, length-delimited
};

const char* value_type_str(ValueType t);
size_t value_type_size(ValueType t);  // 0 for None and Bytes

// SI-ish unit codes. A unit is part of the type (§4), so a millimetre and a metre are not the same
// resource and the toolchain can say so. Deliberately small and extensible by appending — never
// renumber, these are on the wire.
enum class Unit : uint16_t {
    None = 0,
    Ratio = 1,        // dimensionless 0..1
    Percent = 2,
    Metre = 10,
    Millimetre = 11,
    Radian = 20,
    Degree = 21,
    MetrePerSecond2 = 30,
    RadianPerSecond = 31,
    Newton = 40,
    NewtonMetre = 41,
    Celsius = 50,
    Kelvin = 51,
    Pascal = 60,
    Volt = 70,
    Millivolt = 71,
    Ampere = 72,
    Milliampere = 73,
    Watt = 74,
    Second = 80,
    Millisecond = 81,
    Microsecond = 82,
    Hertz = 90,
    Count = 100,
    Byte = 101,
    Raw = 200,  // an ADC count or a PWM duty: a number with no physical unit yet
};

const char* unit_str(Unit u);

// ---------------------------------------------------------------------------------------------
// Quality — the part §4 forbids dropping.
// ---------------------------------------------------------------------------------------------

enum class Quality : uint8_t {
    // The value is within its staleness bound and came from the owning node.
    Good = 0,
    // Past the staleness bound. The value is still delivered, with its exact age, because "an
    // estimator predicting through a dropout legitimately wants the last measurement and its exact
    // age". Under `strict` policy the caller gets Unavailable instead and no value at all.
    Stale = 1,
    // The owning node is dead (§8.2) or the path is unreachable. No value.
    Unavailable = 2,
    // The path exists but has never been written, so there is nothing to be stale about. Distinct
    // from Unavailable: the resource is fine, it just has no reading yet.
    NoData = 3,
    // The owning node reported the sensor itself as faulty.
    Faulty = 4,
};

const char* quality_str(Quality q);

// True when a Reading carries a usable number. Stale is deliberately *included* — that is the whole
// point of §4 rule 2 — so anything refusing stale data must say so explicitly rather than relying
// on this.
constexpr bool quality_has_value(Quality q) { return q == Quality::Good || q == Quality::Stale; }

// §4 rule 2's per-resource policy. Mechanism in the OS, policy in the manifest.
enum class StalenessPolicy : uint8_t {
    // Deliver the value past its bound, marked Stale. The default, and right for anything an
    // estimator or a human consumes.
    Informative = 0,
    // Deliver Unavailable and no value. For feedback paths "where consuming old data is worse than
    // none" — a current-limit setpoint, say.
    Strict = 1,
};

const char* staleness_policy_str(StalenessPolicy p);

// §7.2's two resource kinds. Conflating them "is how a click gets eaten by a cache".
enum class ResourceKind : uint8_t {
    Sampled = 0,  // last-value semantics: cache, staleness bound, STALE marking
    Event = 1,    // queue semantics: every publication delivered in order; staleness does not apply
};

const char* resource_kind_str(ResourceKind k);

enum class Access : uint8_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
};

// ---------------------------------------------------------------------------------------------
// The raw payload of a value. Fixed size: §6 budgets the namespace at 64 B per entry, and a
// variable-length value would put the allocator in the read path.
// ---------------------------------------------------------------------------------------------

constexpr size_t kValueBytesMax = 8;

struct Value {
    ValueType type = ValueType::None;
    uint8_t len = 0;  // bytes used in `raw`; for Bytes, the payload length
    uint8_t raw[kValueBytesMax] = {};

    // Typed constructors. Deliberately explicit — a Value is never produced by accident.
    static Value of_bool(bool v);
    static Value of_i32(int32_t v);
    static Value of_u32(uint32_t v);
    static Value of_f32(float v);
    static Value of_i64(int64_t v);
    static Value of_u64(uint64_t v);
    static Value of_f64(double v);
    static Value of_bytes(const uint8_t* p, uint8_t n);

    // Typed accessors. Each returns false on a type mismatch rather than reinterpreting: a
    // temperature read as an integer is a bug, not a cast.
    bool as_bool(bool& out) const;
    bool as_i32(int32_t& out) const;
    bool as_u32(uint32_t& out) const;
    bool as_f32(float& out) const;
    bool as_i64(int64_t& out) const;
    bool as_u64(uint64_t& out) const;
    bool as_f64(double& out) const;

    // Render for a human or a JSON line. Returns bytes written, excluding the NUL.
    size_t format(char* out, size_t cap) const;
};

static_assert(sizeof(Value) == 10, "Value is type + len + 8 payload bytes, no padding games");

// ---------------------------------------------------------------------------------------------
// What a read returns. §4 rule 2's tuple, entire.
// ---------------------------------------------------------------------------------------------

struct Reading {
    Value value{};
    Unit unit = Unit::None;
    uint32_t timestamp_ms = 0;  // when the owning node sampled it, on the owning node's clock
    uint32_t age_ms = 0;        // how old it was when this read was answered
    uint8_t latency_class = kClassL4;  // §4: the class of the binding that produced it
    Quality quality = Quality::NoData;

    // The only way to get the number, and it hands back the quality with it. There is no overload
    // that returns the value alone — see the header comment.
    bool get_f32(float& out, Quality& q) const {
        q = quality;
        return quality_has_value(quality) && value.as_f32(out);
    }
    bool get_i32(int32_t& out, Quality& q) const {
        q = quality;
        return quality_has_value(quality) && value.as_i32(out);
    }

    bool usable() const { return quality_has_value(quality); }

    // One line, always carrying every element of §4's tuple. Used by potctl and the JSON stream,
    // and by the tests that assert the age can never go missing.
    size_t format(char* out, size_t cap) const;
};

// Decide the quality of a cached sample, given how old it is and what the resource declared.
// This is §4 rule 2 in one function, so there is exactly one place the rule can be got wrong.
Quality classify(uint32_t age_ms, uint32_t staleness_bound_ms, StalenessPolicy policy,
                 bool has_data, bool owner_alive);

}  // namespace pot
