# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The phone's link and catalogue sync (docs/plans/mobile.md M3, M4): the C ABI
# in src/mobile/mv_mobile_link.h. Static for src.ios to link; on the Mac it is
# what the link tests run, so nothing here is Apple-only but the APIs named.
set(_M "${MV_SOURCE_ROOT}/src/mobile")
add_library(mv_mobile_link STATIC
  ${_M}/link.cpp ${_M}/link.h
  ${_M}/link_payload.cpp ${_M}/link_payload.h
  ${_M}/link_generation.cpp ${_M}/link_generation.h
  ${_M}/link_thumbs.cpp ${_M}/link_thumbs.h
  ${_M}/sync.cpp ${_M}/sync.h
  ${_M}/link_abi.cpp ${_M}/mv_mobile_link.h)
target_include_directories(mv_mobile_link PUBLIC "${MV_SOURCE_ROOT}/src")
target_compile_features(mv_mobile_link PUBLIC cxx_std_20)
target_link_libraries(mv_mobile_link PUBLIC mv_project_options mv_driftwire_engine mv_ai_engine
                      PRIVATE ${MV_SQLITE_TARGET} ${MV_SODIUM_TARGET})
