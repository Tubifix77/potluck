# Host build for the portable half of Potluck.
#
# The pot_frame and pot_link components have no ESP-IDF dependency, on purpose: the codec, the
# heartbeat state machine and the statistics are where the bugs that matter live, and they should
# be testable in a second on a laptop rather than in a minute on a board. This CMakeLists compiles
# those same sources for the host — not a copy of them — and links them against a header-only test
# harness with no external dependencies.
#
#   cmake -S tests -B build/tests -G Ninja
#   cmake --build build/tests
#   ctest --test-dir build/tests --output-on-failure
#
# Or just run tools/run_host_tests.ps1, which does the above and finds a compiler first.

cmake_minimum_required(VERSION 3.20)
project(pot_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(POT_FIRMWARE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware")

option(POT_ASAN "Build the tests with AddressSanitizer" OFF)
option(POT_WERROR "Treat warnings as errors" ON)

add_library(pot_portable STATIC
  "${POT_FIRMWARE}/components/pot_frame/src/frame.cpp"
  "${POT_FIRMWARE}/components/pot_frame/src/payloads.cpp"
  "${POT_FIRMWARE}/components/pot_frame/src/serial_framing.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/link_stats.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/membership.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/stats_json.cpp"
  "${POT_FIRMWARE}/components/pot_link/src/node.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/value.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/namespace.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/ns_payloads.cpp"
  "${POT_FIRMWARE}/components/pot_ns/src/sys_resources.cpp"
)
target_include_directories(pot_portable PUBLIC
  "${POT_FIRMWARE}/components/pot_frame/include"
  "${POT_FIRMWARE}/components/pot_link/include"
  "${POT_FIRMWARE}/components/pot_ns/include"
)

add_executable(pot_tests
  test_main.cpp
  test_frame_golden.cpp
  test_frame_codec.cpp
  test_frame_reject.cpp
  test_frame_fuzz.cpp
  test_heartbeat.cpp
  test_link_stats.cpp
  test_stats_json.cpp
  test_node.cpp
  test_namespace.cpp
  test_remote_read.cpp
  test_sys_resources.cpp
  test_serial_framing.cpp
  test_ns_wire.cpp
  test_work_units.cpp
)
target_link_libraries(pot_tests PRIVATE pot_portable)
target_include_directories(pot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Warnings are set high deliberately. The portable code is destined for a target where a
# sign-conversion mistake in length arithmetic is a remote memory-safety bug, so the host build is
# the cheapest place to catch one.
foreach(tgt pot_portable pot_tests)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /permissive- /utf-8)
    if(POT_WERROR)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${tgt} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual)
    if(POT_WERROR)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
  endif()

  if(POT_ASAN)
    if(MSVC)
      target_compile_options(${tgt} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endforeach()

# The cell simulator. Links the same pot_portable the tests do, so it runs the firmware's actual
# node policy rather than a reimplementation of it -- which is the only reason its results mean
# anything. See sim/sim.cpp.
add_executable(pot_sim "${CMAKE_CURRENT_SOURCE_DIR}/../sim/sim.cpp")
target_link_libraries(pot_sim PRIVATE pot_portable)
target_include_directories(pot_sim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sim")
if(MSVC)
  target_compile_options(pot_sim PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(pot_sim PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(pot_sim PRIVATE -Wall -Wextra)
endif()

# The section 7.8 work-unit driver: a coordinator and N workers on the same modelled cell, sharing
# sim/cell.hpp with pot_sim. See sim/work.cpp.
add_executable(pot_work "${CMAKE_CURRENT_SOURCE_DIR}/../sim/work.cpp")
target_link_libraries(pot_work PRIVATE pot_portable)
target_include_directories(pot_work PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sim")
if(MSVC)
  target_compile_options(pot_work PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(pot_work PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(pot_work PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME pot_tests COMMAND pot_tests)

# The suites are also registered individually so a failure names the area rather than just
# "pot_tests". The filter is a substring match on suite or case name.
foreach(suite golden codec reject fuzz heartbeat peer_table stats budget json node ns m1 sysres serial work)
  add_test(NAME pot_${suite} COMMAND pot_tests ${suite})
endforeach()
