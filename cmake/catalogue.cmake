# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/catalogue (docs/plans/phone-in-the-app.md, 2026-11-12): the catalogue a
# phone reads, generic half. Local search builds a generation and links this
# to deliver it; the app's phone feature (src/phone) links it for the keys,
# library.json, the QR and the nearby copy; the phone verifies with it.
#
#   mv_sha256      the SHA-256 the catalogue, the model downloader and the
#                  tests share (catalogue/sha256.*).
#   mv_catalogue   keys, current.json, delivery, retention, the pack readers
#                  (catalog.*), moving the key (catalog_chain.*), and the
#                  publisher key and the library's start (publisher.*).
#
# Portable (D9), libsodium only. Included by cmake/ai.cmake, so every build
# with Local search has it; the app includes it for src/phone too.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, MV_SOURCE_ROOT or the root.

if(TARGET mv_catalogue)
  return()
endif()
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_C "${MV_SOURCE_ROOT}/src/catalogue")

add_library(mv_sha256 STATIC ${_C}/sha256.cpp ${_C}/sha256.h)
target_include_directories(mv_sha256 PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_sha256 PUBLIC mv_project_options)

add_library(mv_catalogue STATIC
  ${_C}/catalog.cpp ${_C}/catalog.h
  ${_C}/catalog_chain.cpp ${_C}/catalog_chain.h
  ${_C}/publisher.cpp ${_C}/publisher.h
)
target_include_directories(mv_catalogue PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_catalogue PUBLIC mv_project_options mv_sha256 PRIVATE ${MV_SODIUM_TARGET})

set_target_properties(mv_sha256 mv_catalogue PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
