# Potluck M0 firmware — ESP-IDF project.
#
#   idf.py set-target esp32
#   idf.py build
#   idf.py -p COMx flash monitor
#
# Target is ESP32 classic, per ADR-001's scope discipline and §6's budget, which is written against
# the classic part's 320 KB DRAM / 160 KB static ceiling.

cmake_minimum_required(VERSION 3.16)

# M6.1: the extender variant takes poor-mans-extender's components directory from the environment
# (tools\build_firmware.ps1 -PmeHotspot). Read from there, never copied: that repository stays its own.
if(DEFINED ENV{POT_PME_HOTSPOT_DIR})
  set(EXTRA_COMPONENT_DIRS "$ENV{POT_PME_HOTSPOT_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(potluck_m0)
