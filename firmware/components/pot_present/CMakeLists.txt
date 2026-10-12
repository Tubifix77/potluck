# M13: the presenting role. present_core.cpp is portable (also built by tests/potluck_core.cmake); present.cpp is
# the board half over the managed MQTT client, built only with CONFIG_POT_PRESENT -- without it no byte of the
# client reaches the image and the component is its header and its pure core.
#
# The requirements are the same in both branches on purpose: ESP-IDF expands requirements before Kconfig values
# exist, so a branch on CONFIG_* cannot change them. espressif/mqtt is required through idf_component.yml, whose
# rule fetches it only when CONFIG_POT_PRESENT is set.
set(pot_present_srcs "src/present_core.cpp")
if(CONFIG_POT_PRESENT)
  list(APPEND pot_present_srcs "src/present.cpp")
endif()
idf_component_register(
  SRCS ${pot_present_srcs}
  INCLUDE_DIRS "include"
  REQUIRES pot_actors pot_deploy pot_link pot_ns
  PRIV_REQUIRES esp_netif esp_timer log freertos nvs_flash heap
)
