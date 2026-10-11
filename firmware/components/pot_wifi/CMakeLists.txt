# M13: Potluck's own Wi-Fi station (include/pot/wifi.hpp), an optional board module. Without CONFIG_POT_WIFI the
# same source compiles to stubs, so callers need no #if and no byte of the station reaches the image.
idf_component_register(
  SRCS "src/wifi.cpp"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash log heap
)
