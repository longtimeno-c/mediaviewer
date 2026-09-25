# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The headless portable core build: the parts of Milestone G that have no
# window, no GPU and no codec (io's file / volume / verified-copy ports in
# their POSIX form, the add-on host, and the Import engine), built and tested
# on Linux. It exists so CI proves the shared engine on every pull request
# without a Windows or Mac runner, and so a contributor on any POSIX machine
# can run the Import tests. It is not a product build: MediaViewer ships on
# Windows and macOS only (D9).
#
#   cmake -S cmake/portable -B build-portable
#   cmake --build build-portable && ctest --test-dir build-portable
#
# Dependencies: SQLite, libsodium, BLAKE3 and Catch2 v3, from vcpkg
# (tools/portable/vcpkg.json, the root manifest's baseline) or the system.
# Without an installed BLAKE3 / Catch2 package, point MV_BLAKE3_SOURCE_DIR at
# BLAKE3's c/ directory and MV_CATCH2_AMALGAMATED_DIR at Catch2's
# extras/ directory.

cmake_minimum_required(VERSION 3.28)
project(mediaviewer_portable LANGUAGES C CXX)

get_filename_component(MV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(MV_ASAN "Build with AddressSanitizer + UBSan" OFF)

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-rtti>)
if(MV_ASAN)
  target_compile_options(mv_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(mv_project_options INTERFACE -fsanitize=address,undefined)
endif()

# ---- dependencies -----------------------------------------------------------
find_package(unofficial-sqlite3 CONFIG QUIET)
if(TARGET unofficial::sqlite3::sqlite3)
  set(MV_SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  set(MV_SQLITE_TARGET SQLite::SQLite3)
endif()

find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  set(MV_SODIUM_TARGET unofficial-sodium::sodium)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(MV_SODIUM_TARGET PkgConfig::SODIUM)
endif()

find_package(blake3 CONFIG QUIET)
if(TARGET BLAKE3::blake3)
  set(MV_BLAKE3_TARGET BLAKE3::blake3)
elseif(MV_BLAKE3_SOURCE_DIR)
  # The portable C implementation only: the SIMD back ends are an
  # optimisation the product builds get from vcpkg's port.
  add_library(mv_blake3_portable STATIC
    "${MV_BLAKE3_SOURCE_DIR}/blake3.c"
    "${MV_BLAKE3_SOURCE_DIR}/blake3_dispatch.c"
    "${MV_BLAKE3_SOURCE_DIR}/blake3_portable.c")
  target_include_directories(mv_blake3_portable PUBLIC "${MV_BLAKE3_SOURCE_DIR}")
  target_compile_definitions(mv_blake3_portable PUBLIC
    BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)
  set_target_properties(mv_blake3_portable PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(MV_BLAKE3_TARGET mv_blake3_portable)
else()
  message(FATAL_ERROR "BLAKE3 not found: install it (vcpkg `blake3`) or set MV_BLAKE3_SOURCE_DIR")
endif()

# ---- io: the POSIX halves of the new ports -------------------------------------
add_library(mv_io STATIC
  "${MV_ROOT}/src/io/content_hash.cpp"
  "${MV_ROOT}/src/io/file_port.cpp"
  "${MV_ROOT}/src/io/file_port_mac.cpp"
  "${MV_ROOT}/src/io/verified_copy.cpp"
  "${MV_ROOT}/src/io/volume_mac.cpp"
  "${MV_ROOT}/src/io/paths_mac.cpp"
  "${MV_ROOT}/src/io/replace_mac.cpp"
  "${MV_ROOT}/src/io/child_process_posix.cpp"
)
target_include_directories(mv_io PUBLIC "${MV_ROOT}/src")
target_link_libraries(mv_io PUBLIC mv_project_options PRIVATE ${MV_BLAKE3_TARGET})
set_target_properties(mv_io PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- the add-on host and the Import add-on --------------------------------------
set(MV_SOURCE_ROOT "${MV_ROOT}")
include("${MV_ROOT}/cmake/import.cmake")

# ---- tests ----------------------------------------------------------------------
enable_testing()
find_package(Catch2 3 CONFIG QUIET)
if(TARGET Catch2::Catch2WithMain)
  set(MV_CATCH2_TARGET Catch2::Catch2WithMain)
elseif(MV_CATCH2_AMALGAMATED_DIR)
  add_library(mv_catch2 STATIC "${MV_CATCH2_AMALGAMATED_DIR}/catch_amalgamated.cpp")
  target_include_directories(mv_catch2 PUBLIC "${MV_CATCH2_AMALGAMATED_DIR}")
  target_compile_definitions(mv_catch2 PUBLIC MV_CATCH2_AMALGAMATED=1)
  set(MV_CATCH2_TARGET mv_catch2)
else()
  message(FATAL_ERROR "Catch2 v3 not found: install it (vcpkg `catch2`) or set MV_CATCH2_AMALGAMATED_DIR")
endif()

add_executable(mv_import_tests
  "${MV_ROOT}/tests/test_content_hash.cpp"
  "${MV_ROOT}/tests/test_verified_copy.cpp"
  "${MV_ROOT}/tests/test_addon_manifest.cpp"
  "${MV_ROOT}/tests/test_import_naming.cpp"
  "${MV_ROOT}/tests/test_import_engine.cpp"
  "${MV_ROOT}/tests/test_json.cpp"
)
target_link_libraries(mv_import_tests PRIVATE mv_io mv_addon mv_import_engine
  ${MV_SODIUM_TARGET} ${MV_CATCH2_TARGET})
target_include_directories(mv_import_tests PRIVATE "${MV_ROOT}/src" "${MV_ROOT}/tests")
target_compile_definitions(mv_import_tests PRIVATE
  MV_IMPORT_MODULE_PATH="$<TARGET_FILE:mv_import>")
add_dependencies(mv_import_tests mv_import)
add_test(NAME mv_import_tests COMMAND mv_import_tests)
# The dateless-file fallback is local wall-clock time; pin the zone.
set_tests_properties(mv_import_tests PROPERTIES ENVIRONMENT "TZ=UTC")

# The packing tool's cross-check (tools/package/test_addon_pack.py).
add_executable(mv_addon_verify "${MV_ROOT}/tools/addon-verify/main.cpp")
target_link_libraries(mv_addon_verify PRIVATE mv_addon mv_io)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME addon_pack COMMAND Python3::Interpreter
           "${MV_ROOT}/tools/package/test_addon_pack.py" -v)
  set_tests_properties(addon_pack PROPERTIES
    ENVIRONMENT "MV_ADDON_VERIFY=$<TARGET_FILE:mv_addon_verify>")
endif()

# ---- PR 13 / 14: the clip core (edit/clip*), when FFmpeg is present ------------
# Stream-copy trim, the Path 2 re-encode loop (driven by a software encoder in
# the tests only), extract and remux, and the job queue. hwencode_none.cpp:
# a Linux runner has no hardware encoder, so the product Path 2 reports
# unsupported_format there, as it would on a machine without one.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(MV_FFMPEG QUIET IMPORTED_TARGET
    libavformat libavcodec libavfilter libavutil libswscale libswresample)
  pkg_check_modules(MV_SPNG QUIET IMPORTED_TARGET spng)
  find_package(JPEG QUIET)
endif()
if(MV_FFMPEG_FOUND AND MV_SPNG_FOUND AND JPEG_FOUND)
  add_library(mv_clip STATIC
    "${MV_ROOT}/src/edit/clip_common.cpp"
    "${MV_ROOT}/src/edit/clip_copy.cpp"
    "${MV_ROOT}/src/edit/clip_encode.cpp"
    "${MV_ROOT}/src/edit/clip_run.cpp"
    "${MV_ROOT}/src/edit/clip_jobs.cpp"
    "${MV_ROOT}/src/edit/clip_wire.cpp"
    "${MV_ROOT}/src/edit/clip_helper.cpp"
    "${MV_ROOT}/src/edit/hwencode_none.cpp"
    "${MV_ROOT}/src/edit/encode.cpp"
  )
  target_include_directories(mv_clip PUBLIC "${MV_ROOT}/src")
  target_link_libraries(mv_clip PUBLIC mv_io mv_project_options PkgConfig::MV_FFMPEG
    PRIVATE PkgConfig::MV_SPNG JPEG::JPEG)

  # The clip helper (tools/clipjob) with its test hooks: a crash, a hang, a
  # software encoder. The product helper is built without them.
  add_executable(mv_clipjob_test "${MV_ROOT}/tools/clipjob/main.cpp")
  target_link_libraries(mv_clipjob_test PRIVATE mv_clip)
  target_compile_definitions(mv_clipjob_test PRIVATE MV_CLIPJOB_TEST_HOOKS=1)

  add_executable(mv_clip_tests
    "${MV_ROOT}/tests/test_clip_helper.cpp"
    "${MV_ROOT}/tests/test_clip.cpp"
    "${MV_ROOT}/tests/test_clip_session.cpp"
    "${MV_ROOT}/src/abi/clip_session.cpp")
  target_link_libraries(mv_clip_tests PRIVATE mv_clip PkgConfig::MV_SPNG ${MV_CATCH2_TARGET})
  target_include_directories(mv_clip_tests PRIVATE "${MV_ROOT}/src" "${MV_ROOT}/tests"
    "${MV_ROOT}/src/abi/include")
  target_compile_definitions(mv_clip_tests PRIVATE MV_CLIPJOB_PATH="$<TARGET_FILE:mv_clipjob_test>")
  add_dependencies(mv_clip_tests mv_clipjob_test)
  add_test(NAME mv_clip_tests COMMAND mv_clip_tests)

  # The host-side trim model and the command table rows it adds (shell/ code
  # with no window: both hosts drive it).
  add_executable(mv_trim_tests
    "${MV_ROOT}/tests/test_trim_state.cpp"
    "${MV_ROOT}/src/shell/trim_state.cpp"
    "${MV_ROOT}/src/shell/command_table.cpp"
    "${MV_ROOT}/src/shell/key_router.cpp")
  target_link_libraries(mv_trim_tests PRIVATE mv_clip ${MV_CATCH2_TARGET})
  target_include_directories(mv_trim_tests PRIVATE "${MV_ROOT}/src" "${MV_ROOT}/tests")
  add_test(NAME mv_trim_tests COMMAND mv_trim_tests)
else()
  message(STATUS "FFmpeg / libspng / libjpeg not found: the PR 13/14 clip tests are not built")
endif()
