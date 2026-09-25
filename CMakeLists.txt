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
  VERSION 0.1.3
  DESCRIPTION "Windows viewer for a real camera dump; macOS present lab is Milestone F"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(MV_BUILD_TESTS "Build the core test suite" ON)
option(MV_ASAN "Build with AddressSanitizer (plan/09 CI configuration)" OFF)

# PR 16: Apple Silicon present lab. Windows targets are not defined on Darwin.
if(APPLE)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/darwin.cmake")
  return()
endif()
if(NOT WIN32)
  message(FATAL_ERROR
    "MediaViewer v1 is a Windows application; macOS is Milestone F "
    "(plan/15-platforms.md).")
endif()

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
      MV_ASAN=1
      _DISABLE_VECTOR_ANNOTATION
      _DISABLE_STRING_ANNOTATION
      _DISABLE_OPTIONAL_ANNOTATION)

    # The ASan runtime is a DLL, and it ships beside cl.exe and nowhere else:
    # "These files are never placed in C:\Windows\System32 ... These locations
    # are on the path in debugging sessions and in Visual Studio developer
    # command prompts."
    # https://learn.microsoft.com/cpp/sanitizers/asan-known-issues
    #
    # Nothing copies it for us. vcpkg's applocal deploy only knows DLLs from the
    # vcpkg tree, and CMake does not know about it at all. So an ASan build left
    # to itself produces executables that run only inside a developer prompt.
    #
    # That is exactly how the msvc-asan CI leg failed from the day it was added.
    # `catch_discover_tests` enumerates by running the test exe as an MSBuild
    # POST_BUILD step, and MSBuild puts VC\Tools\MSVC\<ver>\bin\HostX64\x64
    # first on PATH -- so discovery found all 426 tests in under a second. The
    # `ctest` step is a bare shell with no developer environment, so every one
    # of those 426 processes died in the loader instead. A loader failure is a
    # hard error, not a message on stderr: the process sits on it until killed,
    # which ctest reports as "***Timeout 300.02 sec" with no output at all --
    # no stack, no exit code, nothing to read. Copying the DLL beside the
    # binaries fixes it for ctest, for a double-click, and for anyone running
    # the exe by hand, which is why it belongs here and not in a CI `env:` line.
    get_filename_component(MV_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB MV_ASAN_RUNTIME "${MV_MSVC_BIN}/clang_rt.asan*dynamic-*.dll")
    if(NOT MV_ASAN_RUNTIME)
      message(FATAL_ERROR
        "MV_ASAN=ON but no ASan runtime DLL was found next to the compiler:\n"
        "  ${MV_MSVC_BIN}\n"
        "Install the \"C++ AddressSanitizer\" component "
        "(Microsoft.VisualStudio.Component.VC.ASAN) in the Visual Studio "
        "Installer. Configuring without it yields a build whose every test "
        "hangs for the full timeout with nothing in the log.")
    endif()
    message(STATUS "ASan runtime: ${MV_ASAN_RUNTIME}")
  endif()
endif()

# Put the ASan runtime beside a target's executable. A no-op unless MV_ASAN.
# Every executable needs it, including the ones that only load an instrumented
# DLL, because the runtime is resolved by the loader per process.
function(mv_asan_runtime_beside target)
  foreach(dll IN LISTS MV_ASAN_RUNTIME)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${dll}" "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copy the ASan runtime beside ${target}")
  endforeach()
endfunction()

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
  src/core/crash_context.cpp
  src/core/crash_context.h
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
  src/gfx/adjust_kernel.h
  src/gfx/pacer.cpp
  src/gfx/swapchain.cpp
  src/gfx/blit.cpp
  src/gfx/texture.cpp
  src/gfx/texture.h
  src/gfx/device.h
  src/gfx/pacer.h
  src/gfx/swapchain.h
  src/gfx/blit.h
  src/gfx/video_blit.cpp
  src/gfx/video_blit.h
  src/gfx/colour_desc.cpp
  src/gfx/colour_desc.h
  src/gfx/metal_pacer.cpp
  src/gfx/metal_pacer.h
  src/gfx/present_policy.h
  src/gfx/pace_json.h
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
  src/io/replace.h
  src/io/replace_win.cpp
  src/io/dir_win.cpp
  src/io/dir_tree.cpp
  src/io/dir.h
  src/io/paths_win.cpp
  src/io/paths.h
  src/io/collision_name.h
  src/io/file_ops.h
  src/io/file_ops_win.cpp
  src/io/pairing.h
  src/io/sort_order.cpp
  src/io/sort_order.h
  # Milestone G (plan/18): verified copy (also F8 across volumes), the file
  # and volume ports Import reaches through the add-on host table.
  src/io/content_hash.cpp
  src/io/content_hash.h
  src/io/file_port.cpp
  src/io/file_port.h
  src/io/file_port_win.cpp
  src/io/child_process.h
  src/io/child_process_win.cpp
  src/io/verified_copy.cpp
  src/io/verified_copy.h
  src/io/volume.h
  src/io/volume_win.cpp
)
# ole32 + uuid: IFileOperation for the Recycle Bin (PR 6).
# setupapi + cfgmgr32: the volume port's eject (plan/18).
# blake3: CC0-1.0 (plan/18, THIRD-PARTY.md).
find_package(blake3 CONFIG REQUIRED)
target_link_libraries(mv_io PUBLIC mv_core
  PRIVATE shell32 ole32 uuid setupapi cfgmgr32 BLAKE3::blake3)
add_library(mv::io ALIAS mv_io)

# ---------------------------------------------------------------------------
# mv_codec — JPEG / PNG / BMP, magic-byte probe (plan/04)
# ---------------------------------------------------------------------------
find_package(JPEG REQUIRED)
# PR 6: animated GIF / WebP (plan/12 2026-09-13, resequenced from PR 7).
# giflib is MIT, libwebp BSD-3: both permissive, dynamic in the x64-windows triplet.
find_package(GIF REQUIRED)
find_package(WebP CONFIG REQUIRED)
# PR 7: camera-dump formats. libheif default-features are OFF in vcpkg.json
# because the port's `hevc` feature is x265 encode (plan/11). libde265 is a
# hard dependency of the port, so HEIC decode does not pull a software HEVC
# encoder. LibRaw uses the thread-safe raw_r target — decode is on the pool.
find_package(TIFF REQUIRED)
find_package(libheif CONFIG REQUIRED)
if(TARGET libheif::heif)
  set(MV_HEIF_TARGET libheif::heif)
elseif(TARGET heif)
  set(MV_HEIF_TARGET heif)
else()
  message(FATAL_ERROR "libheif imported target not found")
endif()
find_package(libavif CONFIG REQUIRED)
if(TARGET avif)
  set(MV_AVIF_TARGET avif)
elseif(TARGET libavif::avif)
  set(MV_AVIF_TARGET libavif::avif)
else()
  message(FATAL_ERROR "libavif imported target not found")
endif()
include(cmake/raw-openmp.cmake)
find_package(libraw CONFIG REQUIRED)
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
  src/codec/anim.cpp
  src/codec/anim.h
  src/codec/apng.cpp
  src/codec/apng.h
  src/codec/gif.cpp
  src/codec/webp.cpp
  src/codec/tiff.cpp
  src/codec/ico.cpp
  src/codec/heif.cpp
  src/codec/avif.cpp
  src/codec/raw.cpp
  src/codec/raw_internal.h
  src/codec/exif.cpp
  src/codec/exif.h
  src/codec/orient.cpp
  src/codec/orient.h
  src/codec/orientation.h
  src/codec/os_decode_win.cpp
  src/codec/crash_test_hook.cpp
  src/codec/crash_test_hook.h
  src/codec/os_decode.h
  src/codec/decode.h
  src/codec/format.h
  src/codec/raster.h
)
target_link_libraries(mv_codec
  PUBLIC mv_core
  PRIVATE JPEG::JPEG ${MV_SPNG_TARGET} GIF::GIF WebP::webp WebP::webpdemux
          TIFF::TIFF ${MV_HEIF_TARGET} ${MV_AVIF_TARGET} libraw::raw_r
          windowscodecs ole32)
add_library(mv::codec ALIAS mv_codec)
target_link_libraries(mv_codec PRIVATE mv_raw_openmp)

# ---------------------------------------------------------------------------
# mv_image — colour (LCMS) + immutable GPU upload
# ---------------------------------------------------------------------------
find_package(lcms2 CONFIG REQUIRED)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(mv_image STATIC
  src/image/colour.cpp
  src/image/linear.cpp
  src/image/linear.h
  src/image/half.h
  src/image/pipeline.cpp
  src/image/upload.cpp
  src/image/tiles.cpp
  src/image/thumb.cpp
  src/image/colour.h
  src/image/pipeline.h
  src/image/upload.h
  src/image/gpu_image.h
  src/image/tiles.h
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
# mv_meta -- PR 9 metadata read model (plan/06). Exiv2 (GPL-2.0, DLL only) for
# EXIF / IPTC / XMP and maker notes, libavformat for container and per-stream
# facts. Depends downward only: meta -> {codec, io, core}.
#
# Built and tested on Windows (plan/12 2026-09-24, /W4 /WX clean). Shares every
# source with the macOS twin. The vcpkg exiv2 port needs its `xmp`, `bmff` and
# `png` features (without `xmp` a PNG's XMP is silently empty).
# ---------------------------------------------------------------------------
find_package(exiv2 CONFIG REQUIRED)
add_library(mv_meta STATIC
  src/meta/read.cpp
  src/meta/still.cpp
  src/meta/carried.cpp
  src/meta/clip.cpp
  src/meta/af.cpp
  src/meta/format.cpp
  src/meta/tables.cpp
  src/meta/fields.cpp
  src/meta/sidecar.cpp
  src/meta/write.cpp
  src/meta/write.h
  src/meta/tables.h
  src/meta/meta.h
  src/meta/af.h
  src/meta/internal.h
)
target_include_directories(mv_meta SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mv_meta PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mv_meta
  PUBLIC mv_core
  PRIVATE mv_codec mv_io Exiv2::exiv2lib ${FFMPEG_LIBRARIES})
add_library(mv::meta ALIAS mv_meta)

# ---------------------------------------------------------------------------
# mv_edit -- PR 10 (plan/07): the EditStack and its geometry ops, the
# full-resolution CPU evaluation for export, lossless JPEG rotate / flip /
# MCU-aligned crop on libjpeg's coefficient API, and the JPEG / PNG export
# encoders. Pure C++ over codec + io + core: no GPU, no platform header (D9);
# the preview is the blit's output -> source map (gfx/blit*.h).
# PR 11: colour adjusts (adjust.*), the one-source shader kernel
# (gfx/adjust_kernel.h, pasted into both blits), the full-resolution bake over the
# FP16 working image (image/linear.h) and the histogram reduction.
# ---------------------------------------------------------------------------
add_library(mv_edit STATIC
  src/edit/adjust.cpp
  src/edit/adjust.h
  src/edit/bake.cpp
  src/edit/bake.h
  src/edit/histogram.cpp
  src/edit/histogram.h
  src/edit/edit_stack.cpp
  src/edit/edit_stack.h
  src/edit/geometry.cpp
  src/edit/geometry.h
  src/edit/lossless_jpeg.cpp
  src/edit/lossless_jpeg.h
  src/edit/encode.cpp
  src/edit/encode.h
  src/edit/export.cpp
  src/edit/export.h
  src/edit/metadata_policy.h
)
target_link_libraries(mv_edit
  PUBLIC mv_core mv_codec mv_image
  PRIVATE JPEG::JPEG ${MV_SPNG_TARGET})
add_library(mv::edit ALIAS mv_edit)

# ---------------------------------------------------------------------------
# mv_clip -- PR 13 / 14 (plan/08): two-path trim, extract & remux, and the
# clip job queue. Module `edit` in the dependency graph (src/edit/clip*), a
# separate library only because it links FFmpeg and mv_edit does not. The
# encode port's Windows half lists the hardware encoders (hwencode_win.cpp);
# FFmpeg is dynamic and LGPL (tools/licence-check.ps1). cmake/portable builds
# the same sources with hwencode_none.cpp for the Linux core tests.
# ---------------------------------------------------------------------------
add_library(mv_clip STATIC
  src/edit/clip.h
  src/edit/clip_internal.h
  src/edit/clip_common.cpp
  src/edit/clip_copy.cpp
  src/edit/clip_encode.cpp
  src/edit/clip_run.cpp
  src/edit/clip_jobs.cpp
  src/edit/clip_jobs.h
  src/edit/clip_wire.cpp
  src/edit/clip_wire.h
  src/edit/clip_helper.cpp
  src/edit/clip_helper.h
  src/edit/hwencode.h
  src/edit/hwencode_win.cpp
)
target_include_directories(mv_clip SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mv_clip PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mv_clip
  PUBLIC mv_core mv_io
  PRIVATE mv_edit ${FFMPEG_LIBRARIES})
add_library(mv::clip ALIAS mv_clip)

# MediaViewerClipJob.exe -- PR 13 / 14's encode and decode jobs, out of process
# (plan/08 "Execution & UX"; plan/12 2026-09-25). Beside MediaViewer.exe; no
# mv_ prefix, so tools/package/build-release.ps1 ships it in the payload.
add_executable(MediaViewerClipJob tools/clipjob/main.cpp)
target_link_libraries(MediaViewerClipJob PRIVATE mv_clip mv_project_options)
target_include_directories(MediaViewerClipJob PRIVATE src)
mv_asan_runtime_beside(MediaViewerClipJob)

# ---------------------------------------------------------------------------
# Milestone G: the add-on host (mv_addon, in the base app) and the Import
# add-on (mv_import, built beside it and packed separately; never installed
# with the app). cmake/import.cmake.
# ---------------------------------------------------------------------------
find_package(unofficial-sodium CONFIG REQUIRED)
set(MV_SQLITE_TARGET unofficial::sqlite3::sqlite3)
set(MV_SODIUM_TARGET unofficial-sodium::sodium)
include("${CMAKE_SOURCE_DIR}/cmake/import.cmake")

# ---------------------------------------------------------------------------
# mv_canvas — pan/zoom springs (plan/03)
# ---------------------------------------------------------------------------
add_library(mv_canvas STATIC
  src/canvas/camera.cpp
  src/canvas/camera.h
  src/canvas/spring.h
  src/canvas/refinement.h
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
  src/abi/addon_abi.cpp
  src/abi/crash_export.cpp
  src/abi/include/mediaviewer/mediaviewer_crash.h
  src/abi/video_session.h
  src/abi/guard.h
  src/abi/include/mediaviewer/mediaviewer.h
  src/abi/include/mediaviewer/mediaviewer_addon.h
  src/abi/include/mediaviewer/mediaviewer_import.h
  src/abi/include/mediaviewer/mediaviewer_clip.h
  src/abi/clip_session.cpp
  src/abi/clip_session.h
)
target_include_directories(mediaviewer_core
  PUBLIC  src/abi/include
  PRIVATE src)
target_compile_definitions(mediaviewer_core PRIVATE MV_BUILDING_CORE)
target_link_libraries(mediaviewer_core PRIVATE mv_core mv_image mv_canvas mv_io mv_player mv_meta mv_addon mv_clip)
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
# PR 7: Crashpad (Apache-2.0) client, static; crashpad_handler.exe runs
# out-of-process beside the lab (plan/13 Part 2).
find_package(crashpad CONFIG REQUIRED)

add_executable(mediaviewer_lab WIN32
  src/shell/main.cpp
  src/shell/present_lab.cpp
  src/shell/av_soak.cpp
  src/shell/av_soak.h
  src/shell/chrome_host.cpp
  src/shell/chrome_host.h
  src/shell/command_table.cpp
  src/shell/commands.h
  src/shell/key_router.cpp
  src/shell/key_router.h
  src/shell/file_jobs.cpp
  src/shell/file_jobs.h
  src/shell/marks.h
  src/shell/meta_store.cpp
  src/shell/meta_store.h
  src/shell/meta_writer.cpp
  src/shell/meta_writer.h
  src/shell/edit_session.cpp
  src/shell/edit_session.h
  src/shell/edit_view.h
  src/shell/adjust_pane.cpp
  src/shell/adjust_pane.h
  src/shell/trim_state.cpp
  src/shell/trim_state.h
  src/shell/open_request.cpp
  src/shell/open_request.h
  src/shell/slideshow.h
  src/shell/navigation.h
  src/shell/settings.cpp
  src/shell/settings.h
  src/shell/settings_store.cpp
  src/shell/settings_store.h
  src/shell/welcome_screen.h
  src/shell/update_guard.cpp
  src/shell/update_guard.h
  src/shell/telemetry.cpp
  src/shell/telemetry.h
  src/shell/crash_reporter_win.cpp
  src/shell/crash_reporter_win.h
  src/shell/minidump_scrub.cpp
  src/shell/minidump_scrub.h
  src/shell/input_state.h
  src/shell/present_lab.h
  src/shell/app.manifest
  src/shell/app_icon.h
  src/shell/mediaviewer.rc
)
# PR 8 identity: icon resource + VERSIONINFO, numbers from project(VERSION).
set_source_files_properties(src/shell/mediaviewer.rc PROPERTIES
  COMPILE_DEFINITIONS "MV_VER_MAJOR=${PROJECT_VERSION_MAJOR};MV_VER_MINOR=${PROJECT_VERSION_MINOR};MV_VER_PATCH=${PROJECT_VERSION_PATCH};MV_APP_VERSION_STR=\"${PROJECT_VERSION}\";MV_ICON_FILE=\"${CMAKE_SOURCE_DIR}/assets/icon/mediaviewer.ico\""
  OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/icon/mediaviewer.ico")
target_link_libraries(mediaviewer_lab PRIVATE
  mv_player
  mv_meta
  mv_edit
  mv_clip
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
  comctl32  # LoadIconWithScaleDown (PR 8 window icon)
  crashpad::crashpad
)
target_compile_definitions(mediaviewer_lab PRIVATE MV_APP_VERSION="${PROJECT_VERSION}")
add_dependencies(mediaviewer_lab MediaViewerClipJob)
mv_asan_runtime_beside(mediaviewer_lab)
# The handler is a separate program (Apache-2.0), not linked. z.dll is its zlib.
set(MV_CRASHPAD_TOOLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad")
if(EXISTS "${MV_CRASHPAD_TOOLS}/crashpad_handler.exe")
  add_custom_command(TARGET mediaviewer_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_CRASHPAD_TOOLS}/crashpad_handler.exe"
            "$<TARGET_FILE_DIR:mediaviewer_lab>/crashpad_handler.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_CRASHPAD_TOOLS}/z.dll"
            "$<TARGET_FILE_DIR:mediaviewer_lab>/z.dll"
    COMMENT "Copy crashpad_handler.exe beside mediaviewer_lab")
else()
  message(WARNING "crashpad_handler.exe not found under ${MV_CRASHPAD_TOOLS}; "
                  "the lab will run without crash reporting")
endif()

# tools/minidump-scrub -- the app's privacy scrub, standalone (PR 7 verify).
add_executable(mv_minidump_scrub
  tools/minidump-scrub/main.cpp
  src/shell/minidump_scrub.cpp
  src/shell/minidump_scrub.h
)
target_link_libraries(mv_minidump_scrub PRIVATE mv_project_options)
target_include_directories(mv_minidump_scrub PRIVATE src)
mv_asan_runtime_beside(mv_minidump_scrub)
target_include_directories(mediaviewer_lab PRIVATE src src/abi/include)
add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")
# PR 8 About: the mark, the licence and the third-party notices ship beside the
# exe so the flyout opens local files, never a network fetch.
add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/icon/mediaviewer-256.png"
          "${CMAKE_SOURCE_DIR}/LICENSE"
          "${CMAKE_SOURCE_DIR}/THIRD-PARTY.md"
          "$<TARGET_FILE_DIR:mediaviewer_lab>"
  COMMENT "Copy About assets (icon PNG, LICENSE, THIRD-PARTY.md) beside mediaviewer_lab")

# PR 3: publish the C# WinUI chrome beside the lab so hostfxr can load it.
# The core still builds with no .NET; chrome is a lab-side host, not a back-edge.
find_program(DOTNET_EXE dotnet)
set(MV_CHROME_PROJ "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/MediaViewer.Chrome.csproj")
set(MV_CHROME_SOURCES
  ${MV_CHROME_PROJ}
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Filmstrip.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Video.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Commands.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Gallery.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Settings.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.DragDrop.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.About.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/ChromeApp.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/CrashCapture.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Update.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Telemetry.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Addons.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Interop/Addons.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/MediaViewer.Updater.csproj"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/UpdateKeys.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/UpdateManifest.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/SignedManifestSource.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/TrialState.cs"
  "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater/UpdateService.cs"
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

  # Milestone G: the Import add-on's chrome goes beside mv_import.dll in
  # build/addons/import, never beside the app (tools/package/addon-pack.py
  # packs that folder, signed). The base install tree stays byte-identical.
  add_custom_target(mv_import_chrome ALL
    COMMAND ${DOTNET_EXE} publish
            "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Import.Chrome/MediaViewer.Import.Chrome.csproj"
            --nologo --configuration $<IF:$<CONFIG:Debug>,Debug,Release>
            -r win-x64 --no-self-contained -p:Platform=x64
            -o "$<TARGET_FILE_DIR:mv_import>"
    DEPENDS mv_import
    COMMENT "Publishing MediaViewer.Import.Chrome beside mv_import.dll"
    VERBATIM)

  # PR 8 clean-VM install: the Windows App SDK ships IN the payload
  # (WindowsAppSDKSelfContained in MediaViewer.Chrome.csproj), because the
  # wizard is per-user and takes no UAC and so cannot install a machine-wide
  # framework package (plan/10 verify, plan/13).
  #
  # Self-contained WinUI activates its WinRT classes registration-free, and
  # registration-free activation reads the manifest of the EXECUTABLE. Our
  # executable is this native host, not the C# project the SDK generated the
  # manifest for, so without this step every XAML activation fails with
  # 0x80040111 CLASS_E_CLASSNOTAVAILABLE - the island silently does not attach
  # and the command bar is missing. Measured.
  #
  # So: merge the SDK's generated activation manifest (980 classes, and it
  # tracks the WindowsAppSDK package version - never hand-maintained) into the
  # host's own manifest, after the publish that produces it.
  find_program(MV_MT_EXE mt
    HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin"
    PATH_SUFFIXES "10.0.26100.0/x64" "10.0.22621.0/x64" "x64"
    DOC "Windows SDK manifest tool, for the self-contained WinUI activation manifest")
  set(MV_WINAPPSDK_MANIFEST
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/obj/x64/$<IF:$<CONFIG:Debug>,Debug,Release>/net8.0-windows10.0.19041.0/win-x64/Manifests/WindowsAppSDK.manifest")
  if(MV_MT_EXE)
    add_custom_command(TARGET mediaviewer_lab POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E echo
              "Merging the Windows App SDK activation manifest into mediaviewer_lab.exe"
      COMMAND "${MV_MT_EXE}" -nologo
              -manifest "${CMAKE_SOURCE_DIR}/src/shell/app.manifest" "${MV_WINAPPSDK_MANIFEST}"
              "-outputresource:$<TARGET_FILE:mediaviewer_lab>$<SEMICOLON>#1"
      COMMENT "Self-contained WinUI activation manifest -> host exe"
      VERBATIM)
  else()
    message(WARNING
      "mt.exe not found: the Windows App SDK activation manifest cannot be merged "
      "into mediaviewer_lab.exe, so the WinUI island will fail to attach with "
      "CLASS_E_CLASSNOTAVAILABLE. Install the Windows SDK, or set MV_MT_EXE.")
  endif()
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
# `tools` so --no-pop shares tools/frametime/report.h with the unit tests
# instead of growing a second copy of the gate.
target_include_directories(mv_frametime PRIVATE src tools)
set_target_properties(mv_frametime PROPERTIES OUTPUT_NAME "frametime")
mv_asan_runtime_beside(mv_frametime)
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
    tests/crt_no_dialogs.cpp
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
    tests/test_tiles.cpp
    tests/test_refinement.cpp
    tests/test_chrome_host.cpp
    tests/test_dir.cpp
    tests/test_dir_tree.cpp
    tests/test_browse_path.cpp
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
    # PR 9 (plan/12 2026-09-24)
    tests/test_meta.cpp
    tests/test_meta_store.cpp
    # PR 12: the metadata writer, and the host-shared write queue.
    tests/test_meta_write.cpp
    tests/test_meta_writer_queue.cpp
    src/shell/meta_store.cpp
    src/shell/meta_writer.cpp
    # PR 10: edit stack, lossless JPEG, export, and the hosts' edit session.
    tests/test_edit.cpp
    tests/test_edit_session.cpp
    tests/test_export_carried.cpp
    src/shell/edit_session.cpp
    # PR 11: colour adjusts, FP16 working space, bake, histogram, the pane state.
    tests/test_adjust.cpp
    src/shell/adjust_pane.cpp
    # PR 13 / 14: clip core on synthetic clips, the ABI's clip session, trim mode.
    tests/test_clip.cpp
    tests/test_clip_helper.cpp
    tests/test_clip_session.cpp
    tests/test_trim_state.cpp
    src/abi/clip_session.cpp
    src/shell/trim_state.cpp
    tests/test_corpus.cpp
    tests/test_key_router.cpp
    tests/test_key_router_review.cpp
    tests/test_collision_name.cpp
    tests/test_file_ops.cpp
    tests/test_file_jobs.cpp
    tests/test_settings_store.cpp
    tests/test_update_guard.cpp
    tests/test_telemetry.cpp
    tests/test_folder_reselect.cpp
    tests/test_marks.cpp
    tests/test_open_request.cpp
    tests/test_slideshow.cpp
    tests/test_anim.cpp
    tests/test_gif_webp.cpp
    tests/test_pairing.cpp
    tests/test_live_photo.cpp
    tests/test_raw.cpp
    tests/test_heif_avif.cpp
    tests/test_animation_session.cpp
    tests/test_navigation.cpp
    tests/test_present_policy.cpp
    tests/test_metal_pacer.cpp
    tests/test_frametime_report.cpp
    tests/test_minidump_scrub.cpp
    tests/test_dino_game.cpp
    src/shell/minidump_scrub.cpp
    tests/test_tiff_ico.cpp
    src/shell/chrome_host.cpp
    src/shell/command_table.cpp
    src/shell/key_router.cpp
    src/shell/file_jobs.cpp
    src/shell/open_request.cpp
    src/shell/settings.cpp
    src/shell/settings_store.cpp
    src/shell/update_guard.cpp
    src/shell/telemetry.cpp
    src/shell/app.manifest
  )
  # PR 8 verify: update signature rejection (managed half of the updater).
  if(DOTNET_EXE)
    # No --nologo: `dotnet run` forwards it to the program, which answers with
    # its usage and exit code 2 instead of running the suite.
    add_test(NAME updater_manifest_verification
      COMMAND ${DOTNET_EXE} run --project
              "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Updater.Tests/MediaViewer.Updater.Tests.csproj"
              -c Release)
    set_tests_properties(updater_manifest_verification PROPERTIES TIMEOUT 300 LABELS updater)
  endif()
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_gfx
    mv_player
    mv_codec
    mv_image
    mv_canvas
    mv_meta
    mv_edit
    mv_clip
    Exiv2::exiv2lib
    ${FFMPEG_LIBRARIES}
    mediaviewer_core
    JPEG::JPEG
    ${MV_SPNG_TARGET}
    GIF::GIF
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux  # tests only: WebPAnimEncoder builds animated fixtures
    TIFF::TIFF        # tests only: in-memory TIFF fixtures (tests/fixtures_tiff_ico.h)
    lcms2::lcms2
    Catch2::Catch2WithMain
    ole32
    uuid
    mv_io)
  target_include_directories(mv_tests PRIVATE src src/abi/include tools)
  # The clip helper with its test hooks (a crash, a hang, a software encoder);
  # the product MediaViewerClipJob.exe is built without them.
  add_executable(mv_clipjob_test tools/clipjob/main.cpp)
  target_link_libraries(mv_clipjob_test PRIVATE mv_clip mv_project_options)
  target_include_directories(mv_clipjob_test PRIVATE src)
  # The hooks reach clip::detail through clip_internal.h, which includes FFmpeg;
  # mv_clip keeps those headers PRIVATE.
  target_include_directories(mv_clipjob_test SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_compile_definitions(mv_clipjob_test PRIVATE MV_CLIPJOB_TEST_HOOKS=1)
  mv_asan_runtime_beside(mv_clipjob_test)
  target_compile_definitions(mv_tests PRIVATE MV_CLIPJOB_PATH="$<TARGET_FILE:mv_clipjob_test>")
  add_dependencies(mv_tests mv_clipjob_test)
  # Before catch_discover_tests below: discovery runs this exe, and it must not
  # have to borrow MSBuild's PATH to start.
  mv_asan_runtime_beside(mv_tests)

  # Portable PR 16 arithmetic. Does not link mediaviewer_core.dll, so it can
  # build while another session holds that file.
  add_executable(mv_pr16_tests
    tests/crt_no_dialogs.cpp
    tests/test_present_policy.cpp
    tests/test_metal_pacer.cpp
    tests/test_frametime_report.cpp
  )
  target_link_libraries(mv_pr16_tests PRIVATE mv_gfx Catch2::Catch2WithMain)
  target_include_directories(mv_pr16_tests PRIVATE src tools)
  mv_asan_runtime_beside(mv_pr16_tests)

  include(Catch)
  # A Catch2 SKIP exits 0, so ctest counted every corpus-gated video test as
  # Passed: a clean clone reported "100% tests passed" with twelve cases —
  # hardware decode, colour, seek, frame step, texture leaks, the video ABI —
  # silently absent. Matching Catch2's own SKIPPED marker makes ctest report
  # them as Skipped instead, so the summary says how much was not proved.
  # tests/corpus.h has the rest: a loud report, and MV_REQUIRE_CORPUS=1 to turn
  # a missing clip into a failure in CI.
  # A per-test timeout, so a test that wedges is named by ctest instead of
  # taking the whole step down with it fifteen minutes later and the log
  # ending mid-sentence. These cases run in well under a second; the corpus
  # sweep sets its own below. This belongs to the suite, not to one CI file:
  # a hang is a hang whoever runs it.
  set(MV_TEST_TIMEOUT 300)
  if(MV_ASAN)
    # FFmpeg's D3D11VA hardware-decoder integration can deadlock inside the
    # Windows driver when the executable is ASan-instrumented. Exclude every
    # video-tagged test here, not only cases carrying both [video] and [ring].
    # Normal MSVC and clang-cl jobs still run the complete video suite.
    catch_discover_tests(mv_tests TEST_SPEC "~[video]"
      PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT ${MV_TEST_TIMEOUT})
  else()
    catch_discover_tests(mv_tests
      PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT ${MV_TEST_TIMEOUT})
  endif()
  catch_discover_tests(mv_pr16_tests TEST_PREFIX "pr16_"
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT ${MV_TEST_TIMEOUT})

  # Milestone G (plan/18): verified copy, add-on manifests / store / loader,
  # and the Import engine end to end. The same sources run on Linux CI
  # (cmake/portable) and on the Mac (cmake/darwin.cmake).
  add_executable(mv_import_tests
    tests/test_content_hash.cpp
    tests/test_verified_copy.cpp
    tests/test_addon_manifest.cpp
    tests/test_import_naming.cpp
    tests/test_import_engine.cpp
    tests/test_json.cpp
  )
  target_link_libraries(mv_import_tests PRIVATE mv_io mv_addon mv_import_engine
    unofficial-sodium::sodium Catch2::Catch2WithMain)
  target_include_directories(mv_import_tests PRIVATE src tests)
  target_compile_definitions(mv_import_tests PRIVATE
    MV_IMPORT_MODULE_PATH="$<TARGET_FILE:mv_import>")
  add_dependencies(mv_import_tests mv_import)
  mv_asan_runtime_beside(mv_import_tests)
  catch_discover_tests(mv_import_tests TEST_PREFIX "import_"
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT ${MV_TEST_TIMEOUT}
               ENVIRONMENT "TZ=UTC")
  if(TARGET mv_chrome)
    add_dependencies(mv_tests mv_chrome)
  endif()

  add_executable(mv_frametime_fixture tests/frametime_fixture.cpp)
  add_executable(mv_swapchain_recovery tests/swapchain_recovery.cpp)
  target_link_libraries(mv_swapchain_recovery PRIVATE mv_gfx)
  target_link_libraries(mv_frametime_fixture PRIVATE mv_project_options)
  mv_asan_runtime_beside(mv_frametime_fixture)
  mv_asan_runtime_beside(mv_swapchain_recovery)
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

# ---------------------------------------------------------------------------
# PR 7 — broken-file corpus (plan/09, plan/10 PR 7 verify: "nothing in the
# broken corpus crashes or hangs"). Its own executable so the Job Object
# memory cap and crash attribution in tests/test_broken_corpus.cpp apply to
# nothing else. Seeds live in tests/data (committed, synthetic, licence-clean).
# ---------------------------------------------------------------------------
if(MV_BUILD_TESTS AND NOT MV_FUZZ)
  add_executable(mv_broken_tests tests/test_broken_corpus.cpp tests/crt_no_dialogs.cpp)
  target_link_libraries(mv_broken_tests PRIVATE mv_codec mv_image Catch2::Catch2WithMain psapi)
  target_include_directories(mv_broken_tests PRIVATE src)
  mv_asan_runtime_beside(mv_broken_tests)
  target_compile_definitions(mv_broken_tests PRIVATE
    MV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  catch_discover_tests(mv_broken_tests TEST_PREFIX "broken_"
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 1800 LABELS broken)

  # PR 7 verify, "iPhone HEIC opens on a clean VM with no Store packs". Also its
  # own executable, and for a stronger reason than the broken corpus: the test
  # asserts that NO Media Foundation / WIC-extension / Store module is mapped
  # into the process while a HEIC decodes through libheif. Inside mv_tests any
  # earlier video case would have loaded mfplat.dll and the assertion could
  # never be made again. See tests/test_clean_vm_heic.cpp.
  add_executable(mv_clean_vm_tests tests/test_clean_vm_heic.cpp tests/crt_no_dialogs.cpp)
  target_link_libraries(mv_clean_vm_tests PRIVATE mv_codec mv_image Catch2::Catch2WithMain psapi)
  target_include_directories(mv_clean_vm_tests PRIVATE src)
  target_compile_definitions(mv_clean_vm_tests PRIVATE
    MV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    MV_TESTMEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools/testmedia")
  catch_discover_tests(mv_clean_vm_tests TEST_PREFIX "cleanvm_"
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 120 LABELS cleanvm)
endif()

# ---------------------------------------------------------------------------
# PR 7 — libFuzzer harnesses, one per decoder entry point (tools/fuzz). Needs
# clang-cl: cmake -T ClangCL -DMV_FUZZ=ON -DMV_BUILD_TESTS=OFF, then build the
# `mv_fuzzers` target only (the rest of the tree is instrumented too, but not
# linked against the sanitizer runtime). tools/fuzz/run.ps1 runs them.
# ---------------------------------------------------------------------------
option(MV_FUZZ "Build libFuzzer harnesses (clang-cl only)" OFF)
if(MV_FUZZ)
  add_subdirectory(tools/fuzz)
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — licence GPL-2.0-or-later (plan/11)")
