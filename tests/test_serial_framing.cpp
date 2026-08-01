// Serial framing tests — ARCHITECTURE.md §5.3's "COBS framing + CRC-16".
//
// The properties that matter for a byte stream, in order of how much damage getting them wrong
// does: a receiver that attaches mid-frame resynchronises rather than emitting garbage; a corrupted
// frame is rejected rather than parsed; and the encoding round-trips exactly, including the awkward
// inputs — all zeros, no zeros, and a run of exactly 254 non-zero bytes where COBS changes gear.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "pot/serial_framing.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

std::vector<uint8_t> collected;
int collect_count = 0;

void collector(void*, const uint8_t* f, size_t n) {
    collected.assign(f, f + n);
    ++collect_count;
}

std::vector<uint8_t> round_trip_cobs(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> enc(cobs_max_encoded(in.size()) + 8, 0xCC);
    const size_t n = cobs_encode(in.empty() ? nullptr : in.data(), in.size(), enc.data(), enc.size());
    if (n == 0 && !in.empty()) {
        return {0xFF};  // sentinel: encode refused
    }
    // The defining property: no zero byte survives encoding, which is what makes 0x00 usable as a
    // delimiter.
    for (size_t i = 0; i < n; ++i) {
        if (enc[i] == 0) {
            return {0xFE};
        }
    }
    std::vector<uint8_t> dec(in.size() + 8, 0xDD);
    const size_t m = cobs_decode(enc.data(), n, dec.data(), dec.size());
    dec.resize(m);
    return dec;
}

std::vector<uint8_t> a_frame(uint16_t payload_len = 48, uint8_t opcode = kOpHeartbeat) {
    EncodeSpec spec;
    spec.src = 0x0102;
    spec.dst = 0x0304;
    spec.opcode = opcode;
    spec.lclass = kClassL3;
    std::vector<uint8_t> payload(payload_len);
    for (uint16_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>(i * 5 + 1);
    }
    std::vector<uint8_t> buf(kHeaderSize + payload_len);
    size_t written = 0;
    encode(spec, payload.empty() ? nullptr : payload.data(), payload_len, buf.data(), buf.size(),
           written);
    buf.resize(written);
    return buf;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// CRC
// ------------------------------------------------------------------------------------------

TEST(serial, crc16_matches_the_ccitt_false_conformance_vector) {
    // "CRC-16" names a dozen incompatible algorithms, and two implementations that each correctly
    // implement a *different* one reject each other's frames forever. 0x29B1 over "123456789" is
    // the standard vector for CCITT-FALSE, and the Python side asserts the same number.
    const char* s = "123456789";
    CHECK_EQ(crc16(reinterpret_cast<const uint8_t*>(s), 9), static_cast<uint16_t>(0x29B1));
    CHECK_EQ(crc16(nullptr, 0), kCrc16Init);
}

TEST(serial, crc16_detects_every_single_bit_flip_in_a_frame) {
    const std::vector<uint8_t> f = a_frame();
    const uint16_t good = crc16(f.data(), f.size());
    for (size_t byte = 0; byte < f.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> m = f;
            m[byte] ^= static_cast<uint8_t>(1u << bit);
            CHECK(crc16(m.data(), m.size()) != good);
        }
    }
}

// ------------------------------------------------------------------------------------------
// COBS
// ------------------------------------------------------------------------------------------

TEST(serial, cobs_round_trips_the_awkward_inputs) {
    struct Case {
        const char* name;
        std::vector<uint8_t> data;
    };
    std::vector<Case> cases = {
        {"empty", {}},
        {"single zero", {0x00}},
        {"all zeros", std::vector<uint8_t>(64, 0x00)},
        {"no zeros", std::vector<uint8_t>(64, 0xAA)},
        {"leading zero", {0x00, 0x01, 0x02}},
        {"trailing zero", {0x01, 0x02, 0x00}},
        {"253 non-zero", std::vector<uint8_t>(253, 0x01)},
        // 254 is where COBS changes gear and emits a new group; the classic off-by-one.
        {"254 non-zero", std::vector<uint8_t>(254, 0x01)},
        {"255 non-zero", std::vector<uint8_t>(255, 0x01)},
        {"510 non-zero", std::vector<uint8_t>(510, 0x01)},
        {"max mtu", std::vector<uint8_t>(kEspNowV2LinkMtu, 0x5A)},
    };
    // Every byte value, so no value is special by accident.
    std::vector<uint8_t> all256(256);
    for (int i = 0; i < 256; ++i) {
        all256[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    }
    cases.push_back({"every byte value", all256});

    for (const Case& c : cases) {
        const std::vector<uint8_t> out = round_trip_cobs(c.data);
        CHECK_BYTES_EQ(out.empty() ? nullptr : out.data(), out.size(),
                       c.data.empty() ? nullptr : c.data.data(), c.data.size());
    }
}

TEST(serial, cobs_rejects_malformed_input_rather_than_guessing) {
    uint8_t out[64];
    // A zero inside the body is not COBS.
    const uint8_t has_zero[] = {0x03, 0x11, 0x00, 0x22};
    CHECK_EQ(cobs_decode(has_zero, sizeof(has_zero), out, sizeof(out)), static_cast<size_t>(0));
    // A group claiming more bytes than remain. Clamping here would silently produce a short frame
    // and leave the CRC to catch it; rejecting is cheaper and says what happened.
    const uint8_t overruns[] = {0x09, 0x11, 0x22};
    CHECK_EQ(cobs_decode(overruns, sizeof(overruns), out, sizeof(out)), static_cast<size_t>(0));
    CHECK_EQ(cobs_decode(nullptr, 4, out, sizeof(out)), static_cast<size_t>(0));
    // Output too small.
    const uint8_t fine[] = {0x03, 0x11, 0x22};
    CHECK_EQ(cobs_decode(fine, sizeof(fine), out, 1), static_cast<size_t>(0));
}

TEST(serial, cobs_encode_refuses_a_buffer_that_is_one_byte_short) {
    const std::vector<uint8_t> in(100, 0x77);
    std::vector<uint8_t> out(cobs_max_encoded(in.size()));
    CHECK(cobs_encode(in.data(), in.size(), out.data(), out.size()) > 0);
    CHECK_EQ(cobs_encode(in.data(), in.size(), out.data(), out.size() - 1), static_cast<size_t>(0));
}

// ------------------------------------------------------------------------------------------
// The envelope
// ------------------------------------------------------------------------------------------

TEST(serial, a_framed_pot_frame_round_trips_and_contains_no_zero) {
    const std::vector<uint8_t> f = a_frame();
    std::vector<uint8_t> wire(kSerialFrameMax);
    const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());
    CHECK(n > f.size());
    CHECK_EQ(wire[n - 1], kSerialDelimiter);
    for (size_t i = 0; i + 1 < n; ++i) {
        CHECK(wire[i] != 0);  // the delimiter is unambiguous
    }

    uint8_t back[kEspNowV2LinkMtu];
    size_t got = 0;
    CHECK_EQ(static_cast<int>(read_serial_frame(wire.data(), n - 1, back, sizeof(back), got)),
             static_cast<int>(SerialError::Ok));
    CHECK_BYTES_EQ(back, got, f.data(), f.size());

    // And the recovered bytes are still a valid Potluck Frame — the envelope left no trace.
    Frame parsed;
    CHECK_EQ(static_cast<int>(parse(back, got, parsed)), static_cast<int>(FrameError::Ok));
    CHECK_EQ(parsed.hdr.opcode, kOpHeartbeat);
}

TEST(serial, a_corrupted_frame_is_rejected_not_parsed) {
    const std::vector<uint8_t> f = a_frame();
    std::vector<uint8_t> wire(kSerialFrameMax);
    const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());

    uint8_t back[kEspNowV2LinkMtu];
    size_t got = 0;
    // Flip one bit anywhere in the body and the frame must not come through.
    for (size_t i = 0; i + 1 < n; ++i) {
        std::vector<uint8_t> m(wire.begin(), wire.begin() + static_cast<long>(n));
        m[i] ^= 0x01;
        if (m[i] == 0) {
            continue;  // that would be a delimiter, i.e. a different frame boundary, not corruption
        }
        const SerialError e = read_serial_frame(m.data(), n - 1, back, sizeof(back), got);
        CHECK(e != SerialError::Ok);
    }
}

// ------------------------------------------------------------------------------------------
// Reassembly and resynchronisation
// ------------------------------------------------------------------------------------------

TEST(serial, the_reassembler_recovers_frames_from_a_byte_stream) {
    SerialReassembler r;
    r.reset();
    collect_count = 0;

    std::vector<uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        const std::vector<uint8_t> f = a_frame(static_cast<uint16_t>(8 + i * 16));
        std::vector<uint8_t> wire(kSerialFrameMax);
        const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());
        stream.insert(stream.end(), wire.begin(), wire.begin() + static_cast<long>(n));
    }

    // Fed one byte at a time — a UART does not deliver whole frames, and a reassembler that only
    // works on tidy buffers is one that works only in tests.
    for (uint8_t b : stream) {
        r.feed(&b, 1, &collector, nullptr);
    }
    CHECK_EQ(collect_count, 5);
    CHECK_EQ(r.stats().frames_ok, 5u);
    CHECK_EQ(r.stats().bad_crc, 0u);
    CHECK_EQ(r.stats().bytes_in, static_cast<uint32_t>(stream.size()));
}

TEST(serial, attaching_mid_stream_resynchronises_at_the_next_boundary) {
    // The case that matters in the field: a host opens the port while a node is already talking.
    // The first partial frame must be discarded quietly, not delivered as garbage.
    std::vector<uint8_t> stream;
    for (int i = 0; i < 4; ++i) {
        const std::vector<uint8_t> f = a_frame(32);
        std::vector<uint8_t> wire(kSerialFrameMax);
        const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());
        stream.insert(stream.end(), wire.begin(), wire.begin() + static_cast<long>(n));
    }

    // Start 10 bytes into the first frame.
    SerialReassembler r;
    r.reset();
    collect_count = 0;
    r.feed(stream.data() + 10, stream.size() - 10, &collector, nullptr);

    // Three whole frames survive; the truncated first one is rejected, not delivered.
    CHECK_EQ(r.stats().frames_ok, 3u);
    CHECK_EQ(collect_count, 3);
    CHECK((r.stats().bad_crc + r.stats().cobs_invalid) == 1u);
}

TEST(serial, garbage_between_frames_does_not_lose_the_frames_around_it) {
    SerialReassembler r;
    r.reset();
    collect_count = 0;

    const std::vector<uint8_t> f = a_frame(24);
    std::vector<uint8_t> wire(kSerialFrameMax);
    const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());

    std::vector<uint8_t> stream(wire.begin(), wire.begin() + static_cast<long>(n));
    const uint8_t noise[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};  // ends with a delimiter
    stream.insert(stream.end(), noise, noise + sizeof(noise));
    stream.insert(stream.end(), wire.begin(), wire.begin() + static_cast<long>(n));

    r.feed(stream.data(), stream.size(), &collector, nullptr);
    CHECK_EQ(r.stats().frames_ok, 2u);
    CHECK(r.stats().bad_crc + r.stats().cobs_invalid >= 1u);
}

TEST(serial, an_overlong_run_is_dropped_and_the_stream_recovers) {
    SerialReassembler r;
    r.reset();
    collect_count = 0;

    // More bytes than any legal frame, with no delimiter, then a good frame.
    std::vector<uint8_t> stream(kSerialFrameMax + 200, 0x41);
    stream.push_back(kSerialDelimiter);
    const std::vector<uint8_t> f = a_frame(16);
    std::vector<uint8_t> wire(kSerialFrameMax);
    const size_t n = write_serial_frame(f.data(), f.size(), wire.data(), wire.size());
    stream.insert(stream.end(), wire.begin(), wire.begin() + static_cast<long>(n));

    r.feed(stream.data(), stream.size(), &collector, nullptr);
    CHECK_EQ(r.stats().too_long, 1u);
    CHECK_EQ(r.stats().frames_ok, 1u);
    CHECK_EQ(collect_count, 1);
}

TEST(serial, repeated_delimiters_are_normal_not_errors) {
    SerialReassembler r;
    r.reset();
    collect_count = 0;
    const uint8_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    r.feed(zeros, sizeof(zeros), &collector, nullptr);
    CHECK_EQ(r.stats().empty, 8u);
    CHECK_EQ(r.stats().frames_ok, 0u);
    CHECK_EQ(r.stats().bad_crc, 0u);
}

TEST(serial, the_frame_size_ceiling_matches_the_5_3_profile) {
    // §5.3 gives the serial profile the same 1446 B payload cap as ESP-NOW v2 "so behaviour
    // matches". The envelope must therefore carry a full v2 MTU.
    CHECK(kSerialFrameMax > kEspNowV2LinkMtu + kCrcBytes);
    const std::vector<uint8_t> big(kEspNowV2LinkMtu, 0x33);
    std::vector<uint8_t> wire(kSerialFrameMax);
    const size_t n = write_serial_frame(big.data(), big.size(), wire.data(), wire.size());
    CHECK(n > 0);
    CHECK(n <= kSerialFrameMax);
}

// ------------------------------------------------------------------------------------------
// Differential corpus. `pot_tests --emit-serial-corpus <dir>` writes payloads and the exact bytes
// this encoder produced for them; host/potluck/tests/test_serial_diff.py requires the Python
// implementation to produce the same bytes and decode them back.
//
// The two sides already pass the same reference vectors. This checks the inputs nobody thought to
// write down, which is where a COBS group boundary or a CRC seeding difference actually hides.
// ------------------------------------------------------------------------------------------

namespace pot_serial_corpus {

int emit(const char* dir) {
    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "serial_corpus.jsonl";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    // Deterministic, so the corpus is byte-identical on every machine.
    uint64_t st = 0x5E71A15EEDull;
    auto next = [&st]() {
        st ^= st >> 12;
        st ^= st << 25;
        st ^= st >> 27;
        return st * 0x2545F4914F6CDD1Dull;
    };

    std::vector<std::vector<uint8_t>> corpus;
    // The structural cases, where COBS changes gear.
    for (size_t n : {size_t{1}, size_t{2}, size_t{253}, size_t{254}, size_t{255}, size_t{256},
                     size_t{508}, size_t{509}, size_t{1446}, size_t{1470}}) {
        corpus.push_back(std::vector<uint8_t>(n, 0x01));
        corpus.push_back(std::vector<uint8_t>(n, 0x00));
    }
    // Real Potluck Frames.
    for (uint16_t n : {uint16_t{0}, uint16_t{8}, uint16_t{48}, uint16_t{226}, uint16_t{1446}}) {
        corpus.push_back(a_frame(n, kOpErr));
    }
    // And random ones, including plenty of zeros, which is where the encoding does its work.
    for (int i = 0; i < 400; ++i) {
        const size_t n = static_cast<size_t>(next() % 600) + 1;
        std::vector<uint8_t> v(n);
        for (size_t k = 0; k < n; ++k) {
            const uint64_t r = next();
            v[k] = (r % 3 == 0) ? 0 : static_cast<uint8_t>(r >> 33);
        }
        corpus.push_back(v);
    }

    static const char* hex = "0123456789abcdef";
    for (const auto& in : corpus) {
        std::vector<uint8_t> wire(kSerialFrameMax + 16);
        const size_t n = write_serial_frame(in.data(), in.size(), wire.data(), wire.size());
        if (n == 0) {
            continue;
        }
        std::fprintf(f, "{\"crc\":%u,\"in\":\"", static_cast<unsigned>(crc16(in.data(), in.size())));
        for (uint8_t b : in) {
            std::fputc(hex[b >> 4], f);
            std::fputc(hex[b & 0xF], f);
        }
        std::fprintf(f, "\",\"wire\":\"");
        for (size_t i = 0; i < n; ++i) {
            std::fputc(hex[wire[i] >> 4], f);
            std::fputc(hex[wire[i] & 0xF], f);
        }
        std::fprintf(f, "\"}\n");
    }
    std::fclose(f);
    std::printf("wrote %s (%zu frames)\n", path.c_str(), corpus.size());
    return 0;
}

}  // namespace pot_serial_corpus
