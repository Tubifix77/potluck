// The node identity on the board: NVS for persistence, the hardware RNG for the key. ESP-only; the
// rules live in trust.hpp.

#pragma once

#include <cstdint>

#include "pot/trust.hpp"

namespace pot {

// Load the identity from NVS. False when there is none (a board that has never generated a key) or
// it is unreadable; `id` is then left with has_key = false.
bool trust_load(Identity& id);
bool trust_save(const Identity& id);

// 32 bytes from the hardware RNG. esp_random.h: "If Wi-Fi or Bluetooth are enabled, this function
// returns true random numbers." When the radio is not running, bootloader_random_enable() supplies an
// entropy source for the duration of the call, as bootloader_random.h describes for that case.
void trust_random_seed(uint8_t seed[kEdSeedLen], bool radio_running);

}  // namespace pot
