# SPDX-License-Identifier: GPL-2.0-or-later
#
# MediaViewer — native core + present lab.
#
# plan/09-build-and-test.md: CMake >= 3.28, vcpkg manifest mode, MSVC 2022
# primary with clang-cl in CI as a second opinion, /W4 /WX /permissive- /GR-
# /utf-8, and an ASan configuration.
#
# The C# shell (src.managed) is a sibling project built by dotnet, not by this
# file. The core builds and tests standalone with no shell present — that is the
# property that keeps it headlessly testable, and it is enforced by the fact
# that nothing here knows the shell exists.

cmake_minimum_required(VERSION 3.28)

# The toolchain file must be set before project(). Prefer an explicit
# -DCMAKE_TOOLCHAIN_FILE, then VCPKG_ROOT, then VCPKG_INSTALLATION_ROOT (the
# name GitHub's hosted runners use), then a checkout beside the user profile.
#
# Finding it here rather than passing -DCMAKE_TOOLCHAIN_FILE on the command line
# is deliberate: a path built by string interpolation in a CI `run:` step is
# silently wrong when the step runs under a shell that does not expand it, and
# the resulting CMake error names the toolchain file rather than the shell.
foreach(candidate IN ITEMS "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}"
                           "$ENV{USERPROFILE}/vcpkg")
  if(DEFINED CMAKE_TOOLCHAIN_FILE)
    break()
  endif()
  if(candidate AND EXISTS "${candidate}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${candidate}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
    message(STATUS "vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
  endif()
endforeach()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "No vcpkg toolchain found. Set VCPKG_ROOT, or pass "
    "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake. See README.md.")
endif()

project(mediaviewer
  VERSION 0.1.0
  DESCRIPTION "Windows viewer for a real camera dump"
  LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MediaViewer is a Windows application. plan/01-decisions.md.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(MV_BUILD_TESTS "Build the core test suite" ON)
option(MV_ASAN "Build with AddressSanitizer (plan/09 CI configuration)" OFF)

# ---------------------------------------------------------------------------
# Warnings and language settings — CLAUDE.md, "Native core (C++20)"
# ---------------------------------------------------------------------------
add_library(mv_project_options INTERFACE)

if(MSVC)
  target_compile_options(mv_project_options INTERFACE
    /W4 /WX
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8
    /GR-                    # no RTTI
    /MP
    # C4324: structure padded due to alignas. We align to a cache line on
    # purpose in spsc_ring.h; the warning is the design working.
    /wd4324
  )
  target_compile_definitions(mv_project_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
  )
  # clang-cl in CI catches different bugs than MSVC (plan/09). It also
  # diagnoses a few MSVC-isms that are not ours to fix.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND
     CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mv_project_options INTERFACE
      -Wno-unused-command-line-argument
      -Wno-language-extension-token)
  endif()
endif()

if(MV_ASAN)
  if(MSVC)
    target_compile_options(mv_project_options INTERFACE /fsanitize=address /Zi)
    target_link_options(mv_project_options INTERFACE /INCREMENTAL:NO)
    # ASan is incompatible with the /RTC checks MSVC adds in Debug.
    string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  endif()
endif()

# ---------------------------------------------------------------------------
# mv_core — arena, job system, result<T>, logging, ETW
#
# Dependencies point downward only (plan/02):
#   shell -> abi -> {canvas, edit, player, image, meta} -> {codec, gfx, io} -> core
# No back-edges, and no native module may depend on shell.
# tools/check-module-graph.ps1 enforces it in CI.
# ---------------------------------------------------------------------------
add_library(mv_core STATIC
  src/core/job_system.cpp
  src/core/trace.cpp
  src/core/job_system.h
  src/core/result.h
  src/core/spsc_ring.h
  src/core/status.h
  src/core/trace.h
)
target_include_directories(mv_core PUBLIC src)
target_link_libraries(mv_core PUBLIC mv_project_options)
add_library(mv::core ALIAS mv_core)

# ---------------------------------------------------------------------------
# mv_gfx — D3D11 device, swapchain, frame pacer
# ---------------------------------------------------------------------------
add_library(mv_gfx STATIC
  src/gfx/device.cpp
  src/gfx/pacer.cpp
  src/gfx/swapchain.cpp
  src/gfx/device.h
  src/gfx/pacer.h
  src/gfx/swapchain.h
)
target_link_libraries(mv_gfx
  PUBLIC mv_core
  PRIVATE d3d11 dxgi dcomp)
add_library(mv::gfx ALIAS mv_gfx)

# ---------------------------------------------------------------------------
# mediaviewer_core — the shared library exposing the flat C ABI.
#
# A DLL, not a static lib: the C# shell P/Invokes it, and the ABI is the only
# way in. plan/14-abi.md.
# ---------------------------------------------------------------------------
add_library(mediaviewer_core SHARED
  src/abi/abi.cpp
  src/abi/guard.h
  src/abi/include/mediaviewer/mediaviewer.h
)
target_include_directories(mediaviewer_core
  PUBLIC  src/abi/include
  PRIVATE src)
target_compile_definitions(mediaviewer_core PRIVATE MV_BUILDING_CORE)
target_link_libraries(mediaviewer_core PRIVATE mv_core)
set_target_properties(mediaviewer_core PROPERTIES OUTPUT_NAME "mediaviewer_core")
add_library(mv::abi ALIAS mediaviewer_core)

# ---------------------------------------------------------------------------
# mediaviewer_lab — the PR 1 present lab.
#
# Stays in the tree as the debug harness (plan/10, PR 1). Under the D1
# amendment its window and swapchain are the app; PR 3 hosts WinUI chrome
# inside them rather than re-implementing presentation.
# ---------------------------------------------------------------------------
find_package(imgui CONFIG REQUIRED)

add_executable(mediaviewer_lab WIN32
  src/shell/main.cpp
  src/shell/present_lab.cpp
  src/shell/input_state.h
  src/shell/present_lab.h
  src/shell/app.manifest
)
target_link_libraries(mediaviewer_lab PRIVATE
  mv_gfx
  mediaviewer_core
  imgui::imgui
  dwmapi
  shell32
  avrt      # MMCSS for the render thread
)
target_include_directories(mediaviewer_lab PRIVATE src src/abi/include)

# ---------------------------------------------------------------------------
# tools/frametime — the frame-time regression harness (plan/09).
# ---------------------------------------------------------------------------
add_executable(mv_frametime
  tools/frametime/main.cpp
)
target_link_libraries(mv_frametime PRIVATE mv_core)
target_include_directories(mv_frametime PRIVATE src)
set_target_properties(mv_frametime PROPERTIES OUTPUT_NAME "frametime")
add_dependencies(mv_frametime mediaviewer_lab)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MV_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(mv_tests
    tests/test_result.cpp
    tests/test_spsc_ring.cpp
    tests/test_job_system.cpp
    tests/test_abi_roundtrip.cpp
    tests/test_pacer.cpp
  )
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_gfx
    mediaviewer_core
    Catch2::Catch2WithMain)
  target_include_directories(mv_tests PRIVATE src src/abi/include)

  include(Catch)
  catch_discover_tests(mv_tests)

  # The DLL must sit beside the test exe for the P/Invoke-shaped ABI tests.
  add_custom_command(TARGET mv_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mediaviewer_core> $<TARGET_FILE_DIR:mv_tests>)
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — licence GPL-2.0-or-later (plan/11)")
