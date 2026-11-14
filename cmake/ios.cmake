# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The iPhone and iPad core (docs/plans/mobile.md 8.1 and 14): the search and
# Driftwire engines as static libraries for src.ios to link, and nothing of the
# viewer. Included from the root CMakeLists.txt, which then returns.
#
#   cmake -S . -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#     -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#
# (CMAKE_OSX_SYSROOT=iphonesimulator for the Simulator.) The root picks the
# triplet from the SDK and the manifest in tools/ios. The iOS build contains no
# GPL or LGPL dependency: tools/ios/licence_check.py fails the build on one, and
# the app runs the same check through the script generated below.

set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE -Wall -Wextra -Werror -fno-rtti)
target_compile_definitions(mv_project_options INTERFACE MV_DARWIN=1)

find_package(Threads REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
set(MV_SQLITE_TARGET unofficial::sqlite3::sqlite3)
set(MV_SODIUM_TARGET unofficial-sodium::sodium)

include("${CMAKE_CURRENT_LIST_DIR}/ai.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/driftwire.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/mobile-link.cmake")

# The phone's catalogue and search (docs/plans/mobile.md M5 and M6's local half).
include("${CMAKE_CURRENT_LIST_DIR}/mobile.cmake")

# ---- the checks --------------------------------------------------------------
# Built for both SDKs, so a device build proves its link too; run on the
# Simulator (tools/ios/run-simulator-smoke.sh).
add_executable(mv_ios_core_smoke "${CMAKE_SOURCE_DIR}/tools/ios/core_smoke.cpp")
target_link_libraries(mv_ios_core_smoke PRIVATE mv_ai_engine mv_infer mv_driftwire_engine)
target_include_directories(mv_ios_core_smoke SYSTEM PRIVATE "${MV_ORT_INCLUDE}")
# A plain program for simctl spawn; CMake makes every iOS executable a bundle otherwise.
set_target_properties(mv_ios_core_smoke PROPERTIES MACOSX_BUNDLE FALSE)

find_program(MV_PYTHON3 python3 NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# The parity set's Simulator half (docs/plans/mobile.md §9; tools/ios/run-parity.sh).
add_executable(mv_mobile_parity "${CMAKE_SOURCE_DIR}/tools/ios/mobile_parity.cpp")
target_link_libraries(mv_mobile_parity PRIVATE mv_mobile_search mv_ai_engine mv_infer mv_driftwire_engine)
set_target_properties(mv_mobile_parity PROPERTIES MACOSX_BUNDLE FALSE)

# Which ONNX Runtime slice this build links, for the telemetry pin below.
get_target_property(_ort_library mv_ort_static IMPORTED_LOCATION)

# One definition of the licence check, for this build and for the app:
#   <build>/ios-licence-check.sh --binary path/to/MediaViewer.app
# (src.ios's build phase, before the app is stripped; a folder is walked for
# every program in it.)
set(MV_IOS_LICENCE_CHECK "${CMAKE_BINARY_DIR}/ios-licence-check.sh")
file(WRITE "${MV_IOS_LICENCE_CHECK}" "#!/bin/sh
exec \"${MV_PYTHON3}\" \"${CMAKE_SOURCE_DIR}/tools/ios/licence_check.py\" \\
  --installed \"${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}\" \\
  --licence \"onnxruntime=${MV_ORT_DIR}/LICENSE\" \"$@\"
")
file(CHMOD "${MV_IOS_LICENCE_CHECK}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

add_custom_target(mv_ios_licence_check ALL
  COMMAND "${MV_IOS_LICENCE_CHECK}" --binary "$<TARGET_FILE:mv_ios_core_smoke>"
  COMMAND "${MV_PYTHON3}" "${CMAKE_SOURCE_DIR}/tools/ios/ort_telemetry_pin.py"
          --library "${_ort_library}" --version "${MV_ORT_VERSION}"
  DEPENDS mv_ios_core_smoke
  COMMENT "No GPL or LGPL dependency in the iOS build; ONNX Runtime's telemetry as reviewed"
  VERBATIM)
