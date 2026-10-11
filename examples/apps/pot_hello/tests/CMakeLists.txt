# pot_hello's own tests, the way an app in its own repository tests itself with Potluck's test kit
# (tests/testkit, DEVELOPING-APPS.md section 8). From this directory:
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
#
# An app outside Potluck's tree sets POTLUCK_DIR to its Potluck checkout instead of the relative default.

cmake_minimum_required(VERSION 3.20)
project(pot_hello_tests C CXX)

if(DEFINED ENV{POTLUCK_DIR})
  set(POTLUCK_DIR "$ENV{POTLUCK_DIR}" CACHE PATH "a Potluck checkout")
else()
  set(POTLUCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "a Potluck checkout")
endif()
if(NOT TARGET potluck_testkit)  # Potluck's own suite adds the kit first, then this directory
  add_subdirectory("${POTLUCK_DIR}/tests/testkit" potluck_testkit)
endif()
enable_testing()

potluck_app_test(pot_hello_tests
  SOURCES  test_hello.cpp ../src/hello.cpp
  INCLUDES ../include)
