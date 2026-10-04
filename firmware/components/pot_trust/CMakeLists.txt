# Section 9.3, M5: node identity, node certificates, enrolment. trust.cpp is portable and compiles for
# the host in tests/; trust_store.cpp is the NVS and RNG backend and builds only here.
idf_component_register(
  SRCS "src/trust.cpp" "src/hello_auth.cpp" "src/trust_store.cpp"
  INCLUDE_DIRS "include"
  REQUIRES monocypher nvs_flash esp_hw_support bootloader_support
)
