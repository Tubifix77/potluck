// Boot epoch — a monotonic per-boot counter held in NVS.
//
// Every M0 payload carries it, for one reason: without it, a peer that reboots inside its own
// death window is indistinguishable from a peer whose link merely recovered. Those need different
// handling even at M0 (the rebooted peer's sequence numbers restarted, so the old expectations
// would report tens of thousands of phantom losses), and §7.7's fencing and §8.3's replay
// rejection both build on the same field later. It is cheap now and load-bearing later.
//
// NVS is the right store: §3 records "NVS works best for storing many small values", and a
// 4-byte counter written once per boot is exactly that. One write per boot is also the wear
// budget — at a boot a minute for ten years that is ~5.3 M writes spread over the NVS partition's
// wear levelling, which is well inside flash endurance.

#pragma once

#include <cstdint>

namespace pot {

// Read the stored epoch, increment it, persist it, and return the new value. Call once, after
// nvs_flash_init() and before announcing anything.
//
// On any NVS failure this returns a value derived from the hardware RNG instead of 0. A colliding
// random epoch is unlikely; a zero epoch would be silently interpreted as "no information" by
// every consumer of the field, which is worse than a wrong value because it is invisible.
uint32_t boot_epoch_next();

// The value returned by the last boot_epoch_next(), or 0 before it has been called.
uint32_t boot_epoch_current();

}  // namespace pot
