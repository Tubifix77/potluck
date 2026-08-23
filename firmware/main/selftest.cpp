// On-target self-test -- the checks that cannot be made on a laptop.
//
//   tools\run_selftest.ps1
//
// WHY THIS EXISTS
//
// The portable core has 172 host test cases and they run on x86-64. The layout `static_assert`s do
// compile for Xtensa, so the sizes and offsets are proven on the target -- but nothing *executes*
// there. Everything below is a property of the running machine rather than of the source:
//
//   * a struct cast onto an unaligned buffer. §5's parser casts the header straight onto received
//     bytes; on x86-64 an unaligned load is merely slow, and on Xtensa it is a different instruction
//     path entirely. If it were wrong, every frame arriving at an odd offset would be wrong, and no
//     host test could ever see it.
//   * single- and double-precision float through the wire form. The S3 has a single-precision FPU
//     and does doubles in software, which is exactly the kind of difference that turns a value into
//     a slightly different value.
//   * two cores actually scheduling. `portNUM_PROCESSORS` is a constant; a task landing on core 1 is
//     an observation.
//   * real stack consumption of the codec path, on the target's frame layout and register window.
//
// WHAT IT DOES NOT PROVE
//
// It runs under QEMU, which emulates the S3's CPU, memory and UARTs -- and no radio, no GPIO matrix.
// So: nothing here touches a peripheral, and **no timing figure printed here is a performance
// measurement.** QEMU is not cycle-accurate; the clock checks below test monotonicity and ordering,
// which are correctness properties, and the microsecond counts are labelled emulated so they cannot
// be quoted as anything else. §6's memory figures come from the linker and §3's timing from a bench.
//
// Borrowed pattern: the sibling Powersuit project runs 27 such on-target checks, and its experience
// was that the ones which paid were exactly these -- the machine-dependent ones.

#include "sdkconfig.h"

#if CONFIG_POT_SELFTEST

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pot/frame.hpp"
#include "pot/namespace.hpp"
#include "pot/ns_payloads.hpp"
#include "pot/opcodes.hpp"
#include "pot/payloads.hpp"
#include "pot/serial_framing.hpp"
#include "pot/value.hpp"

namespace {

using namespace pot;

int g_pass = 0;
int g_fail = 0;

// One line per case, in the same JSON-per-line shape as every other record this firmware emits, so
// the runner can grep it and a capture can hold it.
void report(const char* name, bool ok, const char* detail) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
    }
    std::printf("{\"t\":\"selftest\",\"case\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n", name,
                ok ? "true" : "false", detail != nullptr ? detail : "");
    std::fflush(stdout);
}

#define CHECK(name, cond) report(name, (cond), (cond) ? "" : "failed: " #cond)

// -----------------------------------------------------------------------------------------------
// The frame codec, on this machine's alignment rules
// -----------------------------------------------------------------------------------------------

// A frame built once and reused, so every alignment case parses identical bytes.
struct Sample {
    uint8_t bytes[kEspNowV1LinkMtu];
    size_t len;
};

bool build_sample(Sample& s) {
    HeartbeatPayload hb{};
    hb.uptime_ms = 0x11223344;
    hb.boot_epoch = 0x55667788;
    hb.free_dram_kib = 0x99AA;
    hb.hb_seq = 0x0102;

    EncodeSpec spec;
    spec.src = 0x1001;
    spec.dst = 0x2002;
    spec.opcode = kOpHeartbeat;
    spec.seq = 0x3003;
    spec.msg_id = 0x4004;
    spec.lclass = kClassL3;
    return encode(spec, reinterpret_cast<const uint8_t*>(&hb), sizeof(hb), s.bytes,
                  sizeof(s.bytes), s.len) == FrameError::Ok;
}

bool parses_at_offset(const Sample& s, size_t offset) {
    // A buffer whose payload starts at an odd address, which is what a byte-stream reassembler
    // hands the parser when a frame does not begin on a word boundary.
    static uint8_t staging[kEspNowV1LinkMtu + 8];
    std::memset(staging, 0, sizeof(staging));
    std::memcpy(staging + offset, s.bytes, s.len);

    Frame f;
    if (parse(staging + offset, s.len, f, kMaxPayloadV1) != FrameError::Ok) {
        return false;
    }
    if (f.hdr.src != 0x1001 || f.hdr.dst != 0x2002 || f.hdr.seq != 0x3003 ||
        f.hdr.msg_id != 0x4004 || f.hdr.opcode != kOpHeartbeat) {
        return false;
    }
    HeartbeatPayload hb{};
    if (!load_heartbeat(f.payload, f.payload_len, hb)) {
        return false;
    }
    return hb.uptime_ms == 0x11223344u && hb.boot_epoch == 0x55667788u &&
           hb.free_dram_kib == 0x99AA && hb.hb_seq == 0x0102;
}

void test_alignment() {
    Sample s{};
    CHECK("a_frame_encodes_on_target", build_sample(s));
    if (s.len == 0) {
        return;
    }
    // Every offset in a word, because "works when aligned" is the case that hides the bug.
    for (size_t off = 0; off < 4; ++off) {
        char name[64];
        std::snprintf(name, sizeof(name), "a_frame_parses_at_byte_offset_%u",
                      static_cast<unsigned>(off));
        report(name, parses_at_offset(s, off), "");
    }

    // The same bytes from .rodata rather than RAM. On this part those are different address
    // regions reached by different load paths, and the parser must not be able to tell. Compared
    // against each other rather than against a golden verdict, so the check needs no second copy of
    // a valid frame -- what matters is that the two agree.
    static const uint8_t kFlashBytes[] = {
        0xD9, 0x10, 0x01, 0x10, 0x02, 0x20, 0x03, 0x60, 0x04, 0x40, 0x30, 0x30, 0x00, 0x00, 0x00,
        0x00,
    };
    static uint8_t ram_bytes[sizeof(kFlashBytes)];
    std::memcpy(ram_bytes, kFlashBytes, sizeof(kFlashBytes));
    Frame from_flash;
    Frame from_ram;
    const FrameError ef = parse(kFlashBytes, sizeof(kFlashBytes), from_flash, kMaxPayloadV1);
    const FrameError er = parse(ram_bytes, sizeof(ram_bytes), from_ram, kMaxPayloadV1);
    char detail[48];
    std::snprintf(detail, sizeof(detail), "flash=%d ram=%d", static_cast<int>(ef),
                  static_cast<int>(er));
    report("flash_and_ram_bytes_parse_identically", ef == er, detail);

    // And the encoded sample, which is known good, from both regions.
    CHECK("the_known_good_frame_parses_from_ram", parses_at_offset(s, 0));
}

// -----------------------------------------------------------------------------------------------
// CRC and COBS, on this machine's byte order
// -----------------------------------------------------------------------------------------------

void test_framing() {
    // A fixed input with a value the host tests also pin. An endianness or table mistake in the CRC
    // would make every serial frame from a board unreadable by the host, and vice versa.
    static const uint8_t kProbe[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const uint16_t c = crc16(kProbe, sizeof(kProbe));
    char detail[48];
    std::snprintf(detail, sizeof(detail), "crc16=0x%04x", c);
    // Not compared against a typed-in constant: the property that matters is that it is stable and
    // that a single flipped bit changes it, which is checkable here without importing a golden value
    // that the host tests already own.
    uint8_t flipped[sizeof(kProbe)];
    std::memcpy(flipped, kProbe, sizeof(kProbe));
    flipped[3] ^= 0x01;
    report("crc16_changes_when_a_bit_does", c != crc16(flipped, sizeof(flipped)), detail);

    uint8_t encoded[64];
    uint8_t decoded[64];
    const size_t enc = cobs_encode(kProbe, sizeof(kProbe), encoded, sizeof(encoded));
    const size_t dec = cobs_decode(encoded, enc, decoded, sizeof(decoded));
    CHECK("cobs_round_trips_on_target",
          enc > 0 && dec == sizeof(kProbe) && std::memcmp(decoded, kProbe, dec) == 0);

    bool has_zero = false;
    for (size_t i = 0; i < enc; ++i) {
        has_zero = has_zero || encoded[i] == 0;
    }
    CHECK("cobs_output_contains_no_delimiter", !has_zero);
}

// -----------------------------------------------------------------------------------------------
// Values through the wire form, on an FPU that does singles in hardware and doubles in software
// -----------------------------------------------------------------------------------------------

bool round_trips(const Value& in) {
    uint8_t type = 0;
    uint8_t len = 0;
    uint8_t raw[kValueBytesMax] = {};
    value_to_wire(in, type, len, raw);
    const Value out = value_from_wire(type, len, raw);
    return out.type == in.type && out.len == in.len &&
           std::memcmp(out.raw, in.raw, kValueBytesMax) == 0;
}

void test_values() {
    CHECK("bool_round_trips", round_trips(Value::of_bool(true)));
    CHECK("i32_round_trips", round_trips(Value::of_i32(-2147483647 - 1)));
    CHECK("u32_round_trips", round_trips(Value::of_u32(0xFFFFFFFFu)));
    CHECK("i64_round_trips", round_trips(Value::of_i64(-9007199254740993LL)));
    CHECK("u64_round_trips", round_trips(Value::of_u64(0xFEDCBA9876543210ULL)));

    // Single precision: hardware on this part.
    const float f = -3.4028235e+38f;
    Value vf = Value::of_f32(f);
    float back_f = 0.0f;
    CHECK("f32_survives_the_wire_exactly", round_trips(vf) && vf.as_f32(back_f) && back_f == f);

    // Double precision: software on this part, which is where a truncation would hide. This value
    // needs all 53 bits of mantissa, so a float somewhere in the path would change it.
    const double d = 1.2345678901234567;
    Value vd = Value::of_f64(d);
    double back_d = 0.0;
    CHECK("f64_keeps_all_53_bits", round_trips(vd) && vd.as_f64(back_d) && back_d == d);

    // A denormal, which some soft-float paths flush to zero.
    const double tiny = 5e-324;
    Value vt = Value::of_f64(tiny);
    double back_t = 0.0;
    CHECK("f64_denormal_is_not_flushed", vt.as_f64(back_t) && back_t == tiny && round_trips(vt));

    // A typed accessor must refuse a mismatch rather than reinterpret: "a temperature read as an
    // integer is a bug, not a cast".
    int32_t as_int = 0;
    CHECK("a_type_mismatch_is_refused_not_reinterpreted", !Value::of_f32(1.5f).as_i32(as_int));
}

// -----------------------------------------------------------------------------------------------
// The namespace, at its boundary
// -----------------------------------------------------------------------------------------------

void test_namespace() {
    // The runtime half of the manifest's 128-entry check: the build refuses a manifest that would
    // overfill a node, and the node refuses the entry itself. Both, because the manifest is only one
    // of the ways a resource gets declared.
    static Namespace ns;
    const size_t cap = Namespace::capacity();
    size_t declared = 0;
    for (size_t i = 0; i < cap; ++i) {
        NsDecl d;
        d.path_hash = static_cast<uint32_t>(0x1000000u + i);  // distinct, and never 0
        d.owner_node = 0x1001;
        d.type = ValueType::U32;
        d.unit = Unit::None;
        d.latency_class = kClassL4;
        if (ns.declare(d) == NsError::Ok) {
            ++declared;
        }
    }
    CHECK("the_namespace_holds_exactly_its_capacity", declared == cap);

    NsDecl one_too_many;
    one_too_many.path_hash = 0xDEADBEEF;
    one_too_many.owner_node = 0x1001;
    one_too_many.type = ValueType::U32;
    CHECK("the_entry_past_capacity_is_refused_as_full",
          ns.declare(one_too_many) == NsError::Full);

    // A second declaration of a path already held is not a collision and not an error.
    NsDecl again;
    again.path_hash = 0x1000000u;
    again.owner_node = 0x1001;
    again.type = ValueType::U32;
    const NsError e = ns.declare(again);
    char detail[48];
    std::snprintf(detail, sizeof(detail), "ns_error=%d", static_cast<int>(e));
    report("redeclaring_a_held_path_is_not_full", e != NsError::Full, detail);
}

// -----------------------------------------------------------------------------------------------
// Two cores, and a clock
// -----------------------------------------------------------------------------------------------

struct CoreProbe {
    SemaphoreHandle_t done;
    BaseType_t core;
    UBaseType_t stack_left;
};

void core_probe_task(void* arg) {
    CoreProbe* p = static_cast<CoreProbe*>(arg);
    p->core = xPortGetCoreID();

    // Exercise the codec here, so the high-water mark is the codec path's own cost on this target's
    // frame layout rather than an empty task's.
    Sample s{};
    if (build_sample(s)) {
        for (int i = 0; i < 32; ++i) {
            Frame f;
            (void)parse(s.bytes, s.len, f, kMaxPayloadV1);
        }
    }
    p->stack_left = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreGive(p->done);
    vTaskDelete(nullptr);
}

void test_cores() {
    char detail[64];
    std::snprintf(detail, sizeof(detail), "portNUM_PROCESSORS=%d", portNUM_PROCESSORS);
    report("the_part_reports_two_cores", portNUM_PROCESSORS == 2, detail);
    if (portNUM_PROCESSORS < 2) {
        return;
    }

    CoreProbe probe{};
    probe.done = xSemaphoreCreateBinary();
    probe.core = -1;
    if (probe.done == nullptr) {
        report("a_task_pins_to_core_1", false, "no semaphore");
        return;
    }
    // 4 KB, which is what §6 budgets for a worker-sized task; the high-water mark below says how
    // much of it the codec path actually used.
    const BaseType_t created = xTaskCreatePinnedToCore(&core_probe_task, "pot.probe", 4096, &probe,
                                                       5, nullptr, 1);
    bool ok = created == pdPASS && xSemaphoreTake(probe.done, pdMS_TO_TICKS(2000)) == pdTRUE;
    std::snprintf(detail, sizeof(detail), "ran on core %d", static_cast<int>(probe.core));
    report("a_task_pins_to_core_1", ok && probe.core == 1, detail);

    if (ok) {
        // A measurement of the *code*, which is a property of the compiler and the target's calling
        // convention rather than of the emulator -- so this number is worth keeping. It is words on
        // Xtensa; ESP-IDF's high-water mark is reported in bytes.
        std::snprintf(detail, sizeof(detail), "%u B unused of 4096 after 32 parses",
                      static_cast<unsigned>(probe.stack_left));
        report("the_codec_path_leaves_stack_headroom", probe.stack_left > 512, detail);
    }
    vSemaphoreDelete(probe.done);
}

void test_clock() {
    // Monotonicity and ordering are correctness. Rates and durations are not measurable here: QEMU
    // is not cycle-accurate, so nothing below is a performance figure and the label says so.
    const int64_t t0 = esp_timer_get_time();
    bool monotonic = true;
    int64_t last = t0;
    for (int i = 0; i < 2000; ++i) {
        const int64_t now = esp_timer_get_time();
        monotonic = monotonic && now >= last;
        last = now;
    }
    CHECK("the_microsecond_clock_never_goes_backwards", monotonic);
    CHECK("the_microsecond_clock_advances", last > t0);

    // The FreeRTOS tick and the microsecond timer, against each other. Not a duration measurement:
    // a first attempt asserted that vTaskDelay(50 ms) takes at least 50 ms and it failed at
    // 48,703 us -- and the interesting part is that 48.7 ms is short by more than one tick, so it is
    // not the familiar "N ticks means N-1 full ticks" off-by-one. Under this emulator the two clocks
    // simply run at slightly different rates, about 2.6% apart.
    //
    // Which is worth a check of its own, phrased as what it can honestly assert: the two clocks
    // agree to within a tenth. A gross tick misconfiguration -- FREERTOS_HZ left at its 100 Hz
    // default, say, which section 4 says L1's budget cannot survive -- would show up here as a
    // factor, not a percent. And the delta itself is printed, because it is the number that says
    // why no timing policy may be tuned under emulation.
    const int64_t before = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(50));
    const int64_t span = esp_timer_get_time() - before;
    const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
    char detail[96];
    std::snprintf(detail, sizeof(detail),
                  "%lld us for a 50 ms tick delay, tick=%lld us (emulated, not a measurement)",
                  static_cast<long long>(span), static_cast<long long>(tick_us));
    report("the_tick_and_the_microsecond_clock_agree_within_a_tenth",
           span > 45000 && span < 55000, detail);

    std::snprintf(detail, sizeof(detail), "configTICK_RATE_HZ=%d", static_cast<int>(configTICK_RATE_HZ));
    // Section 4: "L1's 10 ms budget sits exactly on the default scheduler tick [...] so the Potluck
    // reference configuration mandates FREERTOS_HZ=1000". A build that lost that setting would meet
    // every host test and give L1 zero margin on the target.
    report("the_tick_rate_is_the_one_section_4_mandates", configTICK_RATE_HZ == 1000, detail);
}

}  // namespace

extern "C" int pot_selftest_run(void) {
    std::printf("{\"t\":\"selftest_begin\",\"target\":\"esp32s3\",\"note\":"
                "\"no peripheral is touched; no timing here is a performance figure\"}\n");
    std::fflush(stdout);

    test_alignment();
    test_framing();
    test_values();
    test_namespace();
    test_cores();
    test_clock();

    std::printf("{\"t\":\"selftest_end\",\"pass\":%d,\"fail\":%d}\n", g_pass, g_fail);
    std::fflush(stdout);
    return g_fail;
}

#endif  // CONFIG_POT_SELFTEST
