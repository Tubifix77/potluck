# M7: the WebAssembly sandbox (include/pot/wasm_sandbox.hpp), wasm3 v0.9.0 with two patches
# (PROVENANCE.md), and the guest actors that run in it (include/pot/guest.hpp). Dormant unless
# CONFIG_POT_WASM: without it this is a header-only component and no byte of the interpreter reaches the
# image.
if(CONFIG_POT_WASM)
  file(GLOB pot_wasm3_srcs "${CMAKE_CURRENT_LIST_DIR}/wasm3/*.c")
  idf_component_register(
    SRCS "src/wasm_sandbox.cpp" "src/guest.cpp" ${pot_wasm3_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "wasm3"
    REQUIRES pot_actors pot_deploy pot_trust pot_link pot_ns
  )
  # ESP32 selects wasm3's embedded defaults (small stack heights, no cascaded opcodes). M3_IN_IRAM only
  # tells wasm3 not to mark each opcode IRAM_ATTR; no linker fragment moves it there, so the
  # interpreter runs from flash. d_m3MaxNativeStack > 0 compiles in the native-stack check, whose limit
  # the sandbox sets per call. M3_HAS_TAIL_CALL=0: Xtensa GCC cannot emit a sibling call ("machine
  # description does not have a sibcall_epilogue"), so wasm3's musttail dispatch does not compile; without
  # it each dispatched op nests a C frame, and the guest's stack use is measured, not assumed.
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32 M3_IN_IRAM M3_HAS_TAIL_CALL=0
                             d_m3ExternalYield=1 d_m3YieldInLoops=1
                             d_m3MaxNativeStack=16384 d_m3HasWASI=0 d_m3HasUVWASI=0 d_m3HasMetaWASI=0)
  set_source_files_properties(${pot_wasm3_srcs} PROPERTIES COMPILE_OPTIONS "-w;-O2")
else()
  idf_component_register(INCLUDE_DIRS "include" REQUIRES pot_actors pot_deploy pot_trust pot_link pot_ns)
endif()
