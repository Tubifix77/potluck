# Potluck's test kit, for an application's own repository (M12).
#
# Test your actors on a laptop, against Potluck's real portable core (the same Node, namespace, image parser
# and actor API the boards run, compiled for the host), with no board and nothing to install but CMake and a
# C++17 compiler. In your app's tests/CMakeLists.txt:
#
#   cmake_minimum_required(VERSION 3.20)
#   project(my_app_tests CXX)
#   set(POTLUCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../potluck" CACHE PATH "a Potluck checkout")
#   add_subdirectory("${POTLUCK_DIR}/tests/testkit" potluck_testkit)
#   enable_testing()
#   potluck_app_test(my_app_tests
#     SOURCES  test_my_actor.cpp ../firmware/components/pot_my/src/my.cpp
#     INCLUDES ../firmware/components/pot_my/include)
#
# then `cmake -S tests -B build && cmake --build build && ctest --test-dir build`. A test file includes
# "pot/testkit.hpp" (a rig: one node, a clock you move, the LED captured) and "test_harness.hpp" (TEST, CHECK,
# CHECK_EQ, CHECK_STR_EQ, CHECK_BYTES_EQ); the kit supplies main(), so a test file is only its tests. Run a
# subset by name: `my_app_tests <substring>`. DEVELOPING-APPS.md section 8 walks through it, and
# examples/apps/pot_hello/tests is a complete example.

cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_PROJECT_NAME)
  project(potluck_testkit C CXX)
endif()
enable_language(C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(POT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(POT_FIRMWARE "${POT_ROOT}/firmware")
include("${POT_ROOT}/tests/potluck_core.cmake")
if(MSVC)
  target_compile_definitions(pot_portable PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(pot_portable PRIVATE /utf-8)
endif()

add_library(potluck_testkit STATIC "${CMAKE_CURRENT_LIST_DIR}/testkit_main.cpp")
target_include_directories(potluck_testkit PUBLIC "${POT_ROOT}/tests" "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(potluck_testkit PUBLIC pot_portable)
if(MSVC)
  target_compile_options(potluck_testkit PUBLIC /utf-8)
endif()

# potluck_app_test(<name> SOURCES <files...> [INCLUDES <dirs...>]): one test executable, registered with CTest.
function(potluck_app_test name)
  cmake_parse_arguments(T "" "" "SOURCES;INCLUDES" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE potluck_testkit)
  if(T_INCLUDES)
    target_include_directories(${name} PRIVATE ${T_INCLUDES})
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()
