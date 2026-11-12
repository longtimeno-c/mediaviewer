# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/mobile (docs/plans/mobile.md 8.1): the phone's catalogue reader and search,
# as a static library, for iOS and for the macOS tests.
add_library(mv_mobile_search STATIC
  ${MV_SOURCE_ROOT}/src/mobile/catalogue.cpp
  ${MV_SOURCE_ROOT}/src/mobile/catalogue.h
  ${MV_SOURCE_ROOT}/src/mobile/file_map.cpp
  ${MV_SOURCE_ROOT}/src/mobile/file_map.h
  ${MV_SOURCE_ROOT}/src/mobile/search.cpp
  ${MV_SOURCE_ROOT}/src/mobile/search.h
  ${MV_SOURCE_ROOT}/src/mobile/mv_mobile_search.cpp
  ${MV_SOURCE_ROOT}/src/mobile/mv_mobile_search.h
  ${MV_SOURCE_ROOT}/src/mobile/testing.h
)
target_include_directories(mv_mobile_search PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_mobile_search PUBLIC mv_ai_engine mv_project_options PRIVATE ${MV_SQLITE_TARGET})
set_target_properties(mv_mobile_search PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# A generation the publisher builds, for the tests (macOS) and MobileCore's tests (Simulator).
add_library(mv_mobile_test_fixture STATIC
  ${MV_SOURCE_ROOT}/tests/mobile_generation_fixture.cpp
  ${MV_SOURCE_ROOT}/tests/mobile_generation_fixture.h)
target_include_directories(mv_mobile_test_fixture PUBLIC "${MV_SOURCE_ROOT}/tests")
target_link_libraries(mv_mobile_test_fixture PUBLIC mv_ai_engine mv_project_options PRIVATE ${MV_SQLITE_TARGET})
