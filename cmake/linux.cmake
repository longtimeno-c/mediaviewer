# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Linux host, Linux PR 1 (docs/plans/linux.md): the OpenGL/EGL present lab on
# Wayland, frametime, and the portable core tests. Included from the root
# CMakeLists.txt, which then returns, so none of the Windows targets are
# defined. The viewer (decode, Qt chrome, video) is later slices.
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DMV_IMGUI_SOURCE_DIR=<imgui checkout>      # without vcpkg
#
# Dependencies: wayland-client, wayland-egl and xkbcommon (pkg-config),
# glvnd's OpenGL and EGL (FindOpenGL), wayland-scanner, imgui with its
# OpenGL 3 backend and Catch2 v3. With vcpkg (CI) imgui and Catch2 come from
# the root manifest; without it, MV_IMGUI_SOURCE_DIR names an imgui source
# tree (the pinned 1.92.8) and Catch2 comes from CMAKE_PREFIX_PATH.

enable_language(C)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MV_WAYLAND REQUIRED IMPORTED_TARGET wayland-client>=1.20 wayland-egl xkbcommon)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-rtti>
  # GCC's -O2 flow analysis reports std::optional members read only behind
  # has_value() as "may be used uninitialized" (cmake/portable has the same
  # line); clang and MSVC do not. Off for GCC only.
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>)
target_compile_definitions(mv_project_options INTERFACE MV_LINUX=1)
if(MV_ASAN)
  target_compile_options(mv_project_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(mv_project_options INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Wayland protocols: vendored XML (tools/linux/protocols, MIT), generated here.
# ---------------------------------------------------------------------------
set(MV_WAYLAND_GEN_DIR "${CMAKE_BINARY_DIR}/wayland")
file(MAKE_DIRECTORY "${MV_WAYLAND_GEN_DIR}")
set(MV_WAYLAND_PROTOCOL_SOURCES)
foreach(protocol IN ITEMS xdg-shell presentation-time viewporter fractional-scale-v1
                          xdg-decoration-unstable-v1)
  set(xml "${CMAKE_SOURCE_DIR}/tools/linux/protocols/${protocol}.xml")
  set(header "${MV_WAYLAND_GEN_DIR}/${protocol}-client-protocol.h")
  set(code "${MV_WAYLAND_GEN_DIR}/${protocol}-protocol.c")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
    DEPENDS "${xml}" VERBATIM)
  add_custom_command(OUTPUT "${code}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
    DEPENDS "${xml}" VERBATIM)
  list(APPEND MV_WAYLAND_PROTOCOL_SOURCES "${header}" "${code}")
endforeach()
add_library(mv_wayland_protocols STATIC ${MV_WAYLAND_PROTOCOL_SOURCES})
target_include_directories(mv_wayland_protocols PUBLIC "${MV_WAYLAND_GEN_DIR}")
target_link_libraries(mv_wayland_protocols PUBLIC PkgConfig::MV_WAYLAND)

# ---------------------------------------------------------------------------
# Core
# ---------------------------------------------------------------------------
add_library(mv_core STATIC
  src/core/job_system.cpp
  src/core/job_system_posix.cpp
  src/core/trace_posix.cpp
  src/core/boot_timeline.cpp
  src/core/crash_context.cpp
  src/core/boot_timeline.h
  src/core/crash_context.h
  src/core/job_system.h
  src/core/job_system_platform.h
  src/core/parallel.h
  src/core/result.h
  src/core/spsc_ring.h
  src/core/status.h
  src/core/trace.h
)
target_include_directories(mv_core PUBLIC src)
target_link_libraries(mv_core PUBLIC mv_project_options Threads::Threads)
add_library(mv::core ALIAS mv_core)

# ---------------------------------------------------------------------------
# gfx: the portable pacers (metal_pacer is kept for the tests, as on Windows)
# and the OpenGL / EGL / Wayland backend. Its headers name no EGL, GL or
# Wayland type (D9).
# ---------------------------------------------------------------------------
add_library(mv_gfx STATIC
  src/gfx/metal_pacer.cpp
  src/gfx/presentation_pacer.cpp
  src/gfx/device_gl.cpp
  src/gfx/gl_surface_wayland.cpp
  src/gfx/wayland_frame_clock.cpp
  src/gfx/metal_pacer.h
  src/gfx/presentation_pacer.h
  src/gfx/frame_histogram.h
  src/gfx/present_policy.h
  src/gfx/pace_json.h
  src/gfx/device_gl.h
  src/gfx/gl_surface_wayland.h
  src/gfx/wayland_frame_clock.h
)
target_link_libraries(mv_gfx PUBLIC mv_core
  PRIVATE mv_wayland_protocols OpenGL::OpenGL OpenGL::EGL PkgConfig::MV_WAYLAND)
add_library(mv::gfx ALIAS mv_gfx)

add_library(mv_canvas STATIC src/canvas/camera.cpp src/canvas/camera.h src/canvas/spring.h)
target_link_libraries(mv_canvas PUBLIC mv_core)

# ---------------------------------------------------------------------------
# imgui: the present lab and the F3 overlay only (D1), OpenGL 3 backend, no
# platform backend (shell/input_state.h). vcpkg's port when there is one, else
# a source tree.
# ---------------------------------------------------------------------------
find_package(imgui CONFIG QUIET)
if(TARGET imgui::imgui)
  set(MV_IMGUI_TARGET imgui::imgui)
else()
  set(MV_IMGUI_SOURCE_DIR "" CACHE PATH "imgui source tree (1.92.8) when vcpkg has none")
  if(NOT MV_IMGUI_SOURCE_DIR OR NOT EXISTS "${MV_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
    message(FATAL_ERROR
      "imgui not found: configure with vcpkg (the root manifest's imgui[opengl3-binding]) "
      "or set MV_IMGUI_SOURCE_DIR to an imgui 1.92.8 source tree")
  endif()
  add_library(mv_imgui STATIC
    "${MV_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${MV_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${MV_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${MV_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${MV_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    "${MV_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
  target_include_directories(mv_imgui SYSTEM PUBLIC
    "${MV_IMGUI_SOURCE_DIR}" "${MV_IMGUI_SOURCE_DIR}/backends")
  # Third-party code: not held to -Werror. Its loader dlopens libOpenGL.so.0.
  target_link_libraries(mv_imgui PRIVATE ${CMAKE_DL_LIBS})
  set(MV_IMGUI_TARGET mv_imgui)
endif()

# ---------------------------------------------------------------------------
# mediaviewer_lab: the Wayland toplevel and the present lab in it.
# ---------------------------------------------------------------------------
add_executable(mediaviewer_lab
  src/shell/main_linux.cpp
  src/shell/present_lab_linux.cpp
  src/shell/present_lab_linux.h
  src/shell/wayland_window_linux.cpp
  src/shell/wayland_window_linux.h
  src/shell/input_state.h
)
target_link_libraries(mediaviewer_lab PRIVATE mv_gfx mv_core mv_wayland_protocols
  ${MV_IMGUI_TARGET} OpenGL::OpenGL OpenGL::EGL PkgConfig::MV_WAYLAND)
add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")

add_executable(mv_frametime tools/frametime/main_linux.cpp)
target_link_libraries(mv_frametime PRIVATE mv_project_options)
target_include_directories(mv_frametime PRIVATE src tools)
set_target_properties(mv_frametime PROPERTIES OUTPUT_NAME "frametime")
add_dependencies(mv_frametime mediaviewer_lab)

# ---------------------------------------------------------------------------
# Tests: the portable suites that apply to Linux PR 1.
# ---------------------------------------------------------------------------
if(MV_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(mv_tests
    tests/test_result.cpp
    tests/test_spsc_ring.cpp
    tests/test_job_system.cpp
    tests/test_boot_timeline.cpp
    tests/test_input_state.cpp
    tests/test_camera.cpp
    tests/test_present_policy.cpp
    tests/test_metal_pacer.cpp
    tests/test_presentation_pacer.cpp
    tests/test_frametime_report.cpp
  )
  target_link_libraries(mv_tests PRIVATE mv_gfx mv_canvas mv_core Catch2::Catch2WithMain)
  target_include_directories(mv_tests PRIVATE src tools tests)
  include(Catch)
  # As on Windows and the Mac: Catch2's SKIPPED marker reads as Skipped, and a
  # wedged test is named by ctest rather than running until the job dies.
  set(MV_TEST_TIMEOUT 300)
  catch_discover_tests(mv_tests
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT ${MV_TEST_TIMEOUT})

  # The harness against synthetic reports (tests/test_frametime.ps1's twin).
  add_executable(mv_frametime_fixture tests/frametime_fixture_posix.cpp)
  target_link_libraries(mv_frametime_fixture PRIVATE mv_project_options)
  add_test(NAME frametime_harness COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_frametime.sh"
    "$<TARGET_FILE:mv_frametime>" "$<TARGET_FILE:mv_frametime_fixture>"
    "${CMAKE_BINARY_DIR}/frametime-tests")
  set_tests_properties(frametime_harness PROPERTIES TIMEOUT 60)

  # The headless-Weston smoke (tools/linux/lab_smoke.sh): runs only where a
  # weston binary exists, and proves plumbing, never pacing.
  find_program(MV_WESTON weston)
  if(MV_WESTON)
    add_test(NAME linux_lab_smoke
      COMMAND bash "${CMAKE_SOURCE_DIR}/tools/linux/lab_smoke.sh"
              "$<TARGET_FILE:mediaviewer_lab>" "$<TARGET_FILE:mv_frametime>")
    set_tests_properties(linux_lab_smoke PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
  endif()
endif()
