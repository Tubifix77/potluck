// READ / WRITE / REPLY wire payloads — see ns_payloads.hpp.

#include "pot/ns_payloads.hpp"

#include <cstring>

#include "pot/opcodes.hpp"

namespace pot {
namespace {

// Same rule as the M0 loaders: a longer payload is accepted and the extra ignored, so a later
// milestone can append a field; a shorter one is refused, because the alternative is reading
// uninitialised stack as though a peer had sent it.
template <typename T>
bool load_fixed(const uint8_t* payload, uint16_t len, T& out) {
    if (payload == nullptr || len < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, payload, sizeof(T));
    return true;
}

}  // namespace

bool load_read(const uint8_t* p, uint16_t len, ReadPayload& out) { return load_fixed(p, len, out); }
bool load_write(const uint8_t* p, uint16_t len, WritePayload& out) { return load_fixed(p, len, out); }
bool load_reply(const uint8_t* p, uint16_t len, ReplyPayload& out) { return load_fixed(p, len, out); }

bool load_call(const uint8_t* p, uint16_t len, CallPayload& out) {
    if (!load_fixed(p, len, out)) {
        return false;
    }
    // The declared argument length must be backed by bytes that actually arrived.
    return static_cast<size_t>(len) >= sizeof(CallPayload) + static_cast<size_t>(out.arg_len);
}

Value value_from_wire(uint8_t type, uint8_t len, const uint8_t* raw) {
    Value v;
    v.type = static_cast<ValueType>(type);
    v.len = (len > kValueBytesMax) ? static_cast<uint8_t>(kValueBytesMax) : len;
    if (raw != nullptr && v.len > 0) {
        std::memcpy(v.raw, raw, v.len);
    }
    return v;
}

void value_to_wire(const Value& v, uint8_t& type, uint8_t& len, uint8_t* raw) {
    type = static_cast<uint8_t>(v.type);
    len = v.len;
    std::memset(raw, 0, kValueBytesMax);
    if (v.len > 0) {
        std::memcpy(raw, v.raw, (v.len > kValueBytesMax) ? kValueBytesMax : v.len);
    }
}

void reply_from_reading(ReplyPayload& out, uint32_t path_hash, const Reading& r, NsError status) {
    std::memset(&out, 0, sizeof(out));
    out.path_hash = path_hash;
    out.timestamp_ms = r.timestamp_ms;
    out.age_ms = r.age_ms;
    out.unit = static_cast<uint16_t>(r.unit);
    out.reply_to = kOpRead;
    out.status = static_cast<uint8_t>(status);
    out.quality = static_cast<uint8_t>(r.quality);
    out.latency_class = r.latency_class;
    // The value travels only when the quality says it may — a strict resource past its bound, or an
    // unavailable one, sends no number at all. Packing it anyway "just in case" would put the exact
    // byte §4 rule 2 forbids onto the wire, where a careless receiver would find it.
    if (quality_has_value(r.quality)) {
        value_to_wire(r.value, out.value_type, out.value_len, out.value_raw);
    }
}

Reading reading_from_reply(const ReplyPayload& p) {
    Reading r;
    r.timestamp_ms = p.timestamp_ms;
    r.age_ms = p.age_ms;
    r.unit = static_cast<Unit>(p.unit);
    r.quality = static_cast<Quality>(p.quality);
    r.latency_class = p.latency_class;
    if (quality_has_value(r.quality)) {
        r.value = value_from_wire(p.value_type, p.value_len, p.value_raw);
    }
    return r;
}

}  // namespace pot
