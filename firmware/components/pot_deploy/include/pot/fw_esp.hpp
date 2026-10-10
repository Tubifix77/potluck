// M11: the ESP32 implementation of FwSink -- ESP-IDF's OTA API on the app slot not booted from.
// Board-only: the host tests use an in-memory sink.

#pragma once

#include "esp_ota_ops.h"
#include "pot/fw.hpp"

namespace pot {

class EspOtaSink : public FwSink {
  public:
    // Finds the spare slot. False if the partition table has no OTA slots, in which case the node
    // refuses firmware updates (capacity 0).
    bool init();
    uint32_t capacity() const override;
    bool begin(uint32_t image_len) override;
    bool write(const uint8_t* data, size_t len) override;
    bool finish() override;    // esp_ota_end: the image is checked (format, chip, its own SHA-256)
    bool activate() override;  // esp_ota_set_boot_partition
    void abort() override;
    const esp_partition_t* target() const { return part_; }

  private:
    const esp_partition_t* part_ = nullptr;
    esp_ota_handle_t h_ = 0;
    bool open_ = false;
    bool ended_ = false;
};

}  // namespace pot
