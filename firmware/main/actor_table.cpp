// M8.1: the registration table. See actor_table.hpp.

#include "actor_table.hpp"

#include "sdkconfig.h"

#include <cstdlib>
#include <new>

#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "pot/die_temp.hpp"
#if CONFIG_POT_WASM
#include "pot/guest.hpp"
#endif
#include "pot/mc.hpp"
#include "pot/svc_client.hpp"
#include "pot/ticker.hpp"

namespace pot {
namespace {

const char* kTag = "pot.actors";

// ---- led (M3): the status LED's healthy-state appearance -------------------------------------------
class LedActor : public Actor {
  public:
    explicit LedActor(const LedConfig& c) : cfg_(c) {}
    bool start(uint32_t) override {
        board::set_led_healthy(cfg_);
        return true;
    }

  private:
    LedConfig cfg_;
};

bool led_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    LedConfig c{};
    if (!led_config(d, c)) {
        *why = "led actor config out of range";
        return false;
    }
    return true;
}
Actor* led_create(void* mem, const ActorDecl& d, const ActorEnv&) {
    static_assert(sizeof(LedActor) <= kActorSlotBytes, "LedActor outgrew its actor slot");
    LedConfig c{};
    return led_config(d, c) ? new (mem) LedActor(c) : nullptr;
}

// ---- fault (M3): aborts the node after a delay, to exercise trial-and-revert -----------------------
class FaultActor : public Actor {
  public:
    explicit FaultActor(uint32_t at) : at_ms_(at) {}
    bool start(uint32_t) override { return true; }
    void tick(uint32_t now) override {
        if (now >= at_ms_) {
            ESP_LOGE(kTag, "fault actor firing at %u ms, as configured", static_cast<unsigned>(now));
            abort();
        }
    }

  private:
    uint32_t at_ms_;
};

bool fault_check(const ActorDecl& d, const ActorEnv& env, const char** why) {
    FaultConfig c{};
    if (!fault_config(d, c) || c.panic_after_ms >= env.trial_window_ms) {
        // A fault that fires after the trial window would let the node confirm a module that then
        // crashes for ever, with no fallback slot left. Refused as invalid.
        *why = "fault actor must fire inside the trial window";
        return false;
    }
    return true;
}
Actor* fault_create(void* mem, const ActorDecl& d, const ActorEnv&) {
    static_assert(sizeof(FaultActor) <= kActorSlotBytes, "FaultActor outgrew its actor slot");
    FaultConfig c{};
    return fault_config(d, c) ? new (mem) FaultActor(c.panic_after_ms) : nullptr;
}

// ---- die_temp (M8.1): the chip's own temperature sensor, over ESP-IDF's temperature_sensor driver --
// One sensor per chip, so one handle: a second die_temp actor on a node shares it. The driver is not
// thread-safe (ESP-IDF says so); every call here is from the link task.
temperature_sensor_handle_t g_tsens = nullptr;

bool tsens_open(void*) {
    if (g_tsens != nullptr) return true;
    // -10..80 degC: the range ESP-IDF v6.0.2's S3 table gives the smallest error (1 degC). The driver
    // picks its internal setting from the range asked for.
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_handle_t h = nullptr;
    if (temperature_sensor_install(&cfg, &h) != ESP_OK) return false;
    if (temperature_sensor_enable(h) != ESP_OK) {
        temperature_sensor_uninstall(h);
        return false;
    }
    g_tsens = h;
    return true;
}

bool tsens_read(void*, float* c) { return g_tsens != nullptr && temperature_sensor_get_celsius(g_tsens, c) == ESP_OK; }

Actor* die_temp_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(DieTempActor) <= kActorSlotBytes, "DieTempActor outgrew its actor slot");
    DieTempConfig c{};
    if (env.node == nullptr || !die_temp_config(d, c)) return nullptr;
    DieTempDriver drv;
    drv.open = &tsens_open;
    drv.read = &tsens_read;
    return new (mem) DieTempActor(*env.node, c, drv);
}

}  // namespace

namespace app {
namespace {
const ActorKind kBuiltinRows[] = {
    {ActorType::Led, "led", &led_check, &led_create},
    {ActorType::Fault, "fault", &fault_check, &fault_create},
    kSvcClientKind,
    kTickerKind,  // portable: the reconciler places it
    {ActorType::DieTemp, "die_temp", &die_temp_check, &die_temp_create, nullptr, &die_temp_outputs},
    kMcLenderKind,  // M9
    kMcJobKind,
#if CONFIG_POT_WASM
    kGuestKind,  // M7: portable, sandboxed; without the sandbox an image naming a guest is refused
#endif
};
}  // namespace

const ActorKind* actor_table(size_t* n) {
    EXT_RAM_BSS_ATTR static ActorKind s_table[kMaxActorKinds];  // only tasks touch it: PSRAM when present
    static size_t s_n = 0;
    static bool s_done = false;
    if (!s_done) {
        s_done = true;
        const char* why = nullptr;
        const char* a = nullptr;
        const char* b = nullptr;
        if (!merge_actor_tables(kBuiltinRows, sizeof(kBuiltinRows) / sizeof(kBuiltinRows[0]), kAppRows,
                                kAppRowsLen, s_table, kMaxActorKinds, s_n, &why, &a, &b)) {
            ESP_LOGE(kTag, "actor table REFUSED: %s (%s%s%s) -- running Potluck's built-in actors only; "
                     "an image naming an external type will be refused",
                     why, a != nullptr ? a : "?", b != nullptr ? " and " : "", b != nullptr ? b : "");
        }
        for (size_t i = 0; i < kAppRowsLen; ++i) {
            ESP_LOGI(kTag, "external component: %s", kAppRows[i].component);
        }
    }
    *n = s_n;
    return s_table;
}
}  // namespace app

}  // namespace pot
