# Deploy and detach (section 7.4, M3). deploy.cpp is portable and compiles for the host in tests/;
# deploy_esp.cpp is the flash/NVS backend and builds only here. M11: fw.cpp is portable too, and
# fw_esp.cpp is its OTA backend.
idf_component_register(
  SRCS "src/deploy.cpp" "src/deploy_esp.cpp" "src/fw.cpp" "src/fw_esp.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_partition nvs_flash spi_flash monocypher app_update
)
