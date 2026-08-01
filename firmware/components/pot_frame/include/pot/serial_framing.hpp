// Serial framing for Potluck Frames — ARCHITECTURE.md §5.3.
//
// §5.3's transport table says: "USB-serial / UART | 1446 B | COBS framing + CRC-16; same payload
// cap as ESP-NOW v2 so behaviour matches". This is that framing, and it is what M2's `potluck-bridge`
// speaks.
//
// A serial line has no packet boundaries, so something has to supply them. The two requirements are
// that a receiver joining mid-stream can resynchronise, and that a corrupted frame is rejected
// rather than parsed. COBS gives the first — a zero byte appears nowhere except as the delimiter,
// so the next zero is always a frame boundary — and the CRC gives the second.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS NEEDED WHEN THE RADIO IS NOT
// ---------------------------------------------------------------------------------------------
// ESP-NOW delivers whole frames with its own FCS, so on the radio a Potluck Frame needs no envelope at
// all. A UART delivers a byte stream that can start anywhere and lose bytes silently. §5's promise
// that the *same bytes* travel on every transport is about the Potluck Frame, not about what carries it:
// the COBS envelope and the CRC are stripped before anything above the transport sees them, exactly
// as CAN's segmentation byte is (§5.3.1).
//
// ---------------------------------------------------------------------------------------------
// THE CRC IS SPECIFIED HERE, NOT LEFT TO "CRC-16"
// ---------------------------------------------------------------------------------------------
// "CRC-16" names a dozen incompatible algorithms. Two implementations that both correctly implement
// "CRC-16" will reject each other's frames forever, and the symptom is a link that looks dead. So:
//
//   CRC-16/CCITT-FALSE   polynomial 0x1021, init 0xFFFF, no reflection, no final XOR.
//
// The check value for the ASCII string "123456789" is 0x29B1, which is the standard conformance
// vector for this variant and is asserted in the tests on both sides.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/frame.hpp"

namespace pot {

// ---------------------------------------------------------------------------------------------
// CRC-16/CCITT-FALSE
// ---------------------------------------------------------------------------------------------

constexpr uint16_t kCrc16Init = 0xFFFF;
constexpr uint16_t kCrc16Poly = 0x1021;

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = kCrc16Init);

// ---------------------------------------------------------------------------------------------
// COBS
//
// Consistent Overhead Byte Stuffing: encodes a buffer so that it contains no zero byte, at a cost
// of one byte per 254 bytes of payload plus one. A single 0x00 then delimits frames unambiguously.
// ---------------------------------------------------------------------------------------------

// Worst-case encoded size for `n` payload bytes, excluding the delimiter.
constexpr size_t cobs_max_encoded(size_t n) { return n + n / 254 + 1; }

// Encode into `out`. Returns bytes written, or 0 if `out` is too small. Does not append the
// delimiter — write_serial_frame() does that.
size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t cap);

// Decode in place-safe fashion into `out`. Returns bytes written, or 0 on a malformed sequence.
size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t cap);

// ---------------------------------------------------------------------------------------------
// The serial envelope
//
//   [ COBS( pot-frame || crc16-le ) ] 0x00
//
// The CRC covers the Potluck Frame only, and is computed before COBS so that it protects the payload
// rather than the encoding. A receiver decodes, checks, and hands the frame up.
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kSerialDelimiter = 0x00;
constexpr size_t kCrcBytes = 2;

// The largest serial frame: a full ESP-NOW v2 MTU of Potluck Frame, plus CRC, plus COBS overhead, plus
// the delimiter. Sized from §5.3's profile so a serial link never becomes the narrower one.
constexpr size_t kSerialFrameMax = cobs_max_encoded(kEspNowV2LinkMtu + kCrcBytes) + 1;

// Wrap a Potluck Frame for the wire. Returns bytes written including the trailing delimiter, or 0.
size_t write_serial_frame(const uint8_t* frame, size_t len, uint8_t* out, size_t cap);

enum class SerialError : uint8_t {
    Ok = 0,
    Empty,        // a delimiter with nothing before it — normal on resynchronisation
    CobsInvalid,  // the COBS structure does not decode
    TooShort,     // decoded to fewer bytes than a CRC
    BadCrc,       // decoded cleanly and failed the check
    TooLong,      // longer than kSerialFrameMax
};

const char* serial_error_str(SerialError e);

// Unwrap one COBS-encoded chunk (the bytes between two delimiters). Returns the Potluck Frame in `out`.
SerialError read_serial_frame(const uint8_t* chunk, size_t len, uint8_t* out, size_t cap,
                              size_t& written);

// ---------------------------------------------------------------------------------------------
// Stream reassembler
//
// Feeds bytes in, calls back with complete frames. Holds one frame of state and no allocation, so
// it runs on a node as happily as on a host.
//
// Resynchronisation is the point: a reader that attaches to a running node starts mid-frame, and
// the first delimiter it sees ends a partial frame that will fail its CRC. That is expected, not an
// error, and it is why `Empty` and a single bad CRC after attach are counted rather than reported.
// ---------------------------------------------------------------------------------------------

class SerialReassembler {
  public:
    using FrameFn = void (*)(void* ctx, const uint8_t* frame, size_t len);

    SerialReassembler() = default;

    void reset();

    // Feed received bytes. `on_frame` is called once per good frame.
    void feed(const uint8_t* data, size_t len, FrameFn on_frame, void* ctx);

    struct Stats {
        uint32_t frames_ok;
        uint32_t bad_crc;
        uint32_t cobs_invalid;
        uint32_t too_long;
        uint32_t empty;
        uint32_t bytes_in;
    };
    const Stats& stats() const { return stats_; }

  private:
    uint8_t chunk_[kSerialFrameMax];
    uint8_t frame_[kEspNowV2LinkMtu + kCrcBytes];
    size_t n_ = 0;
    bool overflow_ = false;
    Stats stats_{};
};

}  // namespace pot
