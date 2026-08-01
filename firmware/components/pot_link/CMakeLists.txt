# Membership, link statistics and the serial stats format. Depends only on pot_frame, so it too
# compiles for the host in tests/.
idf_component_register(
  SRCS "src/link_stats.cpp" "src/membership.cpp" "src/stats_json.cpp" "src/node.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_frame pot_ns
)
