# The namespace: typed values (§4 rule 2), the resource table (§7.2) and the READ/WRITE/REPLY wire
# payloads. Portable — depends only on pot_frame, so it compiles for the host in tests/ and sim/.
idf_component_register(
  SRCS "src/value.cpp" "src/namespace.cpp" "src/ns_payloads.cpp" "src/sys_resources.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_frame
)
