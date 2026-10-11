# The worked example of DEVELOPING-APPS.md: an external actor component, as an application keeps one in
# its own repository. Potluck's build picks it up because potluck-modules.json sits beside this file:
#
#   tools\build_firmware.ps1 -Variant hello -AppComponents <repo>\examples\apps
#
# and calls app_rows_pot_hello() (src/app_rows.cpp) for its registration rows. Types 0xF0-0xFF: the block
# Potluck keeps for its own examples and tests.
idf_component_register(
    SRCS "src/hello.cpp" "src/app_rows.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pot_actors pot_link pot_ns pot_deploy
)
