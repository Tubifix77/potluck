idf_component_register(
  SRCS "m0_main.cpp" "selftest.cpp" "crypto_bench.cpp"
  INCLUDE_DIRS "."
  # esp_driver_gpio, not the old monolithic `driver`: ESP-IDF v6.0 split the drivers into
  # per-peripheral components and `driver` no longer re-exports driver/gpio.h.
  REQUIRES pot_frame pot_link pot_espnow pot_deploy pot_can esp_timer esp_driver_gpio esp_driver_rmt esp_app_format monocypher mbedtls pot_trust esp_driver_uart
)
