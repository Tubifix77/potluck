// M0 payload load helpers — see payloads.hpp for the layouts these enforce.

#include "pot/payloads.hpp"

#include <cstring>

namespace pot {
namespace {

// A payload longer than the fixed part is accepted and the extra ignored: that is how a v1 node
// keeps working when a later milestone appends a field. A payload *shorter* than the fixed part is
// refused, because the alternative is reading uninitialised stack as though a peer had sent it.
template <typename T>
bool load_fixed(const uint8_t* payload, uint16_t len, T& out) {
    if (payload == nullptr || len < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, payload, sizeof(T));
    return true;
}

}  // namespace

bool load_hello(const uint8_t* payload, uint16_t len, HelloPayload& out) {
    return load_fixed(payload, len, out);
}

bool load_hello_ack(const uint8_t* payload, uint16_t len, HelloAckPayload& out) {
    return load_fixed(payload, len, out);
}

bool load_heartbeat(const uint8_t* payload, uint16_t len, HeartbeatPayload& out) {
    return load_fixed(payload, len, out);
}

bool load_bye(const uint8_t* payload, uint16_t len, ByePayload& out) {
    return load_fixed(payload, len, out);
}

bool load_err(const uint8_t* payload, uint16_t len, ErrPayload& out) {
    return load_fixed(payload, len, out);
}

uint16_t quantise_us_d8(uint32_t us) {
    const uint32_t d8 = us >> 3;
    // Clamp one below the sentinel so "no sample yet" stays distinguishable from a very slow one.
    // A clamped sample is a lie of 0.5 ms at 524 ms, which the histogram records exactly anyway.
    if (d8 >= kRttUnknownD8) {
        return kRttUnknownD8 - 1;
    }
    return static_cast<uint16_t>(d8);
}

uint32_t dequantise_us_d8(uint16_t d8) {
    if (d8 == kRttUnknownD8) {
        return 0;
    }
    return static_cast<uint32_t>(d8) << 3;
}

}  // namespace pot
