# Built-in actors that need more than a configuration (ADR-003 Tier 0). Portable: the same sources
# compile for the host in tests/. M8: svc_client.
idf_component_register(
  SRCS "src/svc_client.cpp" "src/actor_runtime.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_link pot_ns pot_deploy
)
