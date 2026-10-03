# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# plan/23: Local search from inside an editing app. Included by cmake/ai.cmake,
# so the Windows root, cmake/darwin.cmake and the headless cmake/portable build
# all build the portable half.
#
#   mv_nle           src/nle: the search agent's wire format, one search
#                    over mv.ai.1 (search_session), the viewer's thumbnails
#                    read-only, and FCPXML. Portable C++: the Mac agent uses it
#                    today, a Windows NLE bridge (Premiere / Resolve) the same.
#   mv-nle-export    Both platforms: runs one Local search through the reader
#                    and writes the results as FCPXML ("Export results as
#                    FCPXML": Final Cut Pro, DaVinci Resolve, Premiere Pro).
#
# The Final Cut Pro pieces (the XPC search agent and the workflow extension)
# are Mac-only (D9 exception, plan/12 2026-09-28) and live in
# cmake/darwin-fcp.cmake: they ship inside MediaViewer.app, dormant until
# Final Cut Pro is turned on in Settings > Local search. mv_nle itself is
# linked by the agent and the tools, never by the app.
#
# Expects: mv_addon (cmake/import.cmake), MV_SQLITE_TARGET, mv_project_options.

if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_nle "${MV_SOURCE_ROOT}/src/nle")

add_library(mv_nle STATIC
  ${_nle}/search_wire.cpp
  ${_nle}/search_wire.h
  ${_nle}/fcpxml.cpp
  ${_nle}/fcpxml.h
  ${_nle}/thumb_reader.cpp
  ${_nle}/thumb_reader.h
  ${_nle}/search_session.cpp
  ${_nle}/search_session.h
)
target_include_directories(mv_nle PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_nle PUBLIC mv_addon mv_project_options PRIVATE ${MV_SQLITE_TARGET})
set_target_properties(mv_nle PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(mv-nle-export "${_nle}/export_main.cpp")
target_link_libraries(mv-nle-export PRIVATE mv_nle mv_addon mv_io)
if(APPLE)
  # The reader loads the AI pack, which ships arm64 only.
  set_target_properties(mv_nle mv-nle-export PROPERTIES OSX_ARCHITECTURES "arm64")
endif()
