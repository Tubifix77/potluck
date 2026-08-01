// Typed values — see value.hpp and ARCHITECTURE.md §4 rule 2.

#include "pot/value.hpp"

#include <cstdio>
#include <cstring>

namespace pot {

const char* value_type_str(ValueType t) {
    switch (t) {
        case ValueType::None: return "none";
        case ValueType::Bool: return "bool";
        case ValueType::I32: return "i32";
        case ValueType::U32: return "u32";
        case ValueType::F32: return "f32";
        case ValueType::I64: return "i64";
        case ValueType::U64: return "u64";
        case ValueType::F64: return "f64";
        case ValueType::Bytes: return "bytes";
    }
    return "?";
}

size_t value_type_size(ValueType t) {
    switch (t) {
        case ValueType::Bool: return 1;
        case ValueType::I32:
        case ValueType::U32:
        case ValueType::F32: return 4;
        case ValueType::I64:
        case ValueType::U64:
        case ValueType::F64: return 8;
        case ValueType::None:
        case ValueType::Bytes: return 0;
    }
    return 0;
}

const char* unit_str(Unit u) {
    switch (u) {
        case Unit::None: return "";
        case Unit::Ratio: return "";
        case Unit::Percent: return "%";
        case Unit::Metre: return "m";
        case Unit::Millimetre: return "mm";
        case Unit::Radian: return "rad";
        case Unit::Degree: return "deg";
        case Unit::MetrePerSecond2: return "m/s2";
        case Unit::RadianPerSecond: return "rad/s";
        case Unit::Newton: return "N";
        case Unit::NewtonMetre: return "Nm";
        case Unit::Celsius: return "C";
        case Unit::Kelvin: return "K";
        case Unit::Pascal: return "Pa";
        case Unit::Volt: return "V";
        case Unit::Millivolt: return "mV";
        case Unit::Ampere: return "A";
        case Unit::Milliampere: return "mA";
        case Unit::Watt: return "W";
        case Unit::Second: return "s";
        case Unit::Millisecond: return "ms";
        case Unit::Microsecond: return "us";
        case Unit::Hertz: return "Hz";
        case Unit::Count: return "count";
        case Unit::Byte: return "B";
        case Unit::Raw: return "raw";
    }
    return "?";
}

const char* quality_str(Quality q) {
    switch (q) {
        case Quality::Good: return "GOOD";
        case Quality::Stale: return "STALE";
        case Quality::Unavailable: return "UNAVAILABLE";
        case Quality::NoData: return "NO_DATA";
        case Quality::Faulty: return "FAULTY";
    }
    return "?";
}

const char* staleness_policy_str(StalenessPolicy p) {
    switch (p) {
        case StalenessPolicy::Informative: return "informative";
        case StalenessPolicy::Strict: return "strict";
    }
    return "?";
}

const char* resource_kind_str(ResourceKind k) {
    switch (k) {
        case ResourceKind::Sampled: return "sampled";
        case ResourceKind::Event: return "event";
    }
    return "?";
}

// -------------------------------------------------------------------------------------------
// Value
// -------------------------------------------------------------------------------------------

namespace {
template <typename T>
Value make(ValueType t, T v) {
    Value out;
    out.type = t;
    out.len = static_cast<uint8_t>(sizeof(T));
    std::memcpy(out.raw, &v, sizeof(T));
    return out;
}

template <typename T>
bool take(const Value& val, ValueType want, T& out) {
    if (val.type != want || val.len != sizeof(T)) {
        return false;
    }
    std::memcpy(&out, val.raw, sizeof(T));
    return true;
}
}  // namespace

Value Value::of_bool(bool v) { return make(ValueType::Bool, static_cast<uint8_t>(v ? 1 : 0)); }
Value Value::of_i32(int32_t v) { return make(ValueType::I32, v); }
Value Value::of_u32(uint32_t v) { return make(ValueType::U32, v); }
Value Value::of_f32(float v) { return make(ValueType::F32, v); }
Value Value::of_i64(int64_t v) { return make(ValueType::I64, v); }
Value Value::of_u64(uint64_t v) { return make(ValueType::U64, v); }
Value Value::of_f64(double v) { return make(ValueType::F64, v); }

Value Value::of_bytes(const uint8_t* p, uint8_t n) {
    Value out;
    out.type = ValueType::Bytes;
    out.len = (n > kValueBytesMax) ? static_cast<uint8_t>(kValueBytesMax) : n;
    if (p != nullptr && out.len > 0) {
        std::memcpy(out.raw, p, out.len);
    }
    return out;
}

bool Value::as_bool(bool& out) const {
    uint8_t b = 0;
    if (!take(*this, ValueType::Bool, b)) {
        return false;
    }
    out = b != 0;
    return true;
}
bool Value::as_i32(int32_t& out) const { return take(*this, ValueType::I32, out); }
bool Value::as_u32(uint32_t& out) const { return take(*this, ValueType::U32, out); }
bool Value::as_f32(float& out) const { return take(*this, ValueType::F32, out); }
bool Value::as_i64(int64_t& out) const { return take(*this, ValueType::I64, out); }
bool Value::as_u64(uint64_t& out) const { return take(*this, ValueType::U64, out); }
bool Value::as_f64(double& out) const { return take(*this, ValueType::F64, out); }

size_t Value::format(char* out, size_t cap) const {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    int n = 0;
    switch (type) {
        case ValueType::None: n = std::snprintf(out, cap, "null"); break;
        case ValueType::Bool: {
            bool b = false;
            as_bool(b);
            n = std::snprintf(out, cap, "%s", b ? "true" : "false");
            break;
        }
        case ValueType::I32: {
            int32_t v = 0;
            as_i32(v);
            n = std::snprintf(out, cap, "%ld", static_cast<long>(v));
            break;
        }
        case ValueType::U32: {
            uint32_t v = 0;
            as_u32(v);
            n = std::snprintf(out, cap, "%lu", static_cast<unsigned long>(v));
            break;
        }
        case ValueType::F32: {
            float v = 0;
            as_f32(v);
            n = std::snprintf(out, cap, "%.6g", static_cast<double>(v));
            break;
        }
        case ValueType::I64: {
            int64_t v = 0;
            as_i64(v);
            n = std::snprintf(out, cap, "%lld", static_cast<long long>(v));
            break;
        }
        case ValueType::U64: {
            uint64_t v = 0;
            as_u64(v);
            n = std::snprintf(out, cap, "%llu", static_cast<unsigned long long>(v));
            break;
        }
        case ValueType::F64: {
            double v = 0;
            as_f64(v);
            n = std::snprintf(out, cap, "%.10g", v);
            break;
        }
        case ValueType::Bytes: {
            static const char* hex = "0123456789abcdef";
            size_t w = 0;
            for (uint8_t i = 0; i < len && w + 3 < cap; ++i) {
                out[w++] = hex[raw[i] >> 4];
                out[w++] = hex[raw[i] & 0x0F];
            }
            out[w] = '\0';
            return w;
        }
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : (cap - 1);
}

// -------------------------------------------------------------------------------------------
// §4 rule 2, in one function
// -------------------------------------------------------------------------------------------

Quality classify(uint32_t age_ms, uint32_t staleness_bound_ms, StalenessPolicy policy,
                 bool has_data, bool owner_alive) {
    // Order matters. An unreachable owner outranks everything: whatever is cached, we cannot say it
    // still reflects reality, and §8.2 has already declared the node dead.
    if (!owner_alive) {
        return Quality::Unavailable;
    }
    if (!has_data) {
        // Nothing has ever been written. Distinct from Unavailable — the resource is fine, there is
        // simply nothing to be stale about, and a caller waiting for first data wants to know that.
        return Quality::NoData;
    }
    if (staleness_bound_ms == 0 || age_ms <= staleness_bound_ms) {
        return Quality::Good;
    }
    // Past the bound. §4 rule 2: informative delivers it marked, strict delivers nothing. The one
    // thing neither does is hand it back as Good.
    return (policy == StalenessPolicy::Strict) ? Quality::Unavailable : Quality::Stale;
}

size_t Reading::format(char* out, size_t cap) const {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    char vbuf[40];
    value.format(vbuf, sizeof(vbuf));
    // Every element of §4's tuple, every time: value, unit, timestamp, age, class, quality. Tests
    // assert that none of them can be omitted, because a formatter that drops the age is exactly
    // the silent stale read §4 exists to prevent.
    const int n = std::snprintf(out, cap, "%s%s%s ts=%lu age=%lums L%u %s",
                                quality_has_value(quality) ? vbuf : "-",
                                (quality_has_value(quality) && unit_str(unit)[0]) ? " " : "",
                                quality_has_value(quality) ? unit_str(unit) : "",
                                static_cast<unsigned long>(timestamp_ms),
                                static_cast<unsigned long>(age_ms),
                                static_cast<unsigned>(latency_class), quality_str(quality));
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : (cap - 1);
}

}  // namespace pot
