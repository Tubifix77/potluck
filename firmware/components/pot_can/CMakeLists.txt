# The CAN (TWAI) transport, section 5.3.1 / M4. can_profile.cpp is portable and compiles for the host
# in tests/; can_port.cpp is the TWAI driver glue and builds only here.
idf_component_register(
  SRCS "src/can_profile.cpp" "src/can_port.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_frame esp_driver_twai esp_timer
)
