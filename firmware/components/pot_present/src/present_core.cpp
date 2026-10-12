// M13: the presenting role's portable half. See pot/present_core.hpp.

#include "pot/present_core.hpp"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pot::present {

namespace {

// An appender that stops (and remembers it) when the buffer is full, so a builder is a straight line of add()s.
struct Out {
    char* p;
    size_t cap;
    size_t n = 0;
    bool full = false;
    void add(const char* fmt, ...) {
        if (full) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = std::vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) {
            full = true;
            return;
        }
        n += static_cast<size_t>(w);
    }
    size_t done() const { return full ? 0 : n; }
};

// The board a name belongs to: "node-6300/hw/die_temp" -> 4 hex digits in `out`. False for a name that is not a
// board's (a portable actor's output, "act/<actor>/..."): it belongs to the cell, wherever the actor runs.
bool board_of(const Entity& e, char out[5]) {
    if (e.name_len < 10 || std::strncmp(e.name, "node-", 5) != 0 || e.name[9] != '/') return false;
    for (int i = 0; i < 4; ++i) {
        const char ch = e.name[5 + i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        out[i] = ch;
    }
    out[4] = 0;
    return true;
}

bool is_number(ValueType t) {
    return t == ValueType::I32 || t == ValueType::U32 || t == ValueType::F32 || t == ValueType::F64;
}

}  // namespace

Entity entity_of(const NsEntry& e, const char* name, size_t name_len) {
    Entity x;
    x.hash = e.path_hash;
    x.outside = e.outside;
    x.owner = e.owner_node;
    x.type = e.value.type;
    x.unit = e.unit;
    x.policy = static_cast<StalenessPolicy>(e.staleness_policy);
    x.bound_ms = e.staleness_bound_ms;
    x.name = name;
    x.name_len = name_len;
    return x;
}

Component component_of(const Entity& e) {
    const bool writable = (e.outside & kOutsideWritable) != 0 && (e.outside & kOutsideSafety) == 0;
    if (e.type == ValueType::Bool) return writable ? Component::Switch : Component::BinarySensor;
    if (is_number(e.type)) return writable ? Component::Number : Component::Sensor;
    return Component::None;
}

const char* component_str(Component c) {
    switch (c) {
        case Component::Sensor: return "sensor";
        case Component::BinarySensor: return "binary_sensor";
        case Component::Switch: return "switch";
        case Component::Number: return "number";
        case Component::None: break;
    }
    return "none";
}

const char* ha_unit(Unit u) {
    switch (u) {
        case Unit::Percent: return "%";
        case Unit::Metre: return "m";
        case Unit::Millimetre: return "mm";
        case Unit::Degree: return "\xc2\xb0";  // the degree sign, UTF-8
        case Unit::Celsius: return "\xc2\xb0" "C";
        case Unit::Kelvin: return "K";
        case Unit::Pascal: return "Pa";
        case Unit::Volt: return "V";
        case Unit::Millivolt: return "mV";
        case Unit::Ampere: return "A";
        case Unit::Milliampere: return "mA";
        case Unit::Watt: return "W";
        case Unit::Second: return "s";
        case Unit::Millisecond: return "ms";
        case Unit::Microsecond: return "\xc2\xb5" "s";
        case Unit::Hertz: return "Hz";
        case Unit::Byte: return "B";
        case Unit::Newton: return "N";
        default: break;
    }
    return nullptr;
}

const char* ha_device_class(Unit u) {
    switch (u) {
        case Unit::Celsius:
        case Unit::Kelvin: return "temperature";
        case Unit::Volt:
        case Unit::Millivolt: return "voltage";
        case Unit::Ampere:
        case Unit::Milliampere: return "current";
        case Unit::Watt: return "power";
        case Unit::Pascal: return "pressure";
        case Unit::Hertz: return "frequency";
        case Unit::Second:
        case Unit::Millisecond:
        case Unit::Microsecond: return "duration";
        case Unit::Byte: return "data_size";
        case Unit::Metre:
        case Unit::Millimetre: return "distance";
        default: break;
    }
    return nullptr;
}

size_t config_topic(const Cell& c, const Entity& e, char* out, size_t cap) {
    Out o{out, cap};
    o.add("%s/%s/potluck_%s/%08" PRIx32 "/config", c.prefix, component_str(component_of(e)), c.id, e.hash);
    return o.done();
}

size_t state_topic(const Cell& c, uint32_t hash, char* out, size_t cap) {
    Out o{out, cap};
    o.add("potluck/%s/%08" PRIx32 "/state", c.id, hash);
    return o.done();
}

size_t command_topic(const Cell& c, uint32_t hash, char* out, size_t cap) {
    Out o{out, cap};
    o.add("potluck/%s/%08" PRIx32 "/set", c.id, hash);
    return o.done();
}

size_t presenter_topic(const Cell& c, uint16_t node, char* out, size_t cap) {
    Out o{out, cap};
    o.add("potluck/%s/presenter/%04x", c.id, static_cast<unsigned>(node));
    return o.done();
}

size_t event_topic(const Cell& c, char* out, size_t cap) {
    Out o{out, cap};
    o.add("potluck/%s/event", c.id);
    return o.done();
}

size_t command_filter(const Cell& c, char* out, size_t cap) {
    Out o{out, cap};
    o.add("potluck/%s/+/set", c.id);
    return o.done();
}

size_t config_payload(const Cell& c, const Entity& e, uint32_t period_ms, char* out, size_t cap) {
    const Component comp = component_of(e);
    if (comp == Component::None) return 0;
    char board[5] = {};
    const bool on_board = board_of(e, board);
    // The entity's own name: the path after its board's prefix ("hw/die_temp"), or the whole name for the cell's.
    const char* short_name = on_board ? e.name + 10 : e.name;
    const int short_len = static_cast<int>(on_board ? e.name_len - 10 : e.name_len);
    Out o{out, cap};
    o.add("{\"name\":\"%.*s\",\"unique_id\":\"potluck_%s_%08" PRIx32 "\"", short_len, short_name, c.id, e.hash);
    o.add(",\"state_topic\":\"potluck/%s/%08" PRIx32 "/state\"", c.id, e.hash);
    o.add(",\"json_attributes_topic\":\"potluck/%s/%08" PRIx32 "/state\"", c.id, e.hash);
    if (comp == Component::Sensor || comp == Component::Number) {
        o.add(",\"value_template\":\"{{ value_json.value }}\"");
    } else {
        o.add(",\"value_template\":\"{{ 'ON' if value_json.value else 'OFF' }}\"");
    }
    if (comp == Component::Switch || comp == Component::Number) {
        o.add(",\"command_topic\":\"potluck/%s/%08" PRIx32 "/set\"", c.id, e.hash);
    }
    if (comp == Component::Number) {
        const bool integer = e.type == ValueType::I32 || e.type == ValueType::U32;
        o.add(",\"min\":%s,\"max\":1000000,\"step\":%s,\"mode\":\"box\"", e.type == ValueType::U32 ? "0" : "-1000000",
              integer ? "1" : "0.001");
    }
    if (const char* u = ha_unit(e.unit); u != nullptr && comp != Component::Switch && comp != Component::BinarySensor) {
        o.add(",\"unit_of_measurement\":\"%s\"", u);
    }
    if (const char* dc = ha_device_class(e.unit); dc != nullptr && comp == Component::Sensor) {
        o.add(",\"device_class\":\"%s\",\"state_class\":\"measurement\"", dc);
    }
    if (comp == Component::Sensor || comp == Component::BinarySensor) {
        // Seconds: three publication periods, or twice the staleness bound if longer, never under 10 s.
        uint32_t ms = 3 * period_ms;
        if (2 * e.bound_ms > ms) ms = 2 * e.bound_ms;
        uint32_t s = (ms + 999) / 1000;
        if (s < 10) s = 10;
        o.add(",\"expire_after\":%" PRIu32, s);
    }
    o.add(",\"availability\":[");
    for (size_t i = 0; i < c.n_presenters; ++i) {
        o.add("%s{\"topic\":\"potluck/%s/presenter/%04x\",\"value_template\":\"{{ 'online' if value_json.online == 1",
              i ? "," : "", c.id, static_cast<unsigned>(c.presenters[i]));
        if (on_board) o.add(" and '%s' in value_json.nodes", board);
        o.add(" and '%08" PRIx32 "' not in value_json.down else 'offline' }}\"}", e.hash);
    }
    o.add("],\"availability_mode\":\"any\"");
    if (on_board) {
        o.add(",\"device\":{\"identifiers\":[\"potluck_%s_%s\"],\"name\":\"%s node-%s\",\"manufacturer\":\"Potluck\"}", c.id,
              board, c.system, board);
    } else {
        o.add(",\"device\":{\"identifiers\":[\"potluck_%s_cell\"],\"name\":\"%s cell\",\"manufacturer\":\"Potluck\"}", c.id,
              c.system);
    }
    o.add("}");
    return o.done();
}

bool shows_value(const Reading& r, StalenessPolicy p) {
    if (!r.usable()) return false;
    return p != StalenessPolicy::Strict || r.quality == Quality::Good;
}

size_t state_payload(const Reading& r, StalenessPolicy p, uint64_t now_unix_ms, char* out, size_t cap) {
    Out o{out, cap};
    if (!shows_value(r, p)) {
        o.add("{\"value\":null");
    } else if (r.value.type == ValueType::Bool) {
        bool b = false;
        r.value.as_bool(b);
        o.add("{\"value\":%s", b ? "true" : "false");
    } else {
        char v[32];
        r.value.format(v, sizeof(v));
        // JSON has no NaN or infinity: such a reading shows as no value, its quality still saying what it is.
        const bool finite = std::strpbrk(v, "ni") == nullptr;  // "nan", "inf" -- %g's only letters besides e
        o.add("{\"value\":%s", finite ? v : "null");
    }
    const char* u = unit_str(r.unit);
    o.add(",\"unit\":\"%s\",\"quality\":\"%s\",\"age_ms\":%" PRIu32 ",\"latency_class\":%u", u != nullptr ? u : "",
          quality_str(r.quality), r.age_ms, static_cast<unsigned>(r.latency_class));
    if (now_unix_ms != 0 && r.usable()) o.add(",\"sampled_ms\":%" PRIu64, now_unix_ms - r.age_ms);
    o.add("}");
    return o.done();
}

size_t presenter_payload(uint16_t self, const uint16_t* live, size_t n_live, const uint32_t* down, size_t n_down,
                         char* out, size_t cap) {
    Out o{out, cap};
    o.add("{\"online\":1,\"node\":\"%04x\",\"nodes\":[", static_cast<unsigned>(self));
    for (size_t i = 0; i < n_live; ++i) o.add("%s\"%04x\"", i ? "," : "", static_cast<unsigned>(live[i]));
    o.add("],\"down\":[");
    for (size_t i = 0; i < n_down; ++i) o.add("%s\"%08" PRIx32 "\"", i ? "," : "", down[i]);
    o.add("]}");
    return o.done();
}

size_t will_payload(uint16_t self, char* out, size_t cap) {
    Out o{out, cap};
    o.add("{\"online\":0,\"node\":\"%04x\",\"nodes\":[],\"down\":[]}", static_cast<unsigned>(self));
    return o.done();
}

bool parse_command_topic(const Cell& c, const char* topic, size_t len, uint32_t& hash) {
    char want[24];
    const int pl = std::snprintf(want, sizeof(want), "potluck/%s/", c.id);
    if (pl <= 0 || len != static_cast<size_t>(pl) + 8 + 4 || std::strncmp(topic, want, static_cast<size_t>(pl)) != 0 ||
        std::strncmp(topic + pl + 8, "/set", 4) != 0) {
        return false;
    }
    uint32_t h = 0;
    for (int i = 0; i < 8; ++i) {
        const char ch = topic[pl + i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
        else return false;
        h = (h << 4) | d;
    }
    hash = h;
    return h != 0;
}

bool parse_command_value(ValueType t, const char* payload, size_t len, Value& out) {
    char buf[32];
    if (len == 0 || len >= sizeof(buf)) return false;
    std::memcpy(buf, payload, len);
    buf[len] = 0;
    char* end = nullptr;
    switch (t) {
        case ValueType::Bool:
            if (std::strcmp(buf, "ON") == 0) out = Value::of_bool(true);
            else if (std::strcmp(buf, "OFF") == 0) out = Value::of_bool(false);
            else return false;
            return true;
        case ValueType::I32: {
            const long long v = std::strtoll(buf, &end, 10);  // long is 32 bits on the S3 and on Windows
            if (*end != 0 || v < INT32_MIN || v > INT32_MAX) return false;
            out = Value::of_i32(static_cast<int32_t>(v));
            return true;
        }
        case ValueType::U32: {
            if (buf[0] == '-') return false;
            const unsigned long long v = std::strtoull(buf, &end, 10);
            if (*end != 0 || v > UINT32_MAX) return false;
            out = Value::of_u32(static_cast<uint32_t>(v));
            return true;
        }
        case ValueType::F32: {
            const float v = std::strtof(buf, &end);
            if (*end != 0) return false;
            out = Value::of_f32(v);
            return true;
        }
        case ValueType::F64: {
            const double v = std::strtod(buf, &end);
            if (*end != 0) return false;
            out = Value::of_f64(v);
            return true;
        }
        default: break;
    }
    return false;
}

size_t write_event(const char* name, size_t name_len, uint32_t hash, const char* payload, size_t payload_len,
                   const char* result, char* out, size_t cap) {
    // The payload as given, cut at the first character that would need escaping: it is an echo, not data.
    size_t pl = 0;
    while (pl < payload_len && pl < 24 && payload[pl] >= 0x20 && payload[pl] < 0x7F && payload[pl] != '"' &&
           payload[pl] != '\\') {
        ++pl;
    }
    Out o{out, cap};
    o.add("{\"what\":\"outside_write\",\"resource\":\"%.*s\",\"hash\":\"%08" PRIx32 "\",\"value\":\"%.*s\",\"result\":\"%s\"}",
          static_cast<int>(name_len), name, hash, static_cast<int>(pl), payload, result);
    return o.done();
}

}  // namespace pot::present
