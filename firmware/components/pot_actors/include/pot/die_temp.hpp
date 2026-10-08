// M8.1: the first actor with real hardware behind it -- the chip's own temperature sensor, read on
// its node and published to the namespace, where any node reads it with its age and quality.
//
// The actor knows nothing of ESP-IDF: the sensor is a DieTempDriver, two calls supplied by the
// firmware (main/actor_table.cpp, over ESP-IDF's temperature_sensor driver) or by a test. That keeps
// the behaviour -- what is published, when a read failing becomes FAULTY -- testable on the host.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

struct DieTempDriver {
    void* ctx = nullptr;
    bool (*open)(void* ctx) = nullptr;               // install and enable; false = failed (retried)
    bool (*read)(void* ctx, float* celsius) = nullptr;  // false = the driver reported an error
};

class DieTempActor : public Actor {
  public:
    DieTempActor(Node& node, const DieTempConfig& cfg, const DieTempDriver& drv)
        : node_(node), cfg_(cfg), drv_(drv) {}

    // Declares the output: f32, Celsius, sampled, read-only, L4 (it changes over seconds), stale
    // after three periods. Then opens the driver; a driver that will not open is not a reason to
    // refuse the deployment -- the resource reads FAULTY and the open is retried every period.
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    // Test instrument: "POT! die_temp fail 1" makes every read count as a driver error until
    // "POT! die_temp fail 0", so FAULTY can be shown on the bench without breaking the chip.
    bool on_console(const char* line, size_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    struct Stats {
        uint32_t reads;    // published
        uint32_t errors;   // reads (or opens) that failed: published as FAULTY
        float last_c;
    };
    const Stats& stats() const { return stats_; }

  private:
    Node& node_;
    DieTempConfig cfg_;
    DieTempDriver drv_;
    bool open_ = false;
    bool forced_fail_ = false;
    uint32_t next_ms_ = 0;
    Stats stats_{};
};

// The row's check, shared by every registration table that offers die_temp. Its create() lives with
// the driver (it must supply one).
bool die_temp_check(const ActorDecl& d, const ActorEnv& env, const char** why);
// The resource a die_temp actor publishes, on whichever node it is pinned to (the row's output hook).
NsDecl die_temp_decl(uint16_t owner, const DieTempConfig& c);
bool die_temp_output(const ActorDecl& d, NsDecl& out);

}  // namespace pot
