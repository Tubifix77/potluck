// M24: events from the PC -- key and pointer -- as the hub delivers them to an app's input.
//
// An app whose module descriptor says "takes": ["key"] (or "pointer", or both) receives the events the hub is given for
// it (POST /api/input) as CALLs of act/<actor>/in, from the PC's node, one batch at a time: the next batch only after the
// actor answered the last. Each event carries a 16-bit number the hub gives it, counting up from where the hub started;
// a batch the hub did not hear answered is sent again, so an actor drops an event it already took (parse_input and
// InputOrder below do this). The actor answers with reply_call(..., Value::of_u32(last number taken)).
//
//   args: version u8 (1), count u8 (1..kInputBatchMax), then count x 8 bytes:
//     seq u16, kind u8 (kInputKey | kInputPointer), flags u8, a i16/u16, b i16/u16
//     key:     flags bit 0 down (else up), a = key code (USB HID usage, page 7), b = 0
//     pointer: flags = buttons held (bit 0 left, 1 right, 2 middle), a = dx, b = dy (pixels, signed)
//
// Portable: no ESP-IDF, no heap.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kInputVersion = 1;
constexpr uint8_t kInputKey = 1, kInputPointer = 2;
constexpr size_t kInputEventBytes = 8;
constexpr uint8_t kInputBatchMax = 24;  // 2 + 24 x 8 = 194 B of CALL arguments: inside the radio's 218 (kMaxCallArgsV1)

struct InputEvent {
    uint16_t seq;
    uint8_t kind;
    uint8_t flags;
    int16_t a;
    int16_t b;
};

// The batch's events into `out` (at most `cap`); the count, or -1 if the arguments are not a version-1 batch.
inline int parse_input(const uint8_t* args, uint16_t len, InputEvent* out, size_t cap) {
    if (args == nullptr || len < 2 || args[0] != kInputVersion) return -1;
    const uint8_t n = args[1];
    if (n == 0 || n > kInputBatchMax || len != 2 + n * kInputEventBytes || n > cap) return -1;
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t* e = args + 2 + i * kInputEventBytes;
        out[i].seq = static_cast<uint16_t>(e[0] | (e[1] << 8));
        out[i].kind = e[2];
        out[i].flags = e[3];
        out[i].a = static_cast<int16_t>(e[4] | (e[5] << 8));
        out[i].b = static_cast<int16_t>(e[6] | (e[7] << 8));
    }
    return n;
}

// Taking events in order: true for an event to act on, false for one already taken (a resent batch). An event past the
// next expected number counts the ones skipped as missed -- the hub never skips one, so a gap is a fault to show.
struct InputOrder {
    bool started = false;
    uint16_t last = 0;
    uint32_t taken = 0, dropped = 0, missed = 0;

    bool take(uint16_t seq) {
        if (started) {
            const uint16_t ahead = static_cast<uint16_t>(seq - last);  // modulo 2^16
            if (ahead == 0 || ahead >= 0x8000) {
                ++dropped;  // the same number again, or one behind: already taken
                return false;
            }
            missed += ahead - 1u;
        }
        started = true;
        last = seq;
        ++taken;
        return true;
    }
};

}  // namespace pot
