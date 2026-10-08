// M8.1: the die-temperature actor against a fake sensor. What a reader gets -- value, unit, quality --
// when the sensor reads, when it fails, and when it recovers.

#include <cstring>
#include <new>

#include "pot/die_temp.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_t = 0;

struct FakeSensor {
    bool open_ok = true;
    bool read_ok = true;
    float c = 41.5f;
    int opens = 0;
    static bool open(void* ctx) {
        FakeSensor* s = static_cast<FakeSensor*>(ctx);
        ++s->opens;
        return s->open_ok;
    }
    static bool read(void* ctx, float* out) {
        FakeSensor* s = static_cast<FakeSensor*>(ctx);
        if (!s->read_ok) return false;
        *out = s->c;
        return true;
    }
};

struct Rig {
    NodeHal hal{};
    Node* node = nullptr;
    FakeSensor sensor;
    DieTempActor* actor = nullptr;
    alignas(DieTempActor) unsigned char mem[sizeof(DieTempActor)];
    const uint32_t out = path_hash("potluck://lab/node-0100/hw/die_temp");
    Rig() {
        NodeConfig cfg;
        cfg.node_id = 0x100;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        hal.now_ms = [](void*) { return g_t; };
        hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node = new Node(cfg, hal);
        DieTempDriver drv;
        drv.ctx = &sensor;
        drv.open = &FakeSensor::open;
        drv.read = &FakeSensor::read;
        actor = new (mem) DieTempActor(*node, DieTempConfig{out, 1000}, drv);
    }
    ~Rig() {
        actor->~DieTempActor();
        delete node;
    }
    Reading read() {
        Reading r;
        node->read(out, r);
        return r;
    }
    void run_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) actor->tick(++g_t);
    }
};

}  // namespace

TEST(die_temp, publishes_the_die_temperature_in_celsius_with_its_age) {
    g_t = 0;
    Rig r;
    CHECK(r.actor->start(g_t));
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::NoData));  // nothing read yet
    r.run_ms(1);
    const Reading a = r.read();
    CHECK_EQ(static_cast<int>(a.quality), static_cast<int>(Quality::Good));
    CHECK_EQ(static_cast<int>(a.value.type), static_cast<int>(ValueType::F32));
    CHECK_EQ(static_cast<int>(a.unit), static_cast<int>(Unit::Celsius));
    float c = 0;
    std::memcpy(&c, a.value.raw, sizeof(c));
    CHECK(c == 41.5f);
    r.run_ms(2500);
    CHECK_EQ(r.actor->stats().reads, 3u);  // one per second
    CHECK(r.read().age_ms < 1000u);
}

TEST(die_temp, a_failed_read_is_faulty_with_no_number_never_the_old_one) {
    g_t = 0;
    Rig r;
    CHECK(r.actor->start(g_t));
    r.run_ms(1);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Good));
    r.sensor.read_ok = false;
    r.run_ms(1000);
    const Reading f = r.read();
    CHECK_EQ(static_cast<int>(f.quality), static_cast<int>(Quality::Faulty));
    CHECK_EQ(static_cast<int>(f.value.type), static_cast<int>(ValueType::None));  // no value travels
    CHECK_EQ(r.actor->stats().errors, 1u);
    r.sensor.read_ok = true;  // and it recovers on the next good read
    r.sensor.c = 43.0f;
    r.run_ms(1000);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Good));
}

TEST(die_temp, a_sensor_that_will_not_open_reads_faulty_from_the_start_and_is_retried) {
    g_t = 0;
    Rig r;
    r.sensor.open_ok = false;
    CHECK(r.actor->start(g_t));  // the deployment is not refused for it
    r.run_ms(1);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Faulty));  // not NO_DATA
    r.sensor.open_ok = true;
    r.run_ms(1000);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Good));
    CHECK(r.sensor.opens >= 3);
}

TEST(die_temp, the_console_instrument_forces_faulty_and_releases_it) {
    g_t = 0;
    Rig r;
    CHECK(r.actor->start(g_t));
    const char on[] = "POT! die_temp fail 1";
    const char off[] = "POT! die_temp fail 0";
    CHECK(!r.actor->on_console("POT! busy 1", 11));  // not its line
    CHECK(r.actor->on_console(on, sizeof(on) - 1));
    r.run_ms(1);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Faulty));
    CHECK(r.actor->on_console(off, sizeof(off) - 1));
    r.run_ms(1000);
    CHECK_EQ(static_cast<int>(r.read().quality), static_cast<int>(Quality::Good));
    char line[256];
    CHECK(r.actor->stats_json(line, sizeof(line), g_t) > 0);
    CHECK(std::strstr(line, "\"t\":\"die_temp\"") != nullptr);
    CHECK(std::strstr(line, "\"quality\":\"GOOD\"") != nullptr);
}
