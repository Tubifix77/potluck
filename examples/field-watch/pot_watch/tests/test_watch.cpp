// The field watch tested with Potluck's test kit: its inputs fed on the rig's node, the clock moved by the test.

#include <cstring>

#include "pot/testkit.hpp"
#include "test_harness.hpp"
#include "watch/watch.hpp"

using namespace pot;

namespace {
const char* kOut = "potluck://lab/act/field_watch/out";
const char* kAgree = "potluck://lab/act/field_watch/agree";
const char* kMissing = "potluck://lab/act/field_watch/missing";
const char* kPresence = "potluck://lab/act/rf_fusion_room/out";
const char* kMotion = "potluck://lab/act/tablet_camera/front_motion";
const char* kMotionB = "potluck://lab/act/tablet_camera/back_motion";
const char* kSound = "potluck://lab/act/tablet/sound_dbfs";

// What potluck.deploy compiles for the descriptor: the portable header, then the watch's own fields.
size_t decl(uint8_t* buf, size_t cap, uint16_t hold_ms = 10000, const char* motion_b = kMotionB) {
    TickerConfig h{};
    h.out_hash = path_hash(kOut);
    h.period_ms = 500;
    h.count = 3;
    h.node[0] = 0x6300, h.node[1] = 0x7368, h.node[2] = 0x0100;
    h.gravity[0] = h.gravity[1] = h.gravity[2] = 1;
    size_t n = encode_ticker_config(h, buf, cap);
    auto put = [&](const void* p, size_t k) {
        std::memcpy(buf + n, p, k);
        n += k;
    };
    const uint32_t outputs = path_hash("potluck://lab/act/field_watch/");
    const uint32_t presence = path_hash(kPresence), motion = path_hash(kMotion), mb = path_hash(motion_b),
                   sound = path_hash(kSound);
    const float pmin = 0.5f, mmin = 5.0f, smin = -35.0f;
    const uint8_t led = 1;
    put(&outputs, 4), put(&presence, 4), put(&pmin, 4), put(&motion, 4), put(&mb, 4), put(&mmin, 4);
    put(&sound, 4), put(&smin, 4), put(&hold_ms, 2), put(&led, 1);
    return n;
}

// The rig as the reconciler leaves it on the owner: the outputs declared, the actor started.
bool start(testkit::Rig& rig, uint16_t hold_ms = 10000, const char* motion_b = kMotionB) {
    uint8_t cfg[kMaxActorCfg];
    rig.declare(kPortableNode, 0xA0, cfg, decl(cfg, sizeof(cfg), hold_ms, motion_b));
    if (!rig.start(watch::watch_row(), nullptr, true)) return false;
    NsDecl out[kMaxOutputs];
    const size_t n = rig.outputs(out, kMaxOutputs, 0);
    for (size_t i = 0; i < n; ++i) {
        out[i].owner_node = rig.node().config().node_id;
        rig.node().ns().declare(out[i]);
    }
    return n == 3;
}

void quiet(testkit::Rig& rig) {
    rig.feed(kPresence, Value::of_bool(false), ValueType::Bool, Unit::None, 5000);
    rig.feed(kMotion, Value::of_f32(1.0f), ValueType::F32, Unit::None, 5000);
    rig.feed(kMotionB, Value::of_f32(0.0f), ValueType::F32, Unit::None, 5000);
    rig.feed(kSound, Value::of_f32(-48.0f), ValueType::F32, Unit::None, 5000);
}

uint32_t u32(testkit::Rig& rig, const char* path) {
    uint32_t v = 999;
    rig.read(path).value.as_u32(v);
    return v;
}
bool alarm(testkit::Rig& rig) {
    bool v = false;
    rig.read(kOut).value.as_bool(v);
    return v;
}
}  // namespace

TEST(field_watch, one_sense_alone_is_no_alarm_and_two_are) {
    testkit::Rig rig;
    CHECK(start(rig));
    quiet(rig);
    rig.advance(600);
    CHECK(!alarm(rig));
    CHECK_EQ(u32(rig, kAgree), 0u);
    CHECK_EQ(u32(rig, kMissing), 0u);
    rig.feed(kPresence, Value::of_bool(true), ValueType::Bool, Unit::None, 5000);
    rig.advance(600);
    CHECK(!alarm(rig));
    CHECK_EQ(u32(rig, kAgree), watch::kPresence);
    CHECK(!rig.led.on);
    rig.feed(kSound, Value::of_f32(-20.0f), ValueType::F32, Unit::None, 5000);
    rig.advance(600);
    CHECK(alarm(rig));
    CHECK_EQ(u32(rig, kAgree), watch::kPresence | watch::kSound);
    CHECK(rig.led.on && rig.led.r == 0 && rig.led.g == 0xFF && rig.led.b == 0xFF);  // cyan, never red
    CHECK(rig.led.blink_period_ms > 0);
}

TEST(field_watch, senses_a_few_seconds_apart_still_agree_within_the_hold) {
    testkit::Rig rig;
    CHECK(start(rig));
    quiet(rig);
    rig.feed(kMotion, Value::of_f32(30.0f), ValueType::F32, Unit::None, 5000);  // someone passes the camera
    rig.advance(600);
    rig.feed(kMotion, Value::of_f32(1.0f), ValueType::F32, Unit::None, 5000);   // and is gone from its view
    rig.advance(4000);
    rig.feed(kPresence, Value::of_bool(true), ValueType::Bool, Unit::None, 5000);  // the radio links notice later
    rig.advance(600);
    CHECK(alarm(rig));
    CHECK_EQ(u32(rig, kAgree), watch::kPresence | watch::kMotion);
    rig.feed(kPresence, Value::of_bool(false), ValueType::Bool, Unit::None, 5000);
    rig.advance(11000);  // past the hold for both
    CHECK(!alarm(rig));
    CHECK(!rig.led.on);  // cleared at once, not left to lapse
}

TEST(field_watch, the_larger_of_two_cameras_counts) {
    testkit::Rig rig;
    CHECK(start(rig));
    quiet(rig);
    rig.feed(kMotionB, Value::of_f32(12.0f), ValueType::F32, Unit::None, 5000);
    rig.advance(600);
    CHECK_EQ(u32(rig, kAgree), watch::kMotion);
}

TEST(field_watch, a_missing_input_counts_as_off_and_is_named) {
    testkit::Rig rig;
    CHECK(start(rig));
    rig.feed(kPresence, Value::of_bool(true), ValueType::Bool, Unit::None, 5000);
    rig.feed(kMotion, Value::of_f32(30.0f), ValueType::F32, Unit::None, 5000);
    rig.feed(kMotionB, Value::of_f32(0.0f), ValueType::F32, Unit::None, 5000);
    // the sound was never published: NO_DATA
    rig.advance(600);
    CHECK(alarm(rig));
    CHECK_EQ(u32(rig, kMissing), watch::kSound);
    rig.advance(6000);  // the camera's value goes STALE past its 5 s bound: it no longer holds the alarm up
    rig.feed(kPresence, Value::of_bool(true), ValueType::Bool, Unit::None, 5000);
    rig.advance(600);
    CHECK(!alarm(rig));
    CHECK_EQ(u32(rig, kMissing), watch::kMotion | watch::kSound);
}

TEST(field_watch, a_watch_that_moves_away_leaves_the_led_dark) {
    testkit::Rig rig;
    CHECK(start(rig));
    quiet(rig);
    rig.feed(kPresence, Value::of_bool(true), ValueType::Bool, Unit::None, 5000);
    rig.feed(kSound, Value::of_f32(-10.0f), ValueType::F32, Unit::None, 5000);
    rig.advance(600);
    CHECK(rig.led.on);
    rig.stop();  // the reconciler fenced it here: another board runs it now
    CHECK(!rig.led.on);
}

TEST(field_watch, one_camera_is_given_as_the_same_path_twice) {
    testkit::Rig rig;
    CHECK(start(rig, 10000, kMotion));
    quiet(rig);
    rig.feed(kMotion, Value::of_f32(9.0f), ValueType::F32, Unit::None, 5000);
    rig.advance(600);
    CHECK_EQ(u32(rig, kAgree), watch::kMotion);
}

TEST(field_watch, a_hold_shorter_than_its_period_is_refused) {
    testkit::Rig rig;
    uint8_t cfg[kMaxActorCfg];
    rig.declare(kPortableNode, 0xA0, cfg, decl(cfg, sizeof(cfg), 100));
    const char* why = nullptr;
    CHECK(!rig.start(watch::watch_row(), &why, true));
    CHECK(why != nullptr);
}
