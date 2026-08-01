# The only component that knows what a radio or a UART is.
idf_component_register(
  SRCS "src/espnow_port.cpp" "src/boot_epoch.cpp" "src/serial_port.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_frame pot_link esp_wifi esp_netif esp_event nvs_flash esp_timer esp_hw_support
           esp_driver_uart
)
