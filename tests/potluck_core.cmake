# Potluck's portable core for the host: the firmware's own sources (not copies), as the library pot_portable,
# with Monocypher and wasm3 beside it. Included by tests/CMakeLists.txt and by tests/testkit (an
# application's own repository). Needs POT_FIRMWARE: the path to Potluck's firmware directory.

if(TARGET pot_portable)
  return()
endif()
if(NOT POT_FIRMWARE)
  message(FATAL_ERROR "potluck_core.cmake: set POT_FIRMWARE to Potluck's firmware directory first")
endif()

# Monocypher (vendored, unmodified; firmware/components/monocypher/PROVENANCE.md). Third-party C, so
# it gets its own library without this project's strict warning set: its code is not ours to edit.
add_library(monocypher STATIC
  "${POT_FIRMWARE}/components/monocypher/monocypher.c"
  "${POT_FIRMWARE}/components/monocypher/monocypher-ed25519.c"
)
target_include_directories(monocypher PUBLIC "${POT_FIRMWARE}/components/monocypher")

# M7's experiment: wasm3 (vendored, two patches; firmware/components/pot_wasm/PROVENANCE.md), third-party
# C like Monocypher above, so its own library and not this project's warning set. The definitions are
# PUBLIC: the sandbox reads the interpreter's structures, which must be built the same way on both sides.
file(GLOB POT_WASM3_SRCS "${POT_FIRMWARE}/components/pot_wasm/wasm3/*.c")
add_library(wasm3 STATIC ${POT_WASM3_SRCS})
target_include_directories(wasm3 SYSTEM PUBLIC "${POT_FIRMWARE}/components/pot_wasm/wasm3")
target_compile_definitions(wasm3 PUBLIC d_m3ExternalYield=1 d_m3YieldInLoops=1 d_m3MaxNativeStack=65536
                                        d_m3HasWASI=0 d_m3HasUVWASI=0 d_m3HasMetaWASI=0)
if(MSVC)
  target_compile_options(wasm3 PRIVATE /w)
else()
  target_compile_options(wasm3 PRIVATE -w)
endif()

add_library(pot_portable STATIC
  "${POT_FIRMWARE}/components/pot_frame/src/frame.cpp"
  "${POT_FIRMWARE}/components/pot_frame/src/payloads.cpp"
  "${POT_FIRMWARE}/components/pot_frame/src/serial_framing.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/link_stats.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/membership.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/stats_json.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/node.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/telemetry.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/value.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/namespace.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/ns_payloads.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/sys_resources.cpp"
  "${POT_FIRMWARE}/components/pot_deploy/src/deploy.cpp"
  "${POT_FIRMWARE}/components/pot_deploy/src/fw.cpp"
  "${POT_FIRMWARE}/components/pot_deploy/src/node_status.cpp"
  "${POT_FIRMWARE}/components/pot_deploy/src/lease.cpp"
  "${POT_FIRMWARE}/components/pot_deploy/src/package_agent.cpp"
  "${POT_FIRMWARE}/components/pot_can/src/can_profile.cpp"
  "${POT_FIRMWARE}/components/pot_trust/src/trust.cpp"
  "${POT_FIRMWARE}/components/pot_trust/src/hello_auth.cpp"
  "${POT_FIRMWARE}/components/pot_trust/src/frame_auth.cpp"
  "${POT_FIRMWARE}/components/pot_reconcile/src/reconcile.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/svc_client.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/actor_runtime.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/ticker.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/die_temp.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/mc.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/cluster.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/led_switch.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/position.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/foreman.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/inbox.cpp"
  "${POT_FIRMWARE}/components/pot_actors/src/speak.cpp"
  "${POT_FIRMWARE}/components/pot_wasm/src/wasm_sandbox.cpp"
  "${POT_FIRMWARE}/components/pot_wasm/src/guest.cpp"
  "${POT_FIRMWARE}/components/pot_present/src/present_core.cpp"
  "${POT_FIRMWARE}/components/pot_pocket/src/pocket_core.cpp"
)
target_link_libraries(pot_portable PUBLIC monocypher wasm3)
target_include_directories(pot_portable PUBLIC
  "${POT_FIRMWARE}/components/pot_frame/include"
  "${POT_FIRMWARE}/components/pot_link/include"
  "${POT_FIRMWARE}/components/pot_ns/include"
  "${POT_FIRMWARE}/components/pot_deploy/include"
  "${POT_FIRMWARE}/components/pot_can/include"
  "${POT_FIRMWARE}/components/pot_trust/include"
  "${POT_FIRMWARE}/components/pot_reconcile/include"
  "${POT_FIRMWARE}/components/pot_actors/include"
  "${POT_FIRMWARE}/components/pot_wasm/include"
  "${POT_FIRMWARE}/components/pot_present/include"
  "${POT_FIRMWARE}/components/pot_pocket/include"
)
