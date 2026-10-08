# The reconciler (section 7.7, M6). Portable: the same source compiles for the host in tests/.
idf_component_register(
  SRCS "src/reconcile.cpp"
  INCLUDE_DIRS "include"
  REQUIRES pot_link pot_ns pot_deploy pot_actors
)
