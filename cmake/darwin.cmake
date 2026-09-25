# SPDX-License-Identifier: GPL-2.0-or-later
#
# Darwin / Apple Silicon host (PR 16–20). Included from the root
# CMakeLists.txt and then returns, so none of the Windows targets are defined.
# MediaViewer.app, the Quick Look extension, and Sparkle are in darwin-app.cmake.
#
# plan/15: AppKit + CAMetalLayer + CAMetalDisplayLink. No SwiftUI, no
# VideoToolbox, no FFmpeg on this slice.

enable_language(OBJCXX)

# Apple Silicon and Intel (D9 amended 2026-09-24). The release ships one
# universal app: each arch is built natively, then tools/mac/lipo_merge.py
# joins the two .app trees before signing.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(MV_MAC_VCPKG_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(MV_MAC_VCPKG_ARCH "x64")
else()
  message(FATAL_ERROR
    "MediaViewer on macOS needs arm64 or x86_64 (plan/15, D9). "
    "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(MV_MAC_TRIPLET "${MV_MAC_VCPKG_ARCH}-osx")
set(MV_MAC_DYNAMIC_TRIPLET "${MV_MAC_VCPKG_ARCH}-osx-dynamic")

# vcpkg builds the dependencies for the same target: tools/vcpkg/triplets/*-osx.cmake
# and tools/mac/triplets/openmp-dynamic.cmake. Change all three together.
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
# job_system_posix.mm wraps each job body in @autoreleasepool (a job may call
# Metal/AppKit APIs that autorelease temporaries) — needs the ObjC runtime.
target_link_libraries(mv_core PRIVATE "-framework Foundation")
add_library(mv::core ALIAS mv_core)

add_library(mv_gfx STATIC
  src/gfx/adjust_kernel.h
  src/gfx/colour_desc.cpp
  src/gfx/colour_desc.h
  src/gfx/metal_pacer.cpp
  src/gfx/device_mac.mm
  src/gfx/metal_layer.mm
  src/gfx/blit_metal.mm
  src/gfx/video_blit_metal.mm
  src/gfx/video_blit_metal.h
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
# mv_codec — the D5 still set, same TUs as the Windows target (plan/12
# 2026-09-17 folded the PR 7 camera-dump formats into PR 17): JPEG, PNG, BMP,
# GIF, WebP, TIFF, ICO, HEIC/HEIF, AVIF, RAW, APNG. Only the OS-codec hook
# differs (os_decode_mac.cpp).
#
# Linkage (CLAUDE.md "Licensing", plan/11): libheif, libde265 and LibRaw are
# dynamic-link only, so they come from vcpkg's arm64-osx-dynamic triplet;
# giflib, libwebp, libtiff and libavif+dav1d are permissive and stay static
# in the default arm64-osx triplet, installed from the root manifest.
# Install tools/mac/dependencies/vcpkg.json separately with arm64-osx-dynamic
# for the LGPL codecs (including FFmpeg); see RELEASING.md for the commands.
# libheif's default features are OFF for the same reason as on Windows: the
# port's `hevc` feature is x265 encode, which is forbidden.
# ---------------------------------------------------------------------------
if(DEFINED _VCPKG_INSTALLED_DIR)
  set(MV_DYNAMIC_PREFIX_DEFAULT "${_VCPKG_INSTALLED_DIR}/${MV_MAC_DYNAMIC_TRIPLET}")
else()
  set(MV_DYNAMIC_PREFIX_DEFAULT "")
endif()
set(MV_VCPKG_DYNAMIC_PREFIX "${MV_DYNAMIC_PREFIX_DEFAULT}" CACHE PATH
  "vcpkg ${MV_MAC_DYNAMIC_TRIPLET} install prefix (libheif, libde265, LibRaw, FFmpeg)")
if(NOT MV_VCPKG_DYNAMIC_PREFIX OR NOT IS_DIRECTORY "${MV_VCPKG_DYNAMIC_PREFIX}")
  message(FATAL_ERROR
    "${MV_MAC_DYNAMIC_TRIPLET} prefix not found (${MV_VCPKG_DYNAMIC_PREFIX}); install "
    "tools/mac/dependencies/vcpkg.json with triplet ${MV_MAC_DYNAMIC_TRIPLET} into a separate "
    "install root, then set MV_VCPKG_DYNAMIC_PREFIX to its ${MV_MAC_DYNAMIC_TRIPLET} directory. "
    "See RELEASING.md (macOS dependencies).")
endif()
list(APPEND CMAKE_PREFIX_PATH "${MV_VCPKG_DYNAMIC_PREFIX}")

find_package(JPEG REQUIRED)
find_package(spng CONFIG REQUIRED)
if(TARGET spng::spng)
  set(MV_SPNG_TARGET spng::spng)
elseif(TARGET spng::spng_static)
  set(MV_SPNG_TARGET spng::spng_static)
else()
  message(FATAL_ERROR "libspng imported target not found")
endif()
find_package(GIF REQUIRED)
find_package(WebP CONFIG REQUIRED)
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
# LibRaw's thread-safe raw_r target: decode runs on the job pool.
include(${CMAKE_CURRENT_LIST_DIR}/raw-openmp.cmake)
find_package(libraw CONFIG REQUIRED)

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
  src/codec/os_decode_mac.cpp
  src/codec/os_decode.h
  src/codec/crash_test_hook.cpp
  src/codec/crash_test_hook.h
  src/codec/format.h
  src/codec/raster.h
  src/codec/decode.h
)
target_link_libraries(mv_codec
  PUBLIC mv_core
  PRIVATE JPEG::JPEG ${MV_SPNG_TARGET} GIF::GIF WebP::webp WebP::webpdemux
          TIFF::TIFF ${MV_HEIF_TARGET} ${MV_AVIF_TARGET} libraw::raw_r)
add_library(mv::codec ALIAS mv_codec)
target_link_libraries(mv_codec PRIVATE mv_raw_openmp)

# ---------------------------------------------------------------------------
# mv_io — portable io/dir.h + io/file.h (PR 18, folded-in PR 4 — plan/12
# 2026-09-17). dir_mac.cpp uses FSEvents for the watch; dir_win.cpp/file.cpp
# stay Windows-only.
# ---------------------------------------------------------------------------
add_library(mv_io STATIC
  src/io/dir_mac.cpp
  src/io/dir_tree.cpp
  src/io/file_mac.cpp
  src/io/replace_mac.cpp
  src/io/replace.h
  src/io/paths_mac.cpp
  src/io/dir.h
  src/io/file.h
  src/io/paths.h
  # Milestone G (plan/18): verified copy (also F8 across volumes) and the file
  # and volume ports Import reaches through the add-on host table.
  src/io/content_hash.cpp
  src/io/content_hash.h
  src/io/file_port.cpp
  src/io/file_port.h
  src/io/file_port_mac.cpp
  src/io/child_process.h
  src/io/child_process_posix.cpp
  src/io/verified_copy.cpp
  src/io/verified_copy.h
  src/io/volume.h
  src/io/volume_mac.cpp
)
find_package(blake3 CONFIG REQUIRED)
target_link_libraries(mv_io PUBLIC mv_core PRIVATE "-framework CoreServices"
  "-framework CoreFoundation" "-framework DiskArbitration" BLAKE3::blake3)
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
  src/image/linear.cpp
  src/image/linear.h
  src/image/half.h
  src/image/pipeline.cpp
  src/image/pipeline_mac.cpp
  src/image/upload_mac.mm
  src/image/thumb_mac.cpp
  src/image/colour.h
  src/image/pipeline.h
  src/image/pipeline_mac.h
  src/image/upload_mac.h
  src/image/gpu_image_mac.h
  src/image/thumb.h
)
target_link_libraries(mv_image PUBLIC mv_codec mv_gfx mv_io
  PRIVATE lcms2::lcms2 unofficial::sqlite3::sqlite3)
add_library(mv::image ALIAS mv_image)

# ---------------------------------------------------------------------------
# mv_player -- demux, decode, A/V clock, transport (PR 19, plan/05 + plan/15).
# The same portable TUs as the Windows target; the D9 host files are the Metal /
# VideoToolbox / Core Audio twins: hwdecode_mac.mm, frame_ring_mac.mm,
# audio_mac.cpp. FFmpeg is LGPL and dynamic-link only (CLAUDE.md), so it comes
# from the arm64-osx-dynamic triplet:
#   vcpkg install --triplet arm64-osx-dynamic \
#     "ffmpeg[core,avcodec,avformat,avfilter,swresample,swscale,dav1d,webp]"
# (add the `ffmpeg` feature if you want the CLI for tools/testmedia clips).
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${MV_VCPKG_DYNAMIC_PREFIX}/share/ffmpeg")
set(ENV{PKG_CONFIG_PATH}
    "${MV_VCPKG_DYNAMIC_PREFIX}/lib/pkgconfig:${MV_VCPKG_DYNAMIC_PREFIX}/debug/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
find_package(FFMPEG REQUIRED)

add_library(mv_player STATIC
  src/player/demux.cpp
  src/player/video_decode.cpp
  src/player/frame_ring.cpp
  src/player/frame_ring_mac.mm
  src/player/video_source.cpp
  src/player/media_source.cpp
  src/player/hwdecode_mac.mm       # D9: the only player/ file that names CoreVideo/Metal decode
  src/player/audio_decode.cpp
  src/player/av_clock.cpp
  src/player/audio_mac.cpp         # D9: the only player/ file that names Core Audio
  src/player/transport.cpp
  src/player/presenter.cpp
  src/player/container_probe.cpp
  src/player/poster.cpp
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
# SYSTEM so FFmpeg's own headers do not trip -Werror; ours stay fully checked.
target_include_directories(mv_player SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mv_player PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mv_player
  PUBLIC mv_core mv_gfx
  PRIVATE mv_codec mv_io ${FFMPEG_LIBRARIES}
          "-framework Foundation" "-framework Metal" "-framework CoreVideo"
          "-framework CoreMedia" "-framework VideoToolbox" "-framework AudioToolbox"
          "-framework CoreAudio" "-framework CoreFoundation")
add_library(mv::player ALIAS mv_player)

# ---------------------------------------------------------------------------
# mv_meta -- PR 9 metadata read model (plan/06): Exiv2 for EXIF / IPTC / XMP and
# maker notes, libavformat for container and per-stream facts. Exiv2 is GPL-2.0
# and dynamic-link only (CLAUDE.md "Licensing"), so it comes from the
# arm64-osx-dynamic triplet with FFmpeg. meta sits beside player/image in the
# module graph: it depends on codec + io + core and on neither of them.
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
# PR 11: colour adjusts, the one-source kernel both blits paste in
# (gfx/adjust_kernel.h), the
# full-resolution bake over the FP16 working image, the histogram.
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
# mv_clip -- PR 13 / 14 (plan/08): two-path trim, extract & remux, the clip
# job queue. The same sources as Windows; the encode port's Mac half lists
# VideoToolbox (hwencode_mac.cpp). FFmpeg dynamic and LGPL, as for mv_player.
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
  src/edit/hwencode_mac.cpp
)
target_include_directories(mv_clip SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mv_clip PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mv_clip
  PUBLIC mv_core mv_io
  PRIVATE mv_edit ${FFMPEG_LIBRARIES})
add_library(mv::clip ALIAS mv_clip)

# MediaViewerClipJob -- encode and decode jobs out of process (plan/12
# 2026-09-25). Beside mediaviewer_lab; macpack.py puts it in Contents/Helpers.
add_executable(MediaViewerClipJob tools/clipjob/main.cpp)
target_link_libraries(MediaViewerClipJob PRIVATE mv_clip)
target_include_directories(MediaViewerClipJob PRIVATE src)

# ---------------------------------------------------------------------------
# Milestone G: the add-on host (in the app) and the Import add-on
# (libmv_import.dylib, packed and signed separately; never in the app bundle).
# ---------------------------------------------------------------------------
find_package(unofficial-sodium CONFIG REQUIRED)
set(MV_SQLITE_TARGET unofficial::sqlite3::sqlite3)
set(MV_SODIUM_TARGET unofficial-sodium::sodium)
include("${CMAKE_CURRENT_LIST_DIR}/import.cmake")

# playprobe -- headless pipeline check (tools/playprobe): decoder actually used,
# presenter counters, drift slope. Not shipped.
add_executable(playprobe tools/playprobe/main_mac.mm)
target_link_libraries(playprobe PRIVATE mv_player mv_core "-framework Foundation" "-framework Metal")
target_include_directories(playprobe PRIVATE src)

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
  src/shell/browse_path.h
  # The one key router and command table, shared with Windows (plan/16): the
  # Mac host translates NSEvents to `key` at the edge, exactly as main.cpp
  # translates virtual keys. Pure C++, no platform header.
  src/shell/command_table.cpp
  src/shell/commands.h
  src/shell/key_router.cpp
  src/shell/key_router.h
  # PR 9: date-taken sort and the metadata cache (both portable).
  src/io/sort_order.cpp
  src/io/sort_order.h
  src/shell/meta_store.cpp
  src/shell/meta_store.h
  # PR 10: per-item edit stacks, crop mode, the lossless-write / export jobs.
  src/shell/edit_session.cpp
  src/shell/edit_session.h
  src/shell/edit_view.h
  # PR 11: the adjust pane's state (readiness, tokens, histogram), shared.
  src/shell/adjust_pane.cpp
  src/shell/adjust_pane.h
  # PR 13 / 14: trim mode and the clip tools (shared with Windows), and the
  # clip job queue + keyframe index behind the Windows ABI, reused as is.
  src/shell/trim_state.cpp
  src/shell/trim_state.h
  src/abi/clip_session.cpp
  src/abi/clip_session.h
  # PR 15: recent folders (Dock menu), copy-path text, shared with Windows.
  src/shell/os_integration.cpp
  src/shell/os_integration.h
)
target_include_directories(mv_shell PUBLIC src/abi/include)
target_link_libraries(mv_shell PUBLIC mv_core mv_io mv_meta mv_edit mv_addon mv_clip)
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
file(GLOB MV_SWIFT_CHROME_SOURCES CONFIGURE_DEPENDS
  "${MV_SWIFT_CHROME_DIR}/Sources/MediaViewerChrome/*.swift"
  "${MV_SWIFT_CHROME_DIR}/Sources/MVChromeBridge/include/*.h")

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
    ${MV_SWIFT_CHROME_SOURCES}
    "${MV_SWIFT_CHROME_COLLECT}"
  COMMENT "swift build: MediaViewerChrome (PR 18 command bar)"
  VERBATIM)
add_custom_target(mv_swift_chrome_build
  DEPENDS "${MV_SWIFT_CHROME_LIB}" "${MV_SWIFT_CHROME_HEADER}")

# The AppKit host, built twice from the same sources: mediaviewer_lab (the
# PR 16 instrument frametime drives, a bare binary) and MediaViewer (PR 20,
# the executable inside MediaViewer.app, with MV_APP_BUNDLE and, given a key,
# Sparkle).
# PR 11 (plan/10: Mac crash reporting, owed since old PR 17): Crashpad, out of
# process, with the Windows host's privacy scrub. Crashpad is Apache-2.0 and
# static, from the root manifest's arm64-osx install like the permissive codecs.
find_package(crashpad CONFIG REQUIRED)

set(MV_MAC_HOST_SOURCES
  src/shell/main_mac.mm
  src/shell/crash_reporter_mac.mm
  src/shell/crash_reporter_mac.h
  src/shell/minidump_scrub.cpp
  src/shell/minidump_scrub.h
  src/shell/present_lab_mac.mm
  src/shell/present_lab_mac.h
  src/shell/install_from_dmg_mac.mm
  src/shell/install_from_dmg_mac.h
  src/shell/media_kind.h
  src/shell/input_state.h
  src/shell/folder_model_mac.cpp
  src/shell/folder_model_mac.h
  # Milestone G: add-ons (Settings > Add-ons, the Import chrome's host side).
  src/shell/addons_mac.mm
  src/shell/addons_mac.h
  src/shell/present_busy.h
)
set_source_files_properties(
  src/shell/main_mac.mm
  src/shell/crash_reporter_mac.mm
  src/shell/present_lab_mac.mm
  src/shell/addons_mac.mm
  src/shell/install_from_dmg_mac.mm
  PROPERTIES COMPILE_FLAGS "-fobjc-arc")

function(mv_mac_host target)
  add_executable(${target} ${MV_MAC_HOST_SOURCES})
  add_dependencies(${target} mv_swift_chrome_build)
  target_link_libraries(${target} PRIVATE
    mv_gfx
    mv_image
    mv_canvas
    mv_shell
    mv_io
    mv_player
    imgui::imgui
    crashpad::crashpad
    "${MV_SWIFT_CHROME_LIB}"
    "-framework Foundation"
    "-framework AppKit"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework CoreServices"
    "-framework DiskArbitration"
    "-framework UniformTypeIdentifiers"
    "-framework MediaPlayer"  # PR 15: Now Playing / MPRemoteCommandCenter
    "-framework SwiftUI"
    "-framework Combine"
    # PR 11: Crashpad's macOS client (audit tokens, IOKit registry reads) and
    # the computer name for the scrub's identity list.
    "-framework SystemConfiguration"
    "-framework IOKit"
    "-framework Security"
    bsm)
  target_include_directories(${target} PRIVATE src "${MV_SWIFT_CHROME_BUILD_DIR}"
    "${MV_SWIFT_CHROME_DIR}/Sources/MVChromeBridge/include")
  # Same string Windows reads from VERSIONINFO (CMakeLists.txt project(VERSION)).
  target_compile_definitions(${target} PRIVATE MV_APP_VERSION="${PROJECT_VERSION}")
endfunction()

mv_mac_host(mediaviewer_lab)
add_dependencies(mediaviewer_lab MediaViewerClipJob)

# ---------------------------------------------------------------------------
# Milestone G: Import.bundle, the Import add-on's SwiftUI chrome. Built beside
# libmv_import.dylib in build/addons/import and packed (signed, notarized) by
# tools/package/addon-pack.py -- never copied into MediaViewer.app.
# ---------------------------------------------------------------------------
set(MV_IMPORT_CHROME_DIR "${CMAKE_SOURCE_DIR}/src.swift/ImportChrome")
set(MV_IMPORT_CHROME_BUILD "${CMAKE_BINARY_DIR}/swift-import-chrome")
set(MV_IMPORT_BUNDLE "${CMAKE_BINARY_DIR}/addons/import/Import.bundle")
file(GLOB MV_IMPORT_CHROME_SOURCES CONFIGURE_DEPENDS
  "${MV_IMPORT_CHROME_DIR}/Sources/ImportChrome/*.swift"
  "${MV_IMPORT_CHROME_DIR}/Sources/CImportApi/include/*.h")
add_custom_command(
  OUTPUT "${MV_IMPORT_BUNDLE}/Contents/MacOS/Import"
  COMMAND swift build -c release
          --package-path "${MV_IMPORT_CHROME_DIR}"
          --build-path "${MV_IMPORT_CHROME_BUILD}"
  COMMAND /bin/sh "${CMAKE_SOURCE_DIR}/cmake/make-import-bundle.sh"
          "${MV_IMPORT_CHROME_BUILD}" "${MV_IMPORT_BUNDLE}" "${MV_IMPORT_VERSION}"
  DEPENDS
    "${MV_IMPORT_CHROME_DIR}/Package.swift"
    ${MV_IMPORT_CHROME_SOURCES}
    "${CMAKE_SOURCE_DIR}/src/abi/include/mediaviewer/mediaviewer_import.h"
    "${CMAKE_SOURCE_DIR}/cmake/make-import-bundle.sh"
  COMMENT "swift build: Import.bundle (Milestone G add-on chrome)"
  VERBATIM)
add_custom_target(mv_import_chrome ALL DEPENDS "${MV_IMPORT_BUNDLE}/Contents/MacOS/Import" mv_import)

add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")

# PR 11: crashpad_handler runs out of process. Beside mediaviewer_lab for the
# lab; macpack.py puts it in MediaViewer.app/Contents/Helpers.
set(MV_CRASHPAD_HANDLER "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad/crashpad_handler")
if(EXISTS "${MV_CRASHPAD_HANDLER}")
  add_custom_command(TARGET mediaviewer_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_CRASHPAD_HANDLER}" "$<TARGET_FILE_DIR:mediaviewer_lab>/crashpad_handler"
    # vcpkg installs the handler without +x; crash_reporter_mac.mm requires X_OK.
    COMMAND chmod 755 "$<TARGET_FILE_DIR:mediaviewer_lab>/crashpad_handler"
    COMMENT "Copy crashpad_handler beside mediaviewer_lab")
else()
  message(WARNING "crashpad_handler not found at ${MV_CRASHPAD_HANDLER}; "
                  "the lab and MediaViewer.app will run without native crash reporting")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/darwin-app.cmake")

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
    tests/test_dino_game.cpp
    tests/test_browse_index.cpp
    tests/test_browse_path.cpp
    tests/test_dir_tree.cpp
    tests/test_key_router.cpp
    tests/test_key_router_review.cpp
    # The D5 still set (PR 17, folded-in PR 7): in-code fixtures, plus the
    # optional corpora which SKIP when absent (plan/09: no RAW in git).
    tests/test_probe.cpp
    tests/test_decode.cpp
    tests/test_tiff_ico.cpp
    tests/test_gif_webp.cpp
    tests/test_heif_avif.cpp
    tests/test_raw.cpp
    tests/test_anim.cpp
    tests/test_colour.cpp
    # PR 19: the portable player logic (clock, drift, presenter, transport, probe,
    # the audio-sink contract). test_video_ring / test_video_colour build D3D11
    # textures and test_video_transport goes through the Windows ABI header, so
    # they stay Windows-only; playprobe covers the Metal ring for real.
    tests/test_av_clock.cpp
    tests/test_audio_sink.cpp
    tests/test_presenter.cpp
    tests/test_transport.cpp
    tests/test_container_probe.cpp
    # PR 9: the metadata read model. Fixtures are built in the test.
    tests/test_meta.cpp
    tests/test_meta_store.cpp
    # PR 10: edit stack, lossless JPEG, export, the edit session.
    tests/test_edit.cpp
    tests/test_edit_session.cpp
    tests/test_export_carried.cpp
    # PR 11: colour adjusts, the FP16 working space, bake, histogram, pane state.
    tests/test_adjust.cpp
    # PR 11 (macOS crash reporting): the scrub, now with POSIX paths.
    tests/test_minidump_scrub.cpp
    src/shell/minidump_scrub.cpp
    # PR 13 / 14: the clip core on synthetic clips, the clip session, trim mode.
    tests/test_clip.cpp
    tests/test_clip_helper.cpp
    tests/test_clip_session.cpp
    tests/test_trim_state.cpp
    # PR 15: recent folders, copy path.
    tests/test_os_integration.cpp
    tests/test_thumb_pixels.cpp
    # PR 15: the Explorer handler's portable half (the COM DLL is Windows-only).
    tests/test_thumb_request.cpp
    src/shellext/thumb_request.cpp
    # PR 15: the Spotlight importer's field mapping.
    tests/test_spotlight_fields.cpp
    src/shell/spotlight_fields.cpp
  )
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_io
    mv_gfx
    mv_shell
    mv_edit
    mv_codec
    mv_image
    mv_player
    mv_meta
    mv_clip
    JPEG::JPEG
    ${MV_SPNG_TARGET}
    GIF::GIF
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux  # tests only: WebPAnimEncoder builds animated fixtures
    TIFF::TIFF        # tests only: in-memory TIFF fixtures (tests/fixtures_tiff_ico.h)
    ${MV_HEIF_TARGET}
    ${MV_AVIF_TARGET}
    libraw::raw_r
    lcms2::lcms2
    Catch2::Catch2WithMain)
  target_include_directories(mv_tests PRIVATE src tools tests src/abi/include)
  # The clip helper with its test hooks (a crash, a hang, a software encoder).
  add_executable(mv_clipjob_test tools/clipjob/main.cpp)
  target_link_libraries(mv_clipjob_test PRIVATE mv_clip)
  target_include_directories(mv_clipjob_test PRIVATE src)
  # The hooks reach clip::detail through clip_internal.h, which includes FFmpeg;
  # mv_clip keeps those headers PRIVATE.
  target_include_directories(mv_clipjob_test SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_compile_definitions(mv_clipjob_test PRIVATE MV_CLIPJOB_TEST_HOOKS=1)
  target_compile_definitions(mv_tests PRIVATE MV_CLIPJOB_PATH="$<TARGET_FILE:mv_clipjob_test>")
  add_dependencies(mv_tests mv_clipjob_test)
  # test_meta.cpp writes fixtures with Exiv2 and libavformat's muxer.
  target_include_directories(mv_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(mv_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(mv_tests PRIVATE Exiv2::exiv2lib ${FFMPEG_LIBRARIES})
  include(Catch)
  catch_discover_tests(mv_tests)

  # Milestone G: the same Import suite as Windows and Linux CI.
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
  catch_discover_tests(mv_import_tests TEST_PREFIX "import_" PROPERTIES ENVIRONMENT "TZ=UTC")
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — Darwin host (PR 16–20), GPL-2.0-or-later")
