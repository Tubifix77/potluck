// M24: events from the PC -- key and pointer -- as the hub delivers them to an app's input.
//
// An app whose module descriptor says "takes": ["key"] (or "pointer", or both) receives the events the hub is given for
// it (POST /api/input) as CALLs of act/<actor>/in, through the board on the PC's cable (ns_payloads.hpp kCallForward),
// one batch at a time: the next batch only after the
// actor answered the last. Each event carries a 16-bit number the hub gives it, counting up within a stream -- a number
// the hub picks at random when it starts the app's queue, so numbers from a restarted hub are never mistaken for ones
// already taken (found 2026-10-25: after a hub restart every event of a 1,200-key run was dropped as already taken, and
// answered as taken). A batch the hub did not hear answered is sent again, so an actor drops an event it already took
// (parse_input and InputOrder below do this). The actor answers with reply_call(..., Value::of_u32(last number taken)).
//
//   args: version u8 (2), count u8 (1..kInputBatchMax), stream u16, then count x 8 bytes:
//     seq u16, kind u8 (kInputKey | kInputPointer), flags u8, a i16/u16, b i16/u16
//     key:     flags bit 0 down (else up), a = key code (USB HID usage, page 7), b = 0
//     pointer: flags = buttons held (bit 0 left, 1 right, 2 middle), a = dx, b = dy (pixels, signed)
//
// Portable: no ESP-IDF, no heap.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kInputVersion = 2;
constexpr uint8_t kInputKey = 1, kInputPointer = 2;
constexpr size_t kInputEventBytes = 8;
constexpr uint8_t kInputBatchMax = 24;  // 4 + 24 x 8 = 196 B (+2 passed on): inside the radio's 218 (kMaxCallArgsV1)

struct InputEvent {
    uint16_t seq;
    uint8_t kind;
    uint8_t flags;
    int16_t a;
    int16_t b;
};

// The batch's events into `out` (at most `cap`) and its stream; the count, or -1 if the arguments are not a batch of
// this version.
inline int parse_input(const uint8_t* args, uint16_t len, InputEvent* out, size_t cap, uint16_t* stream) {
    if (args == nullptr || len < 4 || args[0] != kInputVersion) return -1;
    const uint8_t n = args[1];
    if (n == 0 || n > kInputBatchMax || len != 4 + n * kInputEventBytes || n > cap) return -1;
    if (stream != nullptr) *stream = static_cast<uint16_t>(args[2] | (args[3] << 8));
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t* e = args + 4 + i * kInputEventBytes;
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
    uint16_t stream = 0;
    uint16_t last = 0;
    uint32_t taken = 0, dropped = 0, missed = 0;

    bool take(uint16_t from_stream, uint16_t seq) {
        if (started && from_stream != stream) started = false;  // a new stream (a restarted hub): its numbers start over
        stream = from_stream;
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
