# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/mobile (docs/plans/mobile.md 8.1): the phone's catalogue reader and search,
# as a static library, for iOS and for the macOS tests.
add_library(mv_mobile_search STATIC
  ${MV_SOURCE_ROOT}/src/mobile/catalogue.cpp
  ${MV_SOURCE_ROOT}/src/mobile/catalogue.h
  ${MV_SOURCE_ROOT}/src/mobile/search.cpp
  ${MV_SOURCE_ROOT}/src/mobile/search.h
  ${MV_SOURCE_ROOT}/src/mobile/mv_mobile_search.cpp
  ${MV_SOURCE_ROOT}/src/mobile/mv_mobile_search.h
)
target_include_directories(mv_mobile_search PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_mobile_search PUBLIC mv_ai_engine mv_project_options PRIVATE ${MV_SQLITE_TARGET})
set_target_properties(mv_mobile_search PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
