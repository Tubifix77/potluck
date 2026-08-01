# The Potluck Frame codec. No REQUIRES: this component depends on nothing but the C++ standard library,
# which is what lets tests/CMakeLists.txt compile these same sources for the host.
idf_component_register(
  SRCS "src/frame.cpp" "src/payloads.cpp" "src/serial_framing.cpp"
  INCLUDE_DIRS "include"
)
