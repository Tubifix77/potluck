idf_component_register(
  SRCS "m0_main.cpp"
  INCLUDE_DIRS "."
  # esp_driver_gpio, not the old monolithic `driver`: ESP-IDF v6.0 split the drivers into
  # per-peripheral components and `driver` no longer re-exports driver/gpio.h.
  REQUIRES pot_frame pot_link pot_espnow esp_timer esp_driver_gpio esp_app_format
)
