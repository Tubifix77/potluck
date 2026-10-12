# M13.1: the pocket page. pocket_core.cpp is portable (also built by tests/potluck_core.cmake); pocket.cpp is the board
# half (an access point and ESP-IDF's HTTP server), compiled to stubs without CONFIG_POT_POCKET. The requirements are
# the same either way: ESP-IDF expands them before Kconfig values exist.
idf_component_register(
  SRCS "src/pocket_core.cpp" "src/pocket.cpp"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES esp_wifi esp_netif esp_http_server nvs_flash log heap freertos
)
