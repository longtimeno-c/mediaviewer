# SPDX-License-Identifier: GPL-2.0-or-later
#
# Darwin / Apple Silicon present lab (PR 16). Included from the root
# CMakeLists.txt and then returns, so none of the Windows targets are defined.
#
# plan/15: AppKit + CAMetalLayer + CAMetalDisplayLink. No SwiftUI, no
# VideoToolbox, no FFmpeg on this slice.

enable_language(OBJCXX)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  message(FATAL_ERROR
    "MediaViewer on macOS is Apple Silicon only (plan/15, D9). "
    "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-rtti>
  $<$<COMPILE_LANGUAGE:OBJCXX>:-Wall -Wextra -Werror -fno-rtti -fobjc-arc>
)
target_compile_definitions(mv_project_options INTERFACE MV_DARWIN=1)

add_library(mv_core STATIC
  src/core/job_system_posix.mm
  src/core/trace_posix.cpp
  src/core/job_system.h
  src/core/result.h
  src/core/spsc_ring.h
  src/core/status.h
  src/core/trace.h
)
target_include_directories(mv_core PUBLIC src)
target_link_libraries(mv_core PUBLIC mv_project_options)
# job_system_posix.mm wraps each job body in @autoreleasepool (a job may call
# Metal/AppKit APIs that autorelease temporaries) — needs the ObjC runtime.
target_link_libraries(mv_core PRIVATE "-framework Foundation")
add_library(mv::core ALIAS mv_core)

add_library(mv_gfx STATIC
  src/gfx/metal_pacer.cpp
  src/gfx/device_mac.mm
  src/gfx/metal_layer.mm
  src/gfx/blit_metal.mm
  src/gfx/metal_pacer.h
  src/gfx/present_policy.h
  src/gfx/pace_json.h
  src/gfx/device_mac.h
  src/gfx/metal_layer.h
  src/gfx/blit_metal.h
)
target_link_libraries(mv_gfx PUBLIC mv_core)
target_link_libraries(mv_gfx PRIVATE
  "-framework Foundation"
  "-framework AppKit"
  "-framework Metal"
  "-framework QuartzCore")
add_library(mv::gfx ALIAS mv_gfx)

# ---------------------------------------------------------------------------
# mv_codec — PR 17's format set: JPEG/PNG/BMP only (PR 2 parity). Not
# codec/decode.cpp or codec/os_decode_win.cpp: those pull in GIF/WebP/TIFF/
# HEIC/AVIF/RAW decoders and the Windows OS-codec probe, none of which are
# built on Darwin yet (plan/15 PR 17 scope).
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
  src/codec/jpeg.cpp
  src/codec/png.cpp
  src/codec/bmp.cpp
  src/codec/format.h
  src/codec/raster.h
  src/codec/decode.h
)
target_link_libraries(mv_codec PUBLIC mv_core PRIVATE JPEG::JPEG ${MV_SPNG_TARGET})
add_library(mv::codec ALIAS mv_codec)

# ---------------------------------------------------------------------------
# mv_io — portable io/dir.h + io/file.h (PR 18, folded-in PR 4 — plan/12
# 2026-09-17). dir_mac.cpp uses FSEvents for the watch; dir_win.cpp/file.cpp
# stay Windows-only.
# ---------------------------------------------------------------------------
add_library(mv_io STATIC
  src/io/dir_mac.cpp
  src/io/file_mac.cpp
  src/io/dir.h
  src/io/file.h
)
target_link_libraries(mv_io PUBLIC mv_core PRIVATE "-framework CoreServices")
add_library(mv::io ALIAS mv_io)

# ---------------------------------------------------------------------------
# mv_image — colour (LCMS) + PR 17's decode entrypoint + Metal upload +
# PR 18's JPEG-512 thumbnail cache (folded-in PR 4). image/pipeline.cpp/
# upload.cpp/gpu_image.h/thumb.cpp stay Windows-only (D3D11 or the '\\'
# path-join bug in thumb.cpp); PR 17/18 use the _mac twins instead
# (plan/12 2026-09-17).
# ---------------------------------------------------------------------------
find_package(lcms2 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(mv_image STATIC
  src/image/colour.cpp
  src/image/pipeline_mac.cpp
  src/image/upload_mac.mm
  src/image/thumb_mac.cpp
  src/image/colour.h
  src/image/pipeline_mac.h
  src/image/upload_mac.h
  src/image/gpu_image_mac.h
  src/image/thumb.h
)
target_link_libraries(mv_image PUBLIC mv_codec mv_gfx mv_io
  PRIVATE lcms2::lcms2 unofficial::sqlite3::sqlite3)
add_library(mv::image ALIAS mv_image)

# ---------------------------------------------------------------------------
# mv_canvas — pan/zoom camera + springs (plan/03: omega=18, zeta=1). Zero
# platform dependency; identical to the Windows target's source.
# ---------------------------------------------------------------------------
add_library(mv_canvas STATIC
  src/canvas/camera.cpp
  src/canvas/camera.h
  src/canvas/spring.h
)
target_link_libraries(mv_canvas PUBLIC mv_core)
add_library(mv::canvas ALIAS mv_canvas)

# ---------------------------------------------------------------------------
# mv_shell — browse_index, the wrapping next/prev/first/last/skip arithmetic
# behind PR 18's folded-in PR 4/6 navigation (plan/16-commands.md's Browse
# table). Zero platform dependency on purpose, so it is unit-testable without
# the AppKit glue that drives it (main_mac.mm).
# ---------------------------------------------------------------------------
add_library(mv_shell STATIC
  src/shell/browse_index.cpp
  src/shell/browse_index.h
)
target_link_libraries(mv_shell PUBLIC mv_core)
add_library(mv::shell ALIAS mv_shell)

find_package(imgui CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# MediaViewerChrome — PR 18's SwiftUI command bar, hosted in the AppKit
# window (plan/10, plan/15: "the canvas is not ported to SwiftUI"; command
# bar and window chrome only). Its own SwiftPM package
# (src.swift/MediaViewerChrome), built via `swift build` rather than folded
# into this file's C++ target graph — CMake only invokes it and links the
# result. `-emit-objc-header-path` produces MediaViewerChrome-Swift.h, which
# src/shell/main_mac.mm imports for `MVChromeHost`. The bridge runs the
# other direction too: mv_chrome_fit()/mv_chrome_one_to_one() (the C
# symbols CommandBarView.swift calls) are defined in main_mac.mm itself and
# resolved at mediaviewer_lab's own final link — MediaViewerChrome does not
# link against mediaviewer_lab, only the reverse.
#
# Confirmed on a real toolchain (2026-09-17, Swift 6.3.3/arm64-apple-macosx):
# `swift build -c release` on this Swift version uses the Xcode-style "Swift
# Build" backend, not classic SwiftPM — it ignores an absolute
# `-emit-objc-header-path` and instead writes the umbrella ObjC header to
# `<build-path>/out/Intermediates.noindex/GeneratedModuleMaps/*-Swift.h` and
# the static lib to `<build-path>/out/Products/Release/*.a`. That layout is
# this toolchain's, not a stable contract — the wrapper script below finds
# the real output and copies it to the flat path the rest of this file (and
# main_mac.mm's `#import`) expects, so a different Swift toolchain's layout
# doesn't silently break the import path again.
set(MV_SWIFT_CHROME_DIR "${CMAKE_SOURCE_DIR}/src.swift/MediaViewerChrome")
set(MV_SWIFT_CHROME_BUILD_DIR "${CMAKE_BINARY_DIR}/swift-chrome")
set(MV_SWIFT_CHROME_HEADER "${MV_SWIFT_CHROME_BUILD_DIR}/MediaViewerChrome-Swift.h")
set(MV_SWIFT_CHROME_LIB "${MV_SWIFT_CHROME_BUILD_DIR}/release/libMediaViewerChrome.a")
set(MV_SWIFT_CHROME_COLLECT "${CMAKE_SOURCE_DIR}/cmake/collect-swift-chrome.sh")

add_custom_command(
  OUTPUT "${MV_SWIFT_CHROME_LIB}" "${MV_SWIFT_CHROME_HEADER}"
  COMMAND swift build -c release
          --package-path "${MV_SWIFT_CHROME_DIR}"
          --build-path "${MV_SWIFT_CHROME_BUILD_DIR}"
          -Xswiftc -emit-objc-header-path -Xswiftc "${MV_SWIFT_CHROME_HEADER}"
  COMMAND /bin/sh "${MV_SWIFT_CHROME_COLLECT}"
          "${MV_SWIFT_CHROME_BUILD_DIR}" "${MV_SWIFT_CHROME_HEADER}" "${MV_SWIFT_CHROME_LIB}"
  DEPENDS
    "${MV_SWIFT_CHROME_DIR}/Package.swift"
    "${MV_SWIFT_CHROME_DIR}/Sources/MediaViewerChrome/CommandBarView.swift"
    "${MV_SWIFT_CHROME_DIR}/Sources/MediaViewerChrome/ChromeHost.swift"
    "${MV_SWIFT_CHROME_DIR}/Sources/MVChromeBridge/include/mv_chrome_bridge.h"
    "${MV_SWIFT_CHROME_COLLECT}"
  COMMENT "swift build: MediaViewerChrome (PR 18 command bar)"
  VERBATIM)
add_custom_target(mv_swift_chrome_build
  DEPENDS "${MV_SWIFT_CHROME_LIB}" "${MV_SWIFT_CHROME_HEADER}")

add_executable(mediaviewer_lab
  src/shell/main_mac.mm
  src/shell/present_lab_mac.mm
  src/shell/present_lab_mac.h
  src/shell/input_state.h
  src/shell/folder_model_mac.cpp
  src/shell/folder_model_mac.h
)
add_dependencies(mediaviewer_lab mv_swift_chrome_build)
target_link_libraries(mediaviewer_lab PRIVATE
  mv_gfx
  mv_image
  mv_canvas
  mv_shell
  mv_io
  imgui::imgui
  "${MV_SWIFT_CHROME_LIB}"
  "-framework Foundation"
  "-framework AppKit"
  "-framework Metal"
  "-framework QuartzCore"
  "-framework CoreServices"
  "-framework SwiftUI"
  "-framework Combine")
target_include_directories(mediaviewer_lab PRIVATE src "${MV_SWIFT_CHROME_BUILD_DIR}")
set_source_files_properties(
  src/shell/main_mac.mm
  src/shell/present_lab_mac.mm
  PROPERTIES COMPILE_FLAGS "-fobjc-arc")

add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")

add_executable(mv_frametime
  tools/frametime/main_mac.cpp
)
target_link_libraries(mv_frametime PRIVATE mv_core)
target_include_directories(mv_frametime PRIVATE src tools)
set_target_properties(mv_frametime PROPERTIES OUTPUT_NAME "frametime")
add_dependencies(mv_frametime mediaviewer_lab)

if(MV_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(mv_tests
    tests/test_result.cpp
    tests/test_spsc_ring.cpp
    tests/test_job_system.cpp
    tests/test_input_state.cpp
    tests/test_present_policy.cpp
    tests/test_metal_pacer.cpp
    tests/test_frametime_report.cpp
    tests/test_browse_index.cpp
  )
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_gfx
    mv_shell
    Catch2::Catch2WithMain)
  target_include_directories(mv_tests PRIVATE src tools)
  include(Catch)
  catch_discover_tests(mv_tests)
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — Darwin present lab (PR 16), GPL-2.0-or-later")
