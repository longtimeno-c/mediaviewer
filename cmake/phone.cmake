# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mv_phone (src/phone): Phone access as part of the app (docs/plans/
# phone-in-the-app.md, owner 2026-11-12) — the desk that claims and manages
# `driftwire serve` and serves nearby (desk.*), the state machine behind
# Settings → Phone access (link.*), and the app's one link (app.*). Works with
# no add-on installed; Local search is a catalogue source when loaded.
#
# Portable (D9): over mv_wire (the protocol, its OS ports) and mv_catalogue.
# Included by the app builds (the Windows root, cmake/darwin.cmake,
# cmake/portable) after cmake/driftwire.cmake.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, Threads, mv_io.

if(TARGET mv_phone)
  return()
endif()
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/wire.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/catalogue.cmake")
set(_P "${MV_SOURCE_ROOT}/src/phone")

add_library(mv_phone STATIC
  ${_P}/desk.cpp ${_P}/desk.h
  ${_P}/link.cpp ${_P}/link.h
  ${_P}/app.cpp ${_P}/app.h
)
target_include_directories(mv_phone PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_phone PUBLIC mv_project_options mv_wire mv_catalogue Threads::Threads
  PRIVATE ${MV_SODIUM_TARGET} mv_io)
set_target_properties(mv_phone PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
