// Potluck Frame round-trip and field-fidelity tests.

#include <cstring>
#include <vector>

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

// Encode then parse, checking that nothing was lost in either direction.
void round_trip(const EncodeSpec& spec, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(encoded_size(spec, static_cast<uint16_t>(payload.size())) + 16, 0xCD);
    size_t written = 0;
    const FrameError enc = encode(spec, payload.empty() ? nullptr : payload.data(),
                                  static_cast<uint16_t>(payload.size()), buf.data(), buf.size(),
                                  written);
    CHECK_EQ(static_cast<int>(enc), static_cast<int>(FrameError::Ok));
    if (enc != FrameError::Ok) {
        return;
    }
    CHECK_EQ(written, encoded_size(spec, static_cast<uint16_t>(payload.size())));

    Frame f;
    const FrameError dec = parse(buf.data(), written, f);
    CHECK_EQ(static_cast<int>(dec), static_cast<int>(FrameError::Ok));
    if (dec != FrameError::Ok) {
        return;
    }

    CHECK_EQ(f.hdr.magic, kMagic);
    CHECK_EQ(ver_of(f.hdr.ver_flags), kVersion);
    CHECK_EQ(f.hdr.src, spec.src);
    CHECK_EQ(f.hdr.dst, spec.dst);
    CHECK_EQ(f.hdr.opcode, spec.opcode);
    CHECK_EQ(f.lclass(), spec.lclass);
    CHECK_EQ(f.priority(), spec.priority);
    CHECK_EQ(f.hdr.seq, spec.seq);
    CHECK_EQ(f.hdr.msg_id, spec.msg_id);
    CHECK_EQ(f.wants_ack(), spec.ack_req);
    CHECK_EQ(f.has_auth(), spec.auth);
    CHECK_EQ(f.is_frag(), spec.frag);
    CHECK_EQ(f.payload_len, static_cast<uint16_t>(payload.size()));

    if (!payload.empty()) {
        CHECK_BYTES_EQ(f.payload, f.payload_len, payload.data(), payload.size());
    }
    if (spec.frag) {
        CHECK_EQ(f.hdr.frag_off, spec.frag_off);
        CHECK_EQ(f.hdr.total_len, spec.total_len);
    } else {
        CHECK_EQ(f.hdr.frag_off, static_cast<uint16_t>(0));
        CHECK_EQ(f.hdr.total_len, static_cast<uint16_t>(payload.size()));
    }
    if (spec.auth) {
        CHECK(f.auth_tag != nullptr);
        for (size_t i = 0; i < kAuthTagSize; ++i) {
            CHECK_EQ(f.auth_tag[i], static_cast<uint8_t>(0));
        }
    } else {
        CHECK(f.auth_tag == nullptr);
    }
}

std::vector<uint8_t> ramp(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return v;
}

}  // namespace

TEST(codec, round_trip_minimal) {
    EncodeSpec spec;
    spec.src = 1;
    spec.dst = 2;
    spec.opcode = kOpHeartbeat;
    round_trip(spec, {});
}

TEST(codec, round_trip_every_class_and_priority) {
    // The class and priority share one byte (§5.1 lclass_pri), which is exactly the kind of packed
    // field where an off-by-one shift survives casual testing. Try the whole space.
    for (uint8_t cls = 0; cls <= kClassMax; ++cls) {
        for (uint8_t pri = 0; pri <= kPriorityMax; ++pri) {
            EncodeSpec spec;
            spec.src = 0x1234;
            spec.dst = 0x5678;
            spec.opcode = kOpErr;
            spec.lclass = cls;
            spec.priority = pri;
            round_trip(spec, ramp(8));
        }
    }
}

TEST(codec, round_trip_flag_combinations) {
    // FRAG, ACKREQ and AUTH are independent bits; all eight combinations must survive. ERR is the
    // opcode because §5.4 forbids fragmenting the others M0 uses.
    for (int bits = 0; bits < 8; ++bits) {
        EncodeSpec spec;
        spec.src = 7;
        spec.dst = 8;
        spec.opcode = kOpErr;
        spec.lclass = kClassL4;
        spec.frag = (bits & 1) != 0;
        spec.ack_req = (bits & 2) != 0;
        spec.auth = (bits & 4) != 0;
        if (spec.frag) {
            spec.frag_off = 32;
            spec.total_len = 128;
        }
        round_trip(spec, ramp(16));
    }
}

TEST(codec, round_trip_payload_length_sweep) {
    // Boundaries around both transport profiles (§5.3) plus the small sizes M0 actually uses.
    const uint16_t sizes[] = {0, 1, 2, 8, 12, 24, 47, 48, 49, 225, 226, 227, 1445, 1446};
    for (uint16_t n : sizes) {
        EncodeSpec spec;
        spec.src = 0xABCD;
        spec.dst = 0xEF01;
        spec.opcode = kOpErr;
        spec.lclass = kClassL3;
        spec.seq = n;
        round_trip(spec, ramp(n));
    }
}

TEST(codec, seq_and_msg_id_wrap_boundaries) {
    const uint16_t values[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
    for (uint16_t v : values) {
        EncodeSpec spec;
        spec.src = v;
        spec.dst = static_cast<uint16_t>(~v);
        spec.opcode = kOpHeartbeat;
        spec.seq = v;
        spec.msg_id = v;
        round_trip(spec, ramp(4));
    }
}

TEST(codec, parse_is_zero_copy_into_the_callers_buffer) {
    // The payload pointer must point inside the buffer that was parsed, not at a copy: on a node
    // that buffer is an RX ring slot and duplicating it would blow the §6 budget.
    EncodeSpec spec;
    spec.src = 1;
    spec.dst = 2;
    spec.opcode = kOpErr;
    uint8_t buf[64] = {};
    size_t written = 0;
    const std::vector<uint8_t> payload = ramp(10);
    CHECK_EQ(static_cast<int>(encode(spec, payload.data(), 10, buf, sizeof(buf), written)),
             static_cast<int>(FrameError::Ok));

    Frame f;
    CHECK_EQ(static_cast<int>(parse(buf, written, f)), static_cast<int>(FrameError::Ok));
    CHECK(f.payload == buf + kHeaderSize);
}

TEST(codec, encoded_size_agrees_with_encode) {
    for (int auth = 0; auth < 2; ++auth) {
        for (uint16_t n = 0; n < 64; ++n) {
            EncodeSpec spec;
            spec.opcode = kOpErr;
            spec.auth = auth != 0;
            const std::vector<uint8_t> payload = ramp(n);
            uint8_t buf[128] = {};
            size_t written = 0;
            CHECK_EQ(static_cast<int>(encode(spec, payload.empty() ? nullptr : payload.data(), n,
                                             buf, sizeof(buf), written)),
                     static_cast<int>(FrameError::Ok));
            CHECK_EQ(written, encoded_size(spec, n));
        }
    }
}

TEST(codec, accessors_are_inverses_of_the_packers) {
    for (int v = 0; v < 16; ++v) {
        for (int f = 0; f < 16; ++f) {
            const uint8_t packed = make_ver_flags(static_cast<uint8_t>(v), static_cast<uint8_t>(f));
            CHECK_EQ(ver_of(packed), static_cast<uint8_t>(v));
            CHECK_EQ(flags_of(packed), static_cast<uint8_t>(f));
        }
    }
    for (int c = 0; c < 8; ++c) {
        for (int p = 0; p < 32; ++p) {
            const uint8_t packed = make_lclass_pri(static_cast<uint8_t>(c), static_cast<uint8_t>(p));
            CHECK_EQ(lclass_of(packed), static_cast<uint8_t>(c));
            CHECK_EQ(priority_of(packed), static_cast<uint8_t>(p));
        }
    }
}
