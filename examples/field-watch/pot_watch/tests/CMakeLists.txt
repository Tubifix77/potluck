# The field watch's own tests with Potluck's test kit (tests/testkit, DEVELOPING-APPS.md section 8). From here:
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(pot_watch_tests C CXX)

if(DEFINED ENV{POTLUCK_DIR})
  set(POTLUCK_DIR "$ENV{POTLUCK_DIR}" CACHE PATH "a Potluck checkout")
else()
  set(POTLUCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "a Potluck checkout")
endif()
if(NOT TARGET potluck_testkit)  # Potluck's own suite adds the kit first, then this directory
  add_subdirectory("${POTLUCK_DIR}/tests/testkit" potluck_testkit)
endif()
enable_testing()

potluck_app_test(pot_watch_tests
  SOURCES  test_watch.cpp ../src/watch.cpp
  INCLUDES ../include)
