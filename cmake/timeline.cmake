# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/design/29: a Final Cut Pro project as a timeline model, both platforms.
# Included by the Windows root, cmake/darwin.cmake and the headless
# cmake/portable build, on every architecture (it needs no AI pack).
#
#   mv_nle_timeline   src/nle: the lossless XML tree (xml.h), exact FCPXML
#                     time (rational.h) and the timeline model and new-version
#                     writer (timeline.h), and what the FCP panel shares with
#                     the timeline agent (bridge_state.h). Portable C++, no
#                     dependencies (core/json.h is header-only): the FCP panel
#                     and the agent add-on link it.
#   mv-timeline       Both platforms: describe a project, check a round trip,
#                     write a new version (tools/timeline).
#
# Expects: mv_project_options.

if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_tl "${MV_SOURCE_ROOT}/src/nle")

add_library(mv_nle_timeline STATIC
  ${_tl}/xml.cpp
  ${_tl}/xml.h
  ${_tl}/rational.h
  ${_tl}/timeline.cpp
  ${_tl}/timeline.h
  ${_tl}/edit.cpp
  ${_tl}/edit.h
  ${_tl}/bridge_state.cpp
  ${_tl}/bridge_state.h
)
target_include_directories(mv_nle_timeline PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_nle_timeline PUBLIC mv_project_options)
set_target_properties(mv_nle_timeline PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(mv-timeline "${MV_SOURCE_ROOT}/tools/timeline/main.cpp")
target_link_libraries(mv-timeline PRIVATE mv_nle_timeline)
