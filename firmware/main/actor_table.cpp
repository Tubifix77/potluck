// M8.1: the registration table. See actor_table.hpp.

#include "actor_table.hpp"

#include <cstdlib>
#include <new>

#include "esp_log.h"
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

}  // namespace

namespace app {
const ActorKind kActorTable[] = {
    {ActorType::Led, "led", &led_check, &led_create},
    {ActorType::Fault, "fault", &fault_check, &fault_create},
    kSvcClientKind,
    kTickerKind,  // portable: the reconciler places it
};
const size_t kActorTableLen = sizeof(kActorTable) / sizeof(kActorTable[0]);
}  // namespace app

}  // namespace pot
