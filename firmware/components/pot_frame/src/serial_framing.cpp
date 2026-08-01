// Serial framing — see serial_framing.hpp and ARCHITECTURE.md §5.3.

#include "pot/serial_framing.hpp"

#include <cstring>

namespace pot {

const char* serial_error_str(SerialError e) {
    switch (e) {
        case SerialError::Ok: return "ok";
        case SerialError::Empty: return "empty";
        case SerialError::CobsInvalid: return "cobs_invalid";
        case SerialError::TooShort: return "too_short";
        case SerialError::BadCrc: return "bad_crc";
        case SerialError::TooLong: return "too_long";
    }
    return "unknown";
}

// -------------------------------------------------------------------------------------------
// CRC-16/CCITT-FALSE. Bitwise rather than table-driven: a 512-byte table is real DRAM against §6's
// budget, and at 100 ms per frame the cycles are free. If a later milestone pushes serial hard, a
// table is the obvious trade and this is the place to make it.
// -------------------------------------------------------------------------------------------

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ kCrc16Poly)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// -------------------------------------------------------------------------------------------
// COBS
// -------------------------------------------------------------------------------------------

size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < cobs_max_encoded(len)) {
        return 0;
    }
    if (in == nullptr && len != 0) {
        return 0;
    }

    size_t read = 0;
    size_t write = 1;      // the first code byte is written last, once its run length is known
    size_t code_at = 0;    // where that pending code byte lives
    uint8_t code = 1;

    while (read < len) {
        if (in[read] == 0) {
            out[code_at] = code;
            code_at = write++;
            code = 1;
        } else {
            out[write++] = in[read];
            if (++code == 0xFF) {
                // A run of 254 non-zero bytes: emit the maximum code and start a new group. This is
                // where COBS's one-byte-per-254 overhead comes from.
                out[code_at] = code;
                code_at = write++;
                code = 1;
            }
        }
        ++read;
    }
    out[code_at] = code;
    return write;
}

size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    if (in == nullptr || out == nullptr || len == 0) {
        return 0;
    }

    size_t read = 0;
    size_t write = 0;
    while (read < len) {
        const uint8_t code = in[read];
        if (code == 0) {
            return 0;  // a zero inside the body is not COBS
        }
        ++read;
        // The group claims `code - 1` literal bytes. Running past the input means the encoding is
        // truncated or corrupt — a decoder that clamped here would silently produce a short frame,
        // which the CRC would then have to catch. Rejecting outright is cheaper and clearer.
        if (read + code - 1 > len) {
            return 0;
        }
        for (uint8_t i = 1; i < code; ++i) {
            if (write >= cap) {
                return 0;
            }
            out[write++] = in[read++];
        }
        // A group shorter than 255 encodes a zero, except when it ends the input.
        if (code != 0xFF && read < len) {
            if (write >= cap) {
                return 0;
            }
            out[write++] = 0;
        }
    }
    return write;
}

// -------------------------------------------------------------------------------------------
// Envelope
// -------------------------------------------------------------------------------------------

size_t write_serial_frame(const uint8_t* frame, size_t len, uint8_t* out, size_t cap) {
    if (frame == nullptr || out == nullptr || len == 0) {
        return 0;
    }
    uint8_t staged[kEspNowV2LinkMtu + kCrcBytes];
    if (len + kCrcBytes > sizeof(staged)) {
        return 0;
    }

    std::memcpy(staged, frame, len);
    // The CRC covers the Potluck Frame, computed before COBS so it protects the payload rather than the
    // encoding. Little-endian, like everything else on this wire (§5.1).
    const uint16_t c = crc16(frame, len);
    staged[len] = static_cast<uint8_t>(c & 0xFF);
    staged[len + 1] = static_cast<uint8_t>(c >> 8);

    const size_t n = cobs_encode(staged, len + kCrcBytes, out, cap ? cap - 1 : 0);
    if (n == 0 || n + 1 > cap) {
        return 0;
    }
    out[n] = kSerialDelimiter;
    return n + 1;
}

SerialError read_serial_frame(const uint8_t* chunk, size_t len, uint8_t* out, size_t cap,
                              size_t& written) {
    written = 0;
    if (len == 0) {
        return SerialError::Empty;
    }
    if (len > kSerialFrameMax) {
        return SerialError::TooLong;
    }

    uint8_t staged[kEspNowV2LinkMtu + kCrcBytes];
    const size_t n = cobs_decode(chunk, len, staged, sizeof(staged));
    if (n == 0) {
        return SerialError::CobsInvalid;
    }
    if (n < kCrcBytes) {
        return SerialError::TooShort;
    }

    const size_t body = n - kCrcBytes;
    const uint16_t want =
        static_cast<uint16_t>(staged[body]) | static_cast<uint16_t>(staged[body + 1] << 8);
    if (crc16(staged, body) != want) {
        return SerialError::BadCrc;
    }
    if (body > cap) {
        return SerialError::TooLong;
    }

    std::memcpy(out, staged, body);
    written = body;
    return SerialError::Ok;
}

// -------------------------------------------------------------------------------------------
// Reassembler
// -------------------------------------------------------------------------------------------

void SerialReassembler::reset() {
    n_ = 0;
    overflow_ = false;
    stats_ = Stats{};
}

void SerialReassembler::feed(const uint8_t* data, size_t len, FrameFn on_frame, void* ctx) {
    if (data == nullptr) {
        return;
    }
    stats_.bytes_in += static_cast<uint32_t>(len);

    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b != kSerialDelimiter) {
            if (n_ < sizeof(chunk_)) {
                chunk_[n_++] = b;
            } else {
                // Longer than any legal frame. Keep consuming to the next delimiter rather than
                // emitting garbage — a stream that has lost sync recovers at the next boundary.
                overflow_ = true;
            }
            continue;
        }

        if (overflow_) {
            ++stats_.too_long;
        } else if (n_ == 0) {
            // Two delimiters in a row, or the first byte after attaching. Normal, not an error.
            ++stats_.empty;
        } else {
            size_t got = 0;
            const SerialError e = read_serial_frame(chunk_, n_, frame_, sizeof(frame_), got);
            switch (e) {
                case SerialError::Ok:
                    ++stats_.frames_ok;
                    if (on_frame != nullptr) {
                        on_frame(ctx, frame_, got);
                    }
                    break;
                case SerialError::BadCrc: ++stats_.bad_crc; break;
                case SerialError::TooLong: ++stats_.too_long; break;
                case SerialError::Empty: ++stats_.empty; break;
                default: ++stats_.cobs_invalid; break;
            }
        }
        n_ = 0;
        overflow_ = false;
    }
}

}  // namespace pot
