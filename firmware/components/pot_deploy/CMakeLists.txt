# Deploy and detach (section 7.4, M3). deploy.cpp is portable and compiles for the host in tests/;
# deploy_esp.cpp is the flash/NVS backend and builds only here.
idf_component_register(
  SRCS "src/deploy.cpp" "src/deploy_esp.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_partition nvs_flash spi_flash
)
