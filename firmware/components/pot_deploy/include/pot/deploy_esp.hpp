// The ESP32 implementation of SlotStore. Board-only: host tests use an in-memory store instead.

#pragma once

#include "pot/deploy.hpp"

namespace pot {

class EspSlotStore : public SlotStore {
  public:
    // Finds the modA/modB partitions. False if the partition table lacks them, in which case the
    // node runs its built-in behaviour and refuses deployments.
    bool init();
    bool ready() const { return ok_; }

    bool load_state(DeployState& out) override;
    bool save_state(const DeployState& s) override;
    bool write_slot(uint8_t slot, const uint8_t* data, size_t len) override;
    bool read_slot(uint8_t slot, uint8_t* buf, size_t cap, size_t& len) override;

  private:
    bool ok_ = false;
};

}  // namespace pot
