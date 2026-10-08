// M8.1: this firmware's registration table -- every actor type it can run, one row each.
//
// Adding an actor type means: a class against pot/actor.hpp, its config format in pot/deploy.hpp
// (so images can name it), and one row in actor_table.cpp. Nothing in m0_main.cpp.

#pragma once

#include <cstddef>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"

namespace pot {

namespace app {
extern const ActorKind kActorTable[];
extern const size_t kActorTableLen;
}  // namespace app

// The few board services an actor may need that are not the node's. m0_main.cpp provides them.
namespace board {
// What "healthy" looks like on the status LED (M3). Blue, yellow and red keep their diagnostic
// meanings whatever is deployed.
void set_led_healthy(const LedConfig& c);
}  // namespace board

}  // namespace pot
