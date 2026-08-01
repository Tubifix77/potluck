// READ / WRITE / REPLY on the wire — ARCHITECTURE.md §5.2, §7.2, §4 rule 2.
//
// The layout asserts live in ns_payloads.hpp, where they fail at compile time. What they cannot
// catch is the two sides of a link disagreeing while each is internally consistent: swap
// `quality` and `latency_class` in both the firmware and the host and every self-consistency test
// still passes, while a real fleet reports the wrong staleness forever.
//
// So this file does two things. It pins the bytes against literals typed from §5.2's table rather
// than produced by the encoder, and it emits a corpus for the Python decoder to agree with
// (tests/test_ns_diff.py). §14 asks for the wire format to be "specified and versioned
// independently of the implementation"; two implementations checked against the same bytes is the
// closest a project this size gets to that.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pot/ns_payloads.hpp"
#include "pot/opcodes.hpp"
#include "pot/sys_resources.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

std::vector<uint8_t> bytes_of(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 0x0F]);
    }
    return s;
}

TEST(ns_wire, read_matches_the_bytes_in_section_5_2) {
    ReadPayload r{};
    r.path_hash = 0x11223344;
    r.flags = 0;
    r.reserved0 = 0;

    // path_hash little-endian, then two zero 16-bit fields. Typed out, not generated.
    const std::vector<uint8_t> want = {0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x00, 0x00};
    CHECK_EQ(hex(bytes_of(&r, sizeof(r))), hex(want));
}

TEST(ns_wire, write_matches_the_bytes_in_section_5_2) {
    WritePayload w{};
    w.path_hash = 0xAABBCCDD;
    w.value_type = static_cast<uint8_t>(ValueType::F32);
    w.value_len = 4;
    w.reserved0 = 0;
    const Value v = Value::of_f32(1.0f);  // 0x3F800000
    std::memcpy(w.value_raw, v.raw, sizeof(w.value_raw));

    const std::vector<uint8_t> want = {
        0xDD, 0xCC, 0xBB, 0xAA,              // path_hash
        0x04, 0x04, 0x00, 0x00,              // value_type=F32, value_len=4, reserved0
        0x00, 0x00, 0x80, 0x3F,              // 1.0f little-endian
        0x00, 0x00, 0x00, 0x00,              // the unused tail of value_raw, zeroed
    };
    CHECK_EQ(hex(bytes_of(&w, sizeof(w))), hex(want));
}

TEST(ns_wire, reply_matches_the_bytes_in_section_5_2) {
    ReplyPayload p{};
    p.path_hash = 0x01020304;
    p.timestamp_ms = 0x000186A0;  // 100000
    p.age_ms = 0x0000000C;        // 12
    p.unit = static_cast<uint16_t>(Unit::Celsius);  // 50 = 0x0032
    p.reply_to = kOpRead;
    p.status = static_cast<uint8_t>(NsError::Ok);
    p.quality = static_cast<uint8_t>(Quality::Stale);
    p.latency_class = 2;
    p.value_type = static_cast<uint8_t>(ValueType::I32);
    p.value_len = 4;
    const Value v = Value::of_i32(-1);
    std::memcpy(p.value_raw, v.raw, sizeof(p.value_raw));

    const std::vector<uint8_t> want = {
        0x04, 0x03, 0x02, 0x01,  // path_hash
        0xA0, 0x86, 0x01, 0x00,  // timestamp_ms
        0x0C, 0x00, 0x00, 0x00,  // age_ms
        0x32, 0x00,              // unit = Celsius
        0x10,                    // reply_to = READ
        0x00,                    // status = Ok
        0x01,                    // quality = Stale
        0x02,                    // latency_class = L2
        0x02,                    // value_type = I32
        0x04,                    // value_len
        0xFF, 0xFF, 0xFF, 0xFF,  // -1
        0x00, 0x00, 0x00, 0x00,
    };
    CHECK_EQ(hex(bytes_of(&p, sizeof(p))), hex(want));
}

TEST(ns_wire, a_stale_reply_still_carries_its_value) {
    // §4 rule 2: "Past the resource's staleness bound, quality becomes STALE and the value is
    // *still delivered*". A reply that dropped the value when marking it stale would satisfy every
    // layout assertion and break the one sentence the section is about.
    Reading r;
    r.value = Value::of_f32(21.5f);
    r.unit = Unit::Celsius;
    r.timestamp_ms = 1000;
    r.age_ms = 9999;
    r.latency_class = 3;
    r.quality = Quality::Stale;

    ReplyPayload p{};
    reply_from_reading(p, 0xDEADBEEF, r, NsError::Ok);
    CHECK_EQ(static_cast<int>(p.quality), static_cast<int>(Quality::Stale));
    CHECK_EQ(static_cast<int>(p.value_type), static_cast<int>(ValueType::F32));
    CHECK_EQ(static_cast<int>(p.value_len), 4);
    CHECK_EQ(static_cast<unsigned>(p.age_ms), 9999u);

    const Reading back = reading_from_reply(p);
    float got = 0.0f;
    Quality q = Quality::NoData;
    CHECK(back.get_f32(got, q));
    CHECK_EQ(static_cast<int>(q), static_cast<int>(Quality::Stale));
    CHECK(got > 21.4f && got < 21.6f);
    CHECK_EQ(static_cast<unsigned>(back.age_ms), 9999u);
}

TEST(ns_wire, an_unavailable_reply_carries_no_value) {
    Reading r;  // default: NoData, no value
    r.quality = Quality::Unavailable;
    ReplyPayload p{};
    reply_from_reading(p, 1, r, NsError::NotFound);
    CHECK_EQ(static_cast<int>(p.value_type), static_cast<int>(ValueType::None));
    CHECK_EQ(static_cast<int>(p.value_len), 0);

    const Reading back = reading_from_reply(p);
    CHECK(!back.usable());
}

TEST(ns_wire, loaders_refuse_a_short_payload) {
    uint8_t buf[32] = {};
    ReadPayload r{};
    WritePayload w{};
    ReplyPayload p{};
    CHECK(!load_read(buf, sizeof(ReadPayload) - 1, r));
    CHECK(!load_write(buf, sizeof(WritePayload) - 1, w));
    CHECK(!load_reply(buf, sizeof(ReplyPayload) - 1, p));
    // Exact length is fine, and so is a longer one: a later milestone appending a field must not
    // make today's decoder reject the frame.
    CHECK(load_read(buf, sizeof(ReadPayload), r));
    CHECK(load_reply(buf, sizeof(ReplyPayload) + 8, p));
}

TEST(ns_wire, every_value_type_round_trips_through_the_wire_form) {
    const Value values[] = {
        Value::of_bool(true),  Value::of_bool(false),
        Value::of_i32(-2147483648LL + 0), Value::of_u32(4294967295u),
        Value::of_f32(-0.5f), Value::of_i64(-1),
        Value::of_u64(0xFFFFFFFFFFFFFFFFull), Value::of_f64(3.5),
    };
    for (const Value& v : values) {
        uint8_t type = 0, len = 0, raw[kValueBytesMax] = {};
        value_to_wire(v, type, len, raw);
        const Value back = value_from_wire(type, len, raw);
        CHECK_EQ(static_cast<int>(back.type), static_cast<int>(v.type));
        CHECK_EQ(static_cast<int>(back.len), static_cast<int>(v.len));
        CHECK_EQ(std::memcmp(back.raw, v.raw, v.len), 0);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Corpus for the Python differential test.
// ---------------------------------------------------------------------------------------------

namespace pot_ns_corpus {

int emit(const char* dir) {
    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "ns_corpus.jsonl";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    auto emit_read = [&](uint32_t h, uint16_t flags) {
        ReadPayload r{};
        r.path_hash = h;
        r.flags = flags;
        std::fprintf(f, "{\"op\":\"read\",\"path_hash\":%lu,\"flags\":%u,\"bytes\":\"%s\"}\n",
                     static_cast<unsigned long>(h), static_cast<unsigned>(flags),
                     hex(bytes_of(&r, sizeof(r))).c_str());
    };

    auto emit_write = [&](uint32_t h, const Value& v) {
        WritePayload w{};
        w.path_hash = h;
        value_to_wire(v, w.value_type, w.value_len, w.value_raw);
        std::fprintf(f,
                     "{\"op\":\"write\",\"path_hash\":%lu,\"value_type\":%u,\"value_len\":%u,"
                     "\"bytes\":\"%s\"}\n",
                     static_cast<unsigned long>(h), static_cast<unsigned>(w.value_type),
                     static_cast<unsigned>(w.value_len), hex(bytes_of(&w, sizeof(w))).c_str());
    };

    auto emit_reply = [&](uint32_t h, const Reading& r, NsError st, uint8_t reply_to) {
        ReplyPayload p{};
        reply_from_reading(p, h, r, st);
        p.reply_to = reply_to;
        std::fprintf(f,
                     "{\"op\":\"reply\",\"path_hash\":%lu,\"timestamp_ms\":%lu,\"age_ms\":%lu,"
                     "\"unit\":%u,\"reply_to\":%u,\"status\":%u,\"quality\":%u,"
                     "\"latency_class\":%u,\"value_type\":%u,\"value_len\":%u,\"bytes\":\"%s\"}\n",
                     static_cast<unsigned long>(p.path_hash),
                     static_cast<unsigned long>(p.timestamp_ms),
                     static_cast<unsigned long>(p.age_ms), static_cast<unsigned>(p.unit),
                     static_cast<unsigned>(p.reply_to), static_cast<unsigned>(p.status),
                     static_cast<unsigned>(p.quality), static_cast<unsigned>(p.latency_class),
                     static_cast<unsigned>(p.value_type), static_cast<unsigned>(p.value_len),
                     hex(bytes_of(&p, sizeof(p))).c_str());
    };

    // READs, including the real built-in paths so the host's own hashing is checked against ours.
    for (uint32_t h : {0u, 1u, 0x11223344u, 0xFFFFFFFFu}) {
        emit_read(h, 0);
    }
    for (uint8_t i = 0; i < kSysResourceCount; ++i) {
        emit_read(sys_resource_hash(0x1A2B, static_cast<SysResource>(i)), 0);
    }

    // WRITEs across every value type, including the boundary numbers where a sign or a width bug
    // shows up.
    const Value vals[] = {
        Value::of_bool(true),
        Value::of_bool(false),
        Value::of_i32(0),
        Value::of_i32(-1),
        Value::of_i32(2147483647),
        Value::of_u32(0),
        Value::of_u32(4294967295u),
        Value::of_f32(0.0f),
        Value::of_f32(-0.5f),
        Value::of_f32(3.4028235e38f),
        Value::of_i64(-9223372036854775807LL - 1),
        Value::of_u64(0xFFFFFFFFFFFFFFFFull),
        Value::of_f64(-2.25),
    };
    for (const Value& v : vals) {
        emit_write(0xCAFEBABE, v);
    }

    // REPLIES over every quality and a spread of errors — the combinations §4 rule 2 turns on.
    const Quality qualities[] = {Quality::Good, Quality::Stale, Quality::Unavailable,
                                 Quality::NoData, Quality::Faulty};
    const NsError errors[] = {NsError::Ok, NsError::NotFound, NsError::NotWritable,
                              NsError::TypeMismatch, NsError::WrongOwner,
                              NsError::EventNotCached};
    for (Quality q : qualities) {
        for (NsError e : errors) {
            Reading r;
            r.value = Value::of_f32(21.5f);
            r.unit = Unit::Celsius;
            r.timestamp_ms = 123456;
            r.age_ms = 789;
            r.latency_class = 3;
            r.quality = q;
            emit_reply(0x0BADF00D, r, e, kOpRead);
            emit_reply(0x0BADF00D, r, e, kOpWrite);
        }
    }
    // And a reply for each unit that appears in the built-ins, so a unit renumbering is caught.
    for (Unit u : {Unit::None, Unit::Byte, Unit::Second, Unit::Count, Unit::Volt, Unit::Celsius}) {
        Reading r;
        r.value = Value::of_u32(42);
        r.unit = u;
        r.timestamp_ms = 7;
        r.age_ms = 0;
        r.latency_class = 1;
        r.quality = Quality::Good;
        emit_reply(0x00000007, r, NsError::Ok, kOpRead);
    }

    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
    return 0;
}

}  // namespace pot_ns_corpus
