// Fuzzing the Potluck Frame parser.
//
// §5 says one format means one parser and one fuzz target. This is that target. It is a
// deterministic in-process fuzzer rather than libFuzzer: the corpus is reproducible, it runs on
// every build on every machine, and a failure prints the exact input rather than dropping a file
// somewhere. A real coverage-guided run is worth doing later against the same entry point.
//
// The properties checked hold for *every* input, valid or not:
//
//   1. parse() returns. It does not crash, hang, or read outside the buffer it was given —
//      enforced with guard bands around every input rather than trusted.
//   2. If parse() says Ok, the Frame it produced is internally consistent: the payload and tag
//      pointers lie inside the buffer, and the lengths add up to exactly the bytes supplied.
//   3. If parse() says Ok, re-encoding the parsed frame reproduces the input byte for byte.
//      This is the strongest of the three: it means a parser that quietly ignored a field would
//      be caught, because the field would not come back.

#include <cstring>
#include <vector>

#include "pot/frame.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

// xorshift64*, so the corpus is identical on every machine and every run. A test that fuzzes with
// a time-seeded PRNG is a test that fails on someone else's laptop and cannot be reproduced.
class Rng {
  public:
    explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
    uint8_t byte() { return static_cast<uint8_t>(next() >> 33); }

  private:
    uint64_t s_;
};

constexpr uint8_t kGuard = 0xA5;
constexpr size_t kGuardLen = 32;

// Run parse() on `input` with guard bands on both sides, and check all three properties.
// Returns the parse result so callers can assert on specific corpus entries.
FrameError check_one(const std::vector<uint8_t>& input, uint16_t max_payload = kMaxPayloadV2) {
    std::vector<uint8_t> arena(kGuardLen + input.size() + kGuardLen, kGuard);
    if (!input.empty()) {
        std::memcpy(arena.data() + kGuardLen, input.data(), input.size());
    }
    uint8_t* const buf = arena.data() + kGuardLen;

    Frame f;
    const FrameError e = parse(buf, input.size(), f, max_payload);

    // Property 1: the guards are untouched.
    for (size_t i = 0; i < kGuardLen; ++i) {
        if (arena[i] != kGuard || arena[kGuardLen + input.size() + i] != kGuard) {
            ::tst::fail(__FILE__, __LINE__,
                        "parse() wrote or ran outside its buffer for input " +
                            ::tst::hex(input.data(), input.size()));
            return e;
        }
    }
    ++::tst::checks();

    if (e != FrameError::Ok) {
        return e;
    }

    // Property 2: internal consistency.
    const size_t overhead = kHeaderSize + (f.has_auth() ? kAuthTagSize : 0);
    const bool consistent =
        (static_cast<size_t>(f.payload_len) + overhead == input.size()) &&
        (f.payload_len == 0 || (f.payload >= buf && f.payload + f.payload_len <= buf + input.size())) &&
        (!f.has_auth() ||
         (f.auth_tag >= buf && f.auth_tag + kAuthTagSize <= buf + input.size())) &&
        (f.hdr.magic == kMagic) && (ver_of(f.hdr.ver_flags) == kVersion) &&
        (f.lclass() <= kClassMax) && (f.payload_len <= max_payload);
    if (!consistent) {
        ::tst::fail(__FILE__, __LINE__,
                    "parse() accepted an internally inconsistent frame: " +
                        ::tst::hex(input.data(), input.size()));
        return e;
    }
    ++::tst::checks();

    // Property 3: re-encoding reproduces the input exactly.
    EncodeSpec spec;
    spec.src = f.hdr.src;
    spec.dst = f.hdr.dst;
    spec.opcode = f.hdr.opcode;
    spec.lclass = f.lclass();
    spec.priority = f.priority();
    spec.seq = f.hdr.seq;
    spec.msg_id = f.hdr.msg_id;
    spec.ack_req = f.wants_ack();
    spec.auth = f.has_auth();
    spec.frag = f.is_frag();
    spec.frag_off = f.hdr.frag_off;
    spec.total_len = f.hdr.total_len;

    std::vector<uint8_t> again(input.size() + 16, 0);
    size_t written = 0;
    const FrameError re = encode(spec, f.payload, f.payload_len, again.data(), again.size(), written);
    if (re != FrameError::Ok) {
        // The one legitimate asymmetry: parse() accepts a frame whose AUTH tag holds nonzero
        // bytes, but encode() always writes zeroes because M5 owns that field. Re-encoding a
        // frame with a nonzero tag would differ, so that case is excluded below, not here.
        ::tst::fail(__FILE__, __LINE__, std::string("re-encode failed with ") +
                                            frame_error_str(re) + " for " +
                                            ::tst::hex(input.data(), input.size()));
        return e;
    }

    bool tag_is_zero = true;
    if (f.has_auth()) {
        for (size_t i = 0; i < kAuthTagSize; ++i) {
            if (f.auth_tag[i] != 0) {
                tag_is_zero = false;
                break;
            }
        }
    }
    if (tag_is_zero) {
        if (written != input.size() || std::memcmp(again.data(), input.data(), written) != 0) {
            ::tst::fail(__FILE__, __LINE__,
                        "re-encode did not reproduce the input\n      in  = " +
                            ::tst::hex(input.data(), input.size()) +
                            "\n      out = " + ::tst::hex(again.data(), written));
        }
        ++::tst::checks();
    }

    return e;
}

// Seed corpus: frames that sit on a boundary the parser has to get right. A random fuzzer almost
// never generates a valid 16-byte header by chance, so without these the mutation loop would only
// ever exercise the rejection paths.
std::vector<std::vector<uint8_t>> seed_corpus() {
    std::vector<std::vector<uint8_t>> corpus;

    struct Seed {
        uint8_t opcode;
        uint8_t lclass;
        uint8_t priority;
        bool ack;
        bool auth;
        bool frag;
        uint16_t payload_len;
        uint16_t frag_off;
        uint16_t total_len;
    };
    const Seed seeds[] = {
        {kOpHello, kClassL3, 0, false, false, false, 24, 0, 0},
        {kOpHelloAck, kClassL3, 0, false, false, false, 12, 0, 0},
        {kOpHeartbeat, kClassL3, 0, true, false, false, 48, 0, 0},
        {kOpHeartbeat, kClassL3, 0, false, false, false, 48, 0, 0},
        {kOpBye, kClassL3, 0, false, false, false, 8, 0, 0},
        {kOpErr, kClassL4, 31, false, false, false, 8, 0, 0},
        {kOpErr, kClassL4, 0, false, true, false, 8, 0, 0},     // AUTH
        {kOpErr, kClassL4, 0, false, false, true, 8, 0, 64},    // first fragment
        {kOpErr, kClassL4, 0, false, false, true, 8, 56, 64},   // last fragment
        {kOpErr, kClassL0, 0, false, false, false, 0, 0, 0},    // empty payload
        {kOpErr, kClassL0, 0, true, true, true, 4, 0, 16},      // all three flags
        {kOpErr, kClassL3, 0, false, false, false, 226, 0, 0},  // exactly the v1 cap
        {kOpErr, kClassL3, 0, false, false, false, 1446, 0, 0}, // exactly the v2 cap
    };

    for (const Seed& s : seeds) {
        EncodeSpec spec;
        spec.src = 0x1234;
        spec.dst = 0x5678;
        spec.opcode = s.opcode;
        spec.lclass = s.lclass;
        spec.priority = s.priority;
        spec.seq = 0x9ABC;
        spec.msg_id = 0xDEF0;
        spec.ack_req = s.ack;
        spec.auth = s.auth;
        spec.frag = s.frag;
        spec.frag_off = s.frag_off;
        spec.total_len = s.total_len;

        std::vector<uint8_t> payload(s.payload_len);
        for (uint16_t i = 0; i < s.payload_len; ++i) {
            payload[i] = static_cast<uint8_t>(i * 31 + 7);
        }
        std::vector<uint8_t> buf(encoded_size(spec, s.payload_len));
        size_t written = 0;
        if (encode(spec, payload.empty() ? nullptr : payload.data(), s.payload_len, buf.data(),
                   buf.size(), written) == FrameError::Ok) {
            buf.resize(written);
            corpus.push_back(buf);
        }
    }
    return corpus;
}

}  // namespace

TEST(fuzz, seed_corpus_all_parses_clean) {
    const auto corpus = seed_corpus();
    CHECK_EQ(corpus.size(), static_cast<size_t>(13));
    for (const auto& c : corpus) {
        CHECK_EQ(static_cast<int>(check_one(c)), static_cast<int>(FrameError::Ok));
    }
}

TEST(fuzz, mutated_corpus) {
    // Every mutation of a valid frame. These are the inputs that get closest to slipping through:
    // one flipped bit in a length field is far more dangerous than a megabyte of random noise.
    const auto corpus = seed_corpus();
    Rng rng(0xD9D9D9D900000001ull);

    for (const auto& base : corpus) {
        for (int iter = 0; iter < 400; ++iter) {
            std::vector<uint8_t> m = base;
            const int mutations = 1 + static_cast<int>(rng.below(3));
            for (int k = 0; k < mutations; ++k) {
                switch (rng.below(6)) {
                    case 0:  // flip one bit
                        if (!m.empty()) {
                            const size_t i = rng.below(static_cast<uint32_t>(m.size()));
                            m[i] ^= static_cast<uint8_t>(1u << rng.below(8));
                        }
                        break;
                    case 1:  // replace one byte
                        if (!m.empty()) {
                            m[rng.below(static_cast<uint32_t>(m.size()))] = rng.byte();
                        }
                        break;
                    case 2:  // truncate
                        if (!m.empty()) {
                            m.resize(rng.below(static_cast<uint32_t>(m.size())));
                        }
                        break;
                    case 3:  // extend
                        for (int n = static_cast<int>(rng.below(16)); n > 0; --n) {
                            m.push_back(rng.byte());
                        }
                        break;
                    case 4:  // rewrite a header field with an interesting value
                        if (m.size() >= kHeaderSize) {
                            static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80,
                                                                  0xFE, 0xFF, 0xD9, 0x10};
                            m[rng.below(kHeaderSize)] =
                                interesting[rng.below(sizeof(interesting))];
                        }
                        break;
                    default:  // swap two bytes
                        if (m.size() >= 2) {
                            const size_t a = rng.below(static_cast<uint32_t>(m.size()));
                            const size_t b = rng.below(static_cast<uint32_t>(m.size()));
                            const uint8_t t = m[a];
                            m[a] = m[b];
                            m[b] = t;
                        }
                        break;
                }
            }
            // Alternate the pinned profile cap so the v1 ceiling is exercised too (§5.3).
            check_one(m, (iter & 1) ? kMaxPayloadV1 : kMaxPayloadV2);
        }
    }
}

TEST(fuzz, pure_random_inputs) {
    Rng rng(0xD9D9D9D900000002ull);
    for (int iter = 0; iter < 4000; ++iter) {
        // Lengths clustered around the header size, where the boundary conditions live.
        size_t n;
        switch (rng.below(4)) {
            case 0: n = rng.below(20); break;
            case 1: n = 16 + rng.below(64); break;
            case 2: n = rng.below(300); break;
            default: n = rng.below(1600); break;
        }
        std::vector<uint8_t> input(n);
        for (size_t i = 0; i < n; ++i) {
            input[i] = rng.byte();
        }
        // Give a fraction of them a valid magic byte so the parser gets past the first check.
        if (n > 0 && rng.below(2) == 0) {
            input[0] = kMagic;
            if (n > 1 && rng.below(2) == 0) {
                input[1] = make_ver_flags(kVersion, static_cast<uint8_t>(rng.below(16)));
            }
        }
        check_one(input, (iter & 1) ? kMaxPayloadV1 : kMaxPayloadV2);
    }
}

TEST(fuzz, length_boundary_sweep) {
    // Every total length from 0 to 40 behind a valid header, with and without AUTH. The header is
    // 16 bytes and the tag 8, so 16, 17, 23, 24 and 25 are all boundaries where an off-by-one in the
    // overhead arithmetic would either accept a frame with no room for its tag or reject a valid one.
    for (int auth = 0; auth < 2; ++auth) {
        for (size_t total = 0; total <= 40; ++total) {
            std::vector<uint8_t> f(total, 0x5A);
            if (total >= 1) f[0] = kMagic;
            if (total >= 2) f[1] = make_ver_flags(kVersion, auth ? kFlagAuth : 0);
            if (total >= 7) f[6] = kOpErr;
            if (total >= 8) f[7] = make_lclass_pri(kClassL3, 0);
            if (total >= 16) {
                const size_t overhead = kHeaderSize + (auth ? kAuthTagSize : 0);
                const size_t payload = total > overhead ? total - overhead : 0;
                f[12] = 0;
                f[13] = 0;
                f[14] = static_cast<uint8_t>(payload & 0xFF);
                f[15] = static_cast<uint8_t>(payload >> 8);
            }
            const FrameError e = check_one(f);

            // The expected verdict, derived from §5 rather than from the implementation.
            const size_t overhead = kHeaderSize + (auth ? kAuthTagSize : 0);
            FrameError want;
            if (total < kHeaderSize) {
                want = FrameError::TooShort;
            } else if (total < overhead) {
                want = FrameError::MissingAuthTag;
            } else {
                want = FrameError::Ok;
            }
            CHECK_EQ(static_cast<int>(e), static_cast<int>(want));
        }
    }
}

TEST(fuzz, length_field_targeted_mutation) {
    // frag_off and total_len are the two fields where a wrong answer is a memory-safety bug rather
    // than a parse failure, so they get their own exhaustive-ish pass instead of relying on a random
    // mutator to land on them.
    static const uint16_t values[] = {0,     1,     2,      15,     16,     17,    64,
                                      225,   226,   227,    250,    255,    256,   1445,
                                      1446,  1447,  0x7FFF, 0x8000, 0xFFFE, 0xFFFF};

    for (int frag = 0; frag < 2; ++frag) {
        for (uint16_t off : values) {
            for (uint16_t total : values) {
                std::vector<uint8_t> f(kHeaderSize + 8, 0x33);
                f[0] = kMagic;
                f[1] = make_ver_flags(kVersion, frag ? kFlagFrag : 0);
                f[2] = 0x01;
                f[3] = 0x00;
                f[4] = 0x02;
                f[5] = 0x00;
                f[6] = kOpErr;  // fragmentable, so §5.4 does not short-circuit the length checks
                f[7] = make_lclass_pri(kClassL3, 0);
                f[12] = static_cast<uint8_t>(off & 0xFF);
                f[13] = static_cast<uint8_t>(off >> 8);
                f[14] = static_cast<uint8_t>(total & 0xFF);
                f[15] = static_cast<uint8_t>(total >> 8);

                const FrameError e = check_one(f);

                // Derived from §5.4, independently of the codec.
                const uint16_t payload_len = 8;
                FrameError want;
                if (frag) {
                    want = (static_cast<size_t>(off) + payload_len > total)
                               ? FrameError::LengthMismatch
                               : FrameError::Ok;
                } else {
                    want = (off != 0 || total != payload_len) ? FrameError::LengthMismatch
                                                             : FrameError::Ok;
                }
                CHECK_EQ(static_cast<int>(e), static_cast<int>(want));
            }
        }
    }
}

TEST(fuzz, dictionary_splice) {
    // Splice whole meaningful tokens into random positions rather than flipping bits. A bit-flipper
    // essentially never produces a second valid 16-byte header inside a buffer, so it never explores
    // what happens when one appears where a payload was expected.
    static const uint8_t valid_header[] = {0xD9, 0x10, 0x01, 0x00, 0x02, 0x00, 0x7F, 0x60,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00};
    static const uint8_t magic_only[] = {0xD9};
    static const uint8_t max_len[] = {0xFF, 0xFF};
    static const uint8_t v2_len[] = {0xA6, 0x05};  // 1446, the v2 payload cap
    static const uint8_t v1_len[] = {0xE2, 0x00};  // 226, the v1 payload cap
    static const uint8_t all_flags[] = {0x1F};     // version 1 with every flag bit, reserved included

    struct Token {
        const uint8_t* data;
        size_t len;
    };
    const Token tokens[] = {
        {valid_header, sizeof(valid_header)}, {magic_only, sizeof(magic_only)},
        {max_len, sizeof(max_len)},           {v2_len, sizeof(v2_len)},
        {v1_len, sizeof(v1_len)},             {all_flags, sizeof(all_flags)},
    };

    const auto corpus = seed_corpus();
    Rng rng(0xD9D9D9D900000004ull);
    for (const auto& base : corpus) {
        for (int iter = 0; iter < 200; ++iter) {
            std::vector<uint8_t> m = base;
            const Token& t = tokens[rng.below(sizeof(tokens) / sizeof(tokens[0]))];
            const size_t at = m.empty() ? 0 : rng.below(static_cast<uint32_t>(m.size()));
            if (rng.below(2) == 0) {
                m.insert(m.begin() + static_cast<long>(at), t.data, t.data + t.len);
            } else {
                for (size_t i = 0; i < t.len && at + i < m.size(); ++i) {
                    m[at + i] = t.data[i];
                }
            }
            check_one(m, (iter & 1) ? kMaxPayloadV1 : kMaxPayloadV2);
        }
    }
}

TEST(fuzz, structured_header_sweep) {
    // Walk the header fields exhaustively where the space is small enough to walk. Random mutation
    // reaches these eventually; enumerating them means "eventually" is "every run".
    Rng rng(0xD9D9D9D900000003ull);
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    for (int ver_flags = 0; ver_flags < 256; ++ver_flags) {
        for (int lclass_pri = 0; lclass_pri < 256; lclass_pri += 7) {
            std::vector<uint8_t> f(kHeaderSize + 8);
            f[0] = kMagic;
            f[1] = static_cast<uint8_t>(ver_flags);
            f[2] = 0x01;
            f[3] = 0x00;
            f[4] = 0x02;
            f[5] = 0x00;
            f[6] = static_cast<uint8_t>(rng.below(256));
            f[7] = static_cast<uint8_t>(lclass_pri);
            f[8] = 0x00;
            f[9] = 0x00;
            f[10] = 0x00;
            f[11] = 0x00;
            f[12] = 0x00;
            f[13] = 0x00;
            f[14] = 0x08;  // total_len = 8, matching the payload
            f[15] = 0x00;
            std::memcpy(f.data() + kHeaderSize, payload, 8);
            check_one(f);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Differential corpus.
//
// `pot_tests --emit-fuzz-corpus <dir>` writes every fuzz input together with this parser's verdict.
// host/potluck/tests/test_differential.py replays the file through the Python decoder and
// requires an identical verdict for every input.
//
// This is what makes the second implementation earn its keep. The golden-byte tests prove the two
// agree on frames someone thought to write down; a differential corpus proves they agree on tens of
// thousands of frames nobody thought about — which is where a specification actually gets tested,
// because the interesting disagreements are the ones neither author imagined.
// ---------------------------------------------------------------------------------------------

namespace pot_fuzz_corpus {

// Collects inputs during a corpus run instead of checking them.
std::vector<std::vector<uint8_t>>* g_collect = nullptr;
std::vector<uint16_t>* g_collect_caps = nullptr;

void collect(const std::vector<uint8_t>& input, uint16_t cap) {
    if (g_collect != nullptr) {
        g_collect->push_back(input);
        g_collect_caps->push_back(cap);
    }
}

int emit(const char* dir) {
    std::vector<std::vector<uint8_t>> inputs;
    std::vector<uint16_t> caps;
    g_collect = &inputs;
    g_collect_caps = &caps;

    // Re-run the generators in collect mode. Same seeds, so the corpus is byte-identical to what the
    // in-process fuzzer just exercised.
    {
        const auto corpus = seed_corpus();
        for (const auto& c : corpus) {
            collect(c, kMaxPayloadV2);
            collect(c, kMaxPayloadV1);
        }

        Rng rng(0xD9D9D9D900000001ull);
        for (const auto& base : corpus) {
            for (int iter = 0; iter < 400; ++iter) {
                std::vector<uint8_t> m = base;
                const int mutations = 1 + static_cast<int>(rng.below(3));
                for (int k = 0; k < mutations; ++k) {
                    switch (rng.below(6)) {
                        case 0:
                            if (!m.empty()) {
                                const size_t i = rng.below(static_cast<uint32_t>(m.size()));
                                m[i] ^= static_cast<uint8_t>(1u << rng.below(8));
                            }
                            break;
                        case 1:
                            if (!m.empty()) m[rng.below(static_cast<uint32_t>(m.size()))] = rng.byte();
                            break;
                        case 2:
                            if (!m.empty()) m.resize(rng.below(static_cast<uint32_t>(m.size())));
                            break;
                        case 3:
                            for (int n = static_cast<int>(rng.below(16)); n > 0; --n) {
                                m.push_back(rng.byte());
                            }
                            break;
                        case 4:
                            if (m.size() >= kHeaderSize) {
                                static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80,
                                                                      0xFE, 0xFF, 0xD9, 0x10};
                                m[rng.below(kHeaderSize)] = interesting[rng.below(sizeof(interesting))];
                            }
                            break;
                        default:
                            if (m.size() >= 2) {
                                const size_t a = rng.below(static_cast<uint32_t>(m.size()));
                                const size_t b = rng.below(static_cast<uint32_t>(m.size()));
                                const uint8_t t = m[a];
                                m[a] = m[b];
                                m[b] = t;
                            }
                            break;
                    }
                }
                collect(m, (iter & 1) ? kMaxPayloadV1 : kMaxPayloadV2);
            }
        }
    }
    {
        Rng rng(0xD9D9D9D900000002ull);
        for (int iter = 0; iter < 4000; ++iter) {
            size_t n;
            switch (rng.below(4)) {
                case 0: n = rng.below(20); break;
                case 1: n = 16 + rng.below(64); break;
                case 2: n = rng.below(300); break;
                default: n = rng.below(1600); break;
            }
            std::vector<uint8_t> input(n);
            for (size_t i = 0; i < n; ++i) {
                input[i] = rng.byte();
            }
            if (n > 0 && rng.below(2) == 0) {
                input[0] = kMagic;
                if (n > 1 && rng.below(2) == 0) {
                    input[1] = make_ver_flags(kVersion, static_cast<uint8_t>(rng.below(16)));
                }
            }
            collect(input, (iter & 1) ? kMaxPayloadV1 : kMaxPayloadV2);
        }
    }

    g_collect = nullptr;
    g_collect_caps = nullptr;

    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "fuzz_corpus.jsonl";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    for (size_t i = 0; i < inputs.size(); ++i) {
        Frame parsed;
        const FrameError e = parse(inputs[i].empty() ? nullptr : inputs[i].data(), inputs[i].size(),
                                  parsed, caps[i]);

        std::fprintf(f, "{\"cap\":%u,\"raw\":\"", static_cast<unsigned>(caps[i]));
        for (uint8_t b : inputs[i]) {
            std::fprintf(f, "%02x", b);
        }
        std::fprintf(f, "\",\"verdict\":\"%s\"", frame_error_str(e));
        if (e == FrameError::Ok) {
            // Every field, so a disagreement about *which* frame this is also fails, not only a
            // disagreement about whether it is one.
            std::fprintf(f,
                         ",\"src\":%u,\"dst\":%u,\"opcode\":%u,\"lclass\":%u,\"priority\":%u,"
                         "\"seq\":%u,\"msg_id\":%u,\"frag_off\":%u,\"total_len\":%u,"
                         "\"payload_len\":%u,\"frag\":%s,\"ackreq\":%s,\"auth\":%s",
                         parsed.hdr.src, parsed.hdr.dst, parsed.hdr.opcode, parsed.lclass(),
                         parsed.priority(), parsed.hdr.seq, parsed.hdr.msg_id, parsed.hdr.frag_off,
                         parsed.hdr.total_len, parsed.payload_len,
                         parsed.is_frag() ? "true" : "false",
                         parsed.wants_ack() ? "true" : "false",
                         parsed.has_auth() ? "true" : "false");
        }
        std::fprintf(f, "}\n");
    }
    std::fclose(f);
    std::printf("wrote %s (%zu inputs)\n", path.c_str(), inputs.size());
    return 0;
}

}  // namespace pot_fuzz_corpus
