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
    # vcpkg static libs (Catch2 today; later C++ ports) are built without
    # /fsanitize=address. MSVC STL ASan container annotations change object ABI
    # (annotate_string / annotate_vector / annotate_optional) and the linker
    # refuses to mix them (LNK2038). Disable the annotations so our objects
    # match the vcpkg ABI. Heap, stack, and global ASan checks still run.
    # https://learn.microsoft.com/cpp/sanitizers/error-container-overflow
    target_compile_definitions(mv_project_options INTERFACE
      _DISABLE_VECTOR_ANNOTATION
      _DISABLE_STRING_ANNOTATION
      _DISABLE_OPTIONAL_ANNOTATION)
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
  src/core/job_system_win.cpp
  src/core/trace_win.cpp
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
  src/gfx/blit.cpp
  src/gfx/device.h
  src/gfx/pacer.h
  src/gfx/swapchain.h
  src/gfx/blit.h
  src/gfx/video_blit.cpp
  src/gfx/video_blit.h
  src/gfx/colour_desc.cpp
  src/gfx/colour_desc.h
)
target_link_libraries(mv_gfx
  PUBLIC mv_core
  PRIVATE d3d11 dxgi dcomp d3dcompiler)
add_library(mv::gfx ALIAS mv_gfx)

# ---------------------------------------------------------------------------
# mv_io — whole-file reads (worker threads only)
# ---------------------------------------------------------------------------
add_library(mv_io STATIC
  src/io/file.cpp
  src/io/file.h
  src/io/dir_win.cpp
  src/io/dir.h
  src/io/paths_win.cpp
  src/io/paths.h
)
target_link_libraries(mv_io PUBLIC mv_core PRIVATE shell32)
add_library(mv::io ALIAS mv_io)

# ---------------------------------------------------------------------------
# mv_codec — JPEG / PNG / BMP, magic-byte probe (plan/04)
# ---------------------------------------------------------------------------
find_package(JPEG REQUIRED)
find_package(spng CONFIG REQUIRED)
if(TARGET spng::spng)
  set(MV_SPNG_TARGET spng::spng)
elseif(TARGET spng::spng_static)
  set(MV_SPNG_TARGET spng::spng_static)
else()
  message(FATAL_ERROR "libspng imported target not found")
endif()

add_library(mv_codec STATIC
  src/codec/probe.cpp
  src/codec/decode.cpp
  src/codec/jpeg.cpp
  src/codec/png.cpp
  src/codec/bmp.cpp
  src/codec/decode.h
  src/codec/format.h
  src/codec/raster.h
)
target_link_libraries(mv_codec
  PUBLIC mv_core
  PRIVATE JPEG::JPEG ${MV_SPNG_TARGET})
add_library(mv::codec ALIAS mv_codec)

# ---------------------------------------------------------------------------
# mv_image — colour (LCMS) + immutable GPU upload
# ---------------------------------------------------------------------------
find_package(lcms2 CONFIG REQUIRED)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(mv_image STATIC
  src/image/colour.cpp
  src/image/pipeline.cpp
  src/image/upload.cpp
  src/image/thumb.cpp
  src/image/colour.h
  src/image/pipeline.h
  src/image/upload.h
  src/image/gpu_image.h
  src/image/thumb.h
)
target_link_libraries(mv_image
  PUBLIC mv_codec mv_gfx
  PRIVATE mv_io lcms2::lcms2 unofficial::sqlite3::sqlite3)
add_library(mv::image ALIAS mv_image)

# ---------------------------------------------------------------------------
# mv_player — demux, decode, A/V clock, transport (PR 5a/5b/5c)
#
# Depends downward only: player -> {codec, gfx, io, core}. Never image, never
# shell (tools/check-module-graph.ps1). FFmpeg is LGPL and dynamically linked;
# tools/licence-check.ps1 reads the configure string out of the built DLLs.
#
# The file list is complete for PR 5 on purpose: three sessions share this
# checkout, and CMakeLists.txt is the one file they would all otherwise edit.
# Fill in a stub rather than adding a target source.
# ---------------------------------------------------------------------------
find_package(FFMPEG REQUIRED)

add_library(mv_player STATIC
  # 5a — silent video into the same swapchain
  src/player/demux.cpp
  src/player/video_decode.cpp
  src/player/frame_ring.cpp
  src/player/video_source.cpp
  src/player/media_source.cpp
  src/player/hwdecode_win.cpp      # D9: the only player/ file that may see d3d11.h
  # 5b — audio and the clock
  src/player/audio_decode.cpp
  src/player/av_clock.cpp
  src/player/audio_win.cpp         # D9: the only player/ file that may see WASAPI
  # 5c — transport
  src/player/transport.cpp
  src/player/presenter.cpp
  src/player/container_probe.cpp
  src/player/poster.cpp
  # headers, listed so they show up in the IDE
  src/player/media_source.h
  src/player/video_source.h
  src/player/audio_sink.h
  src/player/audio_block.h
  src/player/av_clock.h
  src/player/presenter.h
  src/player/transport.h
  src/player/container_probe.h
  src/player/poster.h
  src/player/video_internal.h
)
# SYSTEM, deliberately. FFmpeg's own public headers trip C4244 (libavutil's
# common.h narrows int -> uint8_t/int16_t in its inline helpers), and CLAUDE.md
# compiles /W4 /WX, so an ordinary include of <libavutil/common.h> fails the
# build inside a dependency we do not control. Marking the directory SYSTEM
# suppresses warnings from THEIR headers while leaving /W4 /WX fully in force on
# ours -- which is the point. Relaxing /WX for mv_player instead would silence
# our own warnings too, in the one module doing the hardest threading in the app.
target_include_directories(mv_player SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mv_player PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mv_player
  PUBLIC mv_core mv_gfx
  PRIVATE mv_codec mv_io ${FFMPEG_LIBRARIES}
          mfplat mfuuid ole32 uuid avrt ksuser)
add_library(mv::player ALIAS mv_player)

# ---------------------------------------------------------------------------
# mv_canvas — pan/zoom springs (plan/03)
# ---------------------------------------------------------------------------
add_library(mv_canvas STATIC
  src/canvas/camera.cpp
  src/canvas/camera.h
  src/canvas/spring.h
)
target_link_libraries(mv_canvas PUBLIC mv_core)
add_library(mv::canvas ALIAS mv_canvas)

# ---------------------------------------------------------------------------
# mediaviewer_core — the shared library exposing the flat C ABI.
#
# A DLL, not a static lib: the C# shell P/Invokes it, and the ABI is the only
# way in. plan/14-abi.md.
# ---------------------------------------------------------------------------
add_library(mediaviewer_core SHARED
  src/abi/abi.cpp
  src/abi/video_session.h
  src/abi/guard.h
  src/abi/include/mediaviewer/mediaviewer.h
)
target_include_directories(mediaviewer_core
  PUBLIC  src/abi/include
  PRIVATE src)
target_compile_definitions(mediaviewer_core PRIVATE MV_BUILDING_CORE)
target_link_libraries(mediaviewer_core PRIVATE mv_core mv_image mv_canvas mv_io mv_player)
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
  src/shell/av_soak.cpp
  src/shell/av_soak.h
  src/shell/chrome_host.cpp
  src/shell/chrome_host.h
  src/shell/settings.cpp
  src/shell/settings.h
  src/shell/input_state.h
  src/shell/present_lab.h
  src/shell/app.manifest
)
target_link_libraries(mediaviewer_lab PRIVATE
  mv_player
  mv_gfx
  mv_canvas
  mv_io
  mediaviewer_core
  imgui::imgui
  dwmapi
  shell32
  comdlg32
  ole32     # CoInitializeEx for the WinUI island STA
  uuid      # IFileOpenDialog
  avrt      # MMCSS for the render thread
)
target_include_directories(mediaviewer_lab PRIVATE src src/abi/include)
add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")

# PR 3: publish the C# WinUI chrome beside the lab so hostfxr can load it.
# The core still builds with no .NET; chrome is a lab-side host, not a back-edge.
find_program(DOTNET_EXE dotnet)
set(MV_CHROME_PROJ "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/MediaViewer.Chrome.csproj")
set(MV_CHROME_SOURCES
  ${MV_CHROME_PROJ}
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Filmstrip.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Video.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/ChromeApp.cs"
)
if(DOTNET_EXE)
  # Publish beside the lab on every lab link so `cmake --build --target
  # mediaviewer_lab` is enough to load the island. LINK_DEPENDS republishes
  # when the C# sources change without a native rebuild.
  add_custom_command(TARGET mediaviewer_lab POST_BUILD
    COMMAND ${DOTNET_EXE} publish ${MV_CHROME_PROJ}
            --nologo
            --configuration $<IF:$<CONFIG:Debug>,Debug,Release>
            -r win-x64
            --no-self-contained
            -p:Platform=x64
            -o $<TARGET_FILE_DIR:mediaviewer_lab>
    COMMENT "Publishing MediaViewer.Chrome beside mediaviewer_lab"
    VERBATIM
  )
  set_property(TARGET mediaviewer_lab APPEND PROPERTY LINK_DEPENDS ${MV_CHROME_SOURCES})
  add_custom_target(mv_chrome DEPENDS mediaviewer_lab ${MV_CHROME_SOURCES})
else()
  message(STATUS "dotnet not found — lab will run without WinUI chrome (pass --no-chrome)")
endif()

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
if(TARGET mv_chrome)
  add_dependencies(mv_frametime mv_chrome)
endif()

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
    tests/test_input_state.cpp
    tests/test_probe.cpp
    tests/test_decode.cpp
    tests/test_colour.cpp
    tests/test_camera.cpp
    tests/test_abi_image.cpp
    tests/test_upload.cpp
    tests/test_chrome_host.cpp
    tests/test_dir.cpp
    tests/test_folder.cpp
    tests/test_presenter.cpp
    tests/test_colour_desc.cpp
    tests/test_video_transport.cpp
    tests/test_video_ring.cpp
    tests/test_video_colour.cpp
    tests/test_av_clock.cpp
    tests/test_audio_sink.cpp
    tests/test_transport.cpp
    tests/test_container_probe.cpp
    tests/test_corpus.cpp
    src/shell/chrome_host.cpp
    src/shell/settings.cpp
    src/shell/app.manifest
  )
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_gfx
    mv_player
    mv_codec
    mv_image
    mv_canvas
    mediaviewer_core
    JPEG::JPEG
    ${MV_SPNG_TARGET}
    lcms2::lcms2
    Catch2::Catch2WithMain
    ole32
    uuid
    mv_io)
  target_include_directories(mv_tests PRIVATE src src/abi/include)

  include(Catch)
  # A Catch2 SKIP exits 0, so ctest counted every corpus-gated video test as
  # Passed: a clean clone reported "100% tests passed" with twelve cases —
  # hardware decode, colour, seek, frame step, texture leaks, the video ABI —
  # silently absent. Matching Catch2's own SKIPPED marker makes ctest report
  # them as Skipped instead, so the summary says how much was not proved.
  # tests/corpus.h has the rest: a loud report, and MV_REQUIRE_CORPUS=1 to turn
  # a missing clip into a failure in CI.
  catch_discover_tests(mv_tests PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
  if(TARGET mv_chrome)
    add_dependencies(mv_tests mv_chrome)
  endif()

  add_executable(mv_frametime_fixture tests/frametime_fixture.cpp)
  add_executable(mv_swapchain_recovery tests/swapchain_recovery.cpp)
  target_link_libraries(mv_swapchain_recovery PRIVATE mv_gfx)
  target_link_libraries(mv_frametime_fixture PRIVATE mv_project_options)
  add_test(NAME frametime_harness COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frametime.ps1"
    -Harness $<TARGET_FILE:mv_frametime> -Fixture $<TARGET_FILE:mv_frametime_fixture>
    -OutputRoot "${CMAKE_CURRENT_BINARY_DIR}/frametime-tests")
  set_tests_properties(frametime_harness PROPERTIES TIMEOUT 60)

  # The DLL must sit beside the test exe for the P/Invoke-shaped ABI tests.
  add_custom_command(TARGET mv_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mediaviewer_core> $<TARGET_FILE_DIR:mv_tests>)
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — licence GPL-2.0-or-later (plan/11)")
