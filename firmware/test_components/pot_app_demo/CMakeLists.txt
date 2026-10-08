# M8.2 (PS-1): a stand-in external component, kept in Potluck's repository only to test the seam
# that real applications use from their own (tools\build_firmware.ps1 -AppComponents
# firmware\test_components). Its types are in 0xF0-0xFF, the block Potluck keeps for its own tests.
idf_component_register(
  SRCS "demo.cpp"
  INCLUDE_DIRS "."
  REQUIRES pot_actors pot_link pot_ns pot_deploy
)
