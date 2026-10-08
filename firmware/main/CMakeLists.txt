# M6.1: the extender variant pulls in poor-mans-extender's pme_hotspot. The directory arrives through
# the environment (tools\build_firmware.ps1 -PmeHotspot), which -- unlike a Kconfig value -- is
# visible while ESP-IDF expands component requirements.
set(pot_extra_requires "")
if(DEFINED ENV{POT_PME_HOTSPOT_DIR})
  list(APPEND pot_extra_requires pme_hotspot)
endif()

idf_component_register(
  SRCS "m0_main.cpp" "selftest.cpp" "crypto_bench.cpp" "extender.cpp"
  INCLUDE_DIRS "."
  # esp_driver_gpio, not the old monolithic `driver`: ESP-IDF v6.0 split the drivers into
  # per-peripheral components and `driver` no longer re-exports driver/gpio.h.
  REQUIRES pot_frame pot_link pot_espnow pot_deploy pot_can esp_timer esp_driver_gpio esp_driver_rmt esp_app_format monocypher mbedtls pot_trust pot_reconcile pot_actors esp_driver_uart ${pot_extra_requires}
)
