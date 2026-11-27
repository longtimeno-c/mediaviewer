# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone H (docs/design/17-local-ai-search.md): the AI pack. Included by the
# Windows root, cmake/darwin.cmake and the headless cmake/portable build, after
# cmake/import.cmake (it reuses mv_addon for the tests' host table).
#
#   mv_infer       src/infer: ONNX Runtime behind our interface, loaded at run
#                  time (never linked), the CLIP tokenizer and preprocessing,
#                  the model wrappers. Needs ORT's C header only.
#   mv_ai_engine   src/addons/ai: index.db, roots and scans, the indexer and
#                  its yield policy, the search matrix, people.
#   mv_ai          the add-on: ONE shared library whose only export is
#                  mv_addon_get. It links SQLite and nothing of the core; the
#                  core reaches it only through the host table (v2 pixels).
#                  Built beside the app into build/addons/ai, NEVER installed
#                  with it: tools/package/ai-pack.py stages the Core pack
#                  (mv_ai + ORT + models) and addon-pack.py signs it. The base
#                  install tree stays byte-identical (PR 20 verify), and
#                  tools/package/build-release.ps1 fails if ORT lands in it.
#
# ONNX Runtime 1.30.0 (MIT) is fetched from Microsoft's NuGet packages, pinned
# by SHA-256, into the build tree: the header for mv_infer, and the runtime files the pack
# stages beside mv_ai. macOS ships arm64 only (Microsoft publishes no x86_64
# macOS build of 1.30): mv_ai is arm64-only on the Mac and Intel Macs are not
# offered Local search (docs/design/17, 2026-09-26).
#
# Expects, already defined by the includer:
#   mv_project_options, MV_SQLITE_TARGET, mv_addon (cmake/import.cmake)

# mv_download, from here: cmake/portable includes this file without the root.
include("${CMAKE_CURRENT_LIST_DIR}/download.cmake")

set(MV_AI_VERSION "1.0.0" CACHE STRING "AI pack version (its manifest's)")
set(MV_ORT_VERSION "1.30.0")
option(MV_AI_CUDA_PIECE "Stage the ai-cuda piece (ORT's CUDA 13 build) beside the Core pack" OFF)
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(R "${MV_SOURCE_ROOT}")

# ---- ONNX Runtime, pinned -----------------------------------------------------
# Microsoft's NuGet packages, not the GitHub release archives: the
# microsoft/onnxruntime repository has returned 404 since 2026-11-18. The same
# 1.30.0 build (the macOS dylib differs from the GitHub archive's only in its
# ad-hoc signature; the headers only in line endings). One package carries
# every platform, so one pin covers Windows, macOS, Linux and the iPhone.
set(_ort_nuget "https://api.nuget.org/v3-flatcontainer")
set(_ort_package "Microsoft.ML.OnnxRuntime")
set(_ort_sha "30df119bd9164eaa187666d93147448fdd4a9a1a3caf83f1505daf7af2d1c5d2")
if(WIN32)
  set(_ort_rid "win-x64")
elseif(APPLE)
  set(_ort_rid "osx-arm64")
else()
  set(_ort_rid "linux-x64")
endif()

# mv_fetch_ort(<package> <rid> <sha256> <out_var>): one platform's files from
# ORT's NuGet package, laid out as <dir>/include, <dir>/lib (the native
# libraries, under their package names), LICENSE and ThirdPartyNotices.txt.
function(mv_fetch_ort package rid sha out_dir)
  string(TOLOWER "${package}" _id)
  set(_file "${_id}.${MV_ORT_VERSION}.nupkg")
  set(_dir "${CMAKE_BINARY_DIR}/_ort/${_id}-${rid}-${MV_ORT_VERSION}")
  if(NOT EXISTS "${_dir}/include/onnxruntime_c_api.h")
    set(_dl "${CMAKE_BINARY_DIR}/_ort/${_file}")
    if(DEFINED ENV{MV_ORT_ARCHIVE_DIR} AND EXISTS "$ENV{MV_ORT_ARCHIVE_DIR}/${_file}")
      set(_dl "$ENV{MV_ORT_ARCHIVE_DIR}/${_file}")  # offline / CI cache
    else()
      message(STATUS "Fetching ${_file} (ONNX Runtime, MIT)")
      mv_download("${_ort_nuget}/${_id}/${MV_ORT_VERSION}/${_file}" "${_dl}" "${sha}")
    endif()
    file(SHA256 "${_dl}" _got)
    if(NOT _got STREQUAL sha)
      message(FATAL_ERROR "${_file}: SHA-256 ${_got}, expected ${sha}")
    endif()
    # Only this platform's libraries, the headers (build/ in the CPU package,
    # buildTransitive/ in the GPU one) and the licence files.
    set(_tmp "${_dir}.extract")
    file(REMOVE_RECURSE "${_tmp}" "${_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}"
         PATTERNS "runtimes/${rid}/native" "build*/native/include"
                  "LICENSE" "ThirdPartyNotices.txt")
    file(GLOB _libs "${_tmp}/runtimes/${rid}/native/*")
    file(GLOB _headers "${_tmp}/build/native/include/*" "${_tmp}/buildTransitive/native/include/*")
    if(NOT _libs OR NOT _headers)
      message(FATAL_ERROR "${_file}: no runtimes/${rid}/native or native/include in the package")
    endif()
    file(COPY ${_libs} DESTINATION "${_dir}/lib")
    file(COPY "${_tmp}/LICENSE" "${_tmp}/ThirdPartyNotices.txt" DESTINATION "${_dir}")
    file(COPY ${_headers} DESTINATION "${_dir}/include")  # last: marks the layout done
    file(REMOVE_RECURSE "${_tmp}")
    if(_dl STREQUAL "${CMAKE_BINARY_DIR}/_ort/${_file}")
      file(REMOVE "${_dl}")  # ~150 MB, mostly other platforms; the layout is kept
    endif()
  endif()
  set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()

if(IOS)
  # An app may not load downloaded code: Microsoft's static xcframework, linked
  # into the app. It ships inside the same NuGet package as the desktop's
  # libraries (runtimes/ios/native, for .NET iOS), so the phone and desktop
  # share one pinned download. The CocoaPods pod-archive it used to come from
  # sits behind download.onnxruntime.ai, whose certificate stopped matching
  # the host in 2026-11.
  string(TOLOWER "${_ort_package}" _id)
  set(_file "${_id}.${MV_ORT_VERSION}.nupkg")
  set(MV_ORT_DIR "${CMAKE_BINARY_DIR}/_ort/${_id}-ios-${MV_ORT_VERSION}")
  if(NOT EXISTS "${MV_ORT_DIR}/onnxruntime.xcframework/Info.plist")
    set(_dl "${CMAKE_BINARY_DIR}/_ort/${_file}")
    if(DEFINED ENV{MV_ORT_ARCHIVE_DIR} AND EXISTS "$ENV{MV_ORT_ARCHIVE_DIR}/${_file}")
      set(_dl "$ENV{MV_ORT_ARCHIVE_DIR}/${_file}")  # offline / CI cache
    else()
      message(STATUS "Fetching ${_file} (ONNX Runtime, MIT)")
      mv_download("${_ort_nuget}/${_id}/${MV_ORT_VERSION}/${_file}" "${_dl}" "${_ort_sha}")
    endif()
    file(SHA256 "${_dl}" _got)
    if(NOT _got STREQUAL _ort_sha)
      message(FATAL_ERROR "${_file}: SHA-256 ${_got}, expected ${_ort_sha}")
    endif()
    set(_tmp "${MV_ORT_DIR}.extract")
    file(REMOVE_RECURSE "${_tmp}" "${MV_ORT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}"
         PATTERNS "runtimes/ios/native/onnxruntime.xcframework.zip" "LICENSE" "ThirdPartyNotices.txt")
    if(NOT EXISTS "${_tmp}/runtimes/ios/native/onnxruntime.xcframework.zip")
      message(FATAL_ERROR "${_file}: no runtimes/ios/native/onnxruntime.xcframework.zip in the package")
    endif()
    file(COPY "${_tmp}/LICENSE" "${_tmp}/ThirdPartyNotices.txt" DESTINATION "${MV_ORT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${_tmp}/runtimes/ios/native/onnxruntime.xcframework.zip"
         DESTINATION "${MV_ORT_DIR}")  # last: onnxruntime.xcframework marks the layout done
    file(REMOVE_RECURSE "${_tmp}")
    if(_dl STREQUAL "${CMAKE_BINARY_DIR}/_ort/${_file}")
      file(REMOVE "${_dl}")  # ~150 MB, mostly other platforms; the layout is kept
    endif()
  endif()
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(_ort_slice "ios-arm64_x86_64-simulator")
  else()
    set(_ort_slice "ios-arm64")
  endif()
  set(MV_ORT_INCLUDE "${MV_ORT_DIR}/onnxruntime.xcframework/${_ort_slice}/onnxruntime.framework/Headers")
  add_library(mv_ort_static STATIC IMPORTED)
  set_target_properties(mv_ort_static PROPERTIES IMPORTED_LOCATION
    "${MV_ORT_DIR}/onnxruntime.xcframework/${_ort_slice}/onnxruntime.framework/onnxruntime")
  # What the archive's own objects need.
  target_link_libraries(mv_ort_static INTERFACE "-framework Foundation" "-framework CoreML"
                        "-framework UIKit" "-framework Network")
else()
  mv_fetch_ort(${_ort_package} ${_ort_rid} ${_ort_sha} MV_ORT_DIR)
  set(MV_ORT_INCLUDE "${MV_ORT_DIR}/include")
endif()

# ---- PDFium, pinned (#354) ------------------------------------------------------
# A PDF's text for Local search (docs/plans/document-search.md §2, owner call 2):
# bblanchon/pdfium-binaries (PDFium is BSD-3-Clause / Apache-2.0), loaded at
# run time by src/addons/ai/pdf_pdfium.cpp from beside mv_ai. Never linked and
# never in the base install tree; the viewer renders PDFs with the OS.
set(MV_PDFIUM_RELEASE "8066")  # chromium/8066, 2026-09-21
if(WIN32)
  set(_pdfium_name "pdfium-win-x64")
  set(_pdfium_sha "739a57d597d864297909cc40a2411eba728490c76a0fa25e3ea299c7f6b07020")
elseif(APPLE)
  set(_pdfium_name "pdfium-mac-arm64")
  set(_pdfium_sha "336219e80580b93c6523f44db7dc1de59cc497b13a7390ddac84223f68ca162b")
else()
  set(_pdfium_name "pdfium-linux-x64")
  set(_pdfium_sha "0b43f405477cf2cfc4dbff06905093c3309756c6bca1fb9da99234a2ca97fed2")
endif()
set(MV_PDFIUM_DIR "${CMAKE_BINARY_DIR}/_pdfium/${_pdfium_name}-${MV_PDFIUM_RELEASE}")
if(NOT IOS AND NOT EXISTS "${MV_PDFIUM_DIR}/include/fpdf_text.h")  # iOS: pdf_none.cpp
  set(_pdfium_dl "${CMAKE_BINARY_DIR}/_pdfium/${_pdfium_name}-${MV_PDFIUM_RELEASE}.tgz")
  if(DEFINED ENV{MV_ORT_ARCHIVE_DIR} AND EXISTS "$ENV{MV_ORT_ARCHIVE_DIR}/${_pdfium_name}-${MV_PDFIUM_RELEASE}.tgz")
    set(_pdfium_dl "$ENV{MV_ORT_ARCHIVE_DIR}/${_pdfium_name}-${MV_PDFIUM_RELEASE}.tgz")  # offline / CI cache
  else()
    message(STATUS "Fetching ${_pdfium_name}.tgz chromium/${MV_PDFIUM_RELEASE} (PDFium, BSD-3-Clause / Apache-2.0)")
    mv_download(
      "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${MV_PDFIUM_RELEASE}/${_pdfium_name}.tgz"
      "${_pdfium_dl}" "${_pdfium_sha}")
  endif()
  file(SHA256 "${_pdfium_dl}" _got)
  if(NOT _got STREQUAL _pdfium_sha)
    message(FATAL_ERROR "${_pdfium_name}.tgz: SHA-256 ${_got}, expected ${_pdfium_sha}")
  endif()
  file(MAKE_DIRECTORY "${MV_PDFIUM_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${_pdfium_dl}" DESTINATION "${MV_PDFIUM_DIR}")
endif()
set(MV_PDFIUM_INCLUDE "${MV_PDFIUM_DIR}/include")
if(WIN32)
  set(MV_PDFIUM_LIB "${MV_PDFIUM_DIR}/bin/pdfium.dll")
elseif(APPLE)
  set(MV_PDFIUM_LIB "${MV_PDFIUM_DIR}/lib/libpdfium.dylib")
else()
  set(MV_PDFIUM_LIB "${MV_PDFIUM_DIR}/lib/libpdfium.so")
endif()

# ---- mv_infer -----------------------------------------------------------------
set(MV_AI_PDF ${R}/src/addons/ai/pdf_pdfium.cpp)
if(WIN32)
  set(MV_INFER_DYLIB ${R}/src/infer/ort_dylib_win.cpp)
  set(MV_AI_PLATFORM ${R}/src/addons/ai/platform_win.cpp)
elseif(IOS)
  # The phone answers queries and indexes nothing (docs/plans/mobile.md 8.1).
  set(MV_INFER_DYLIB ${R}/src/infer/ort_static_ios.cpp)
  set(MV_AI_PLATFORM ${R}/src/addons/ai/platform_ios.cpp)
  set(MV_AI_PDF ${R}/src/addons/ai/pdf_none.cpp)
else()
  set(MV_INFER_DYLIB ${R}/src/infer/ort_dylib_posix.cpp)
  set(MV_AI_PLATFORM ${R}/src/addons/ai/platform_posix.cpp)
endif()
# The Photos library source (issue #72): PhotoKit on the Mac, none elsewhere.
if(APPLE AND NOT IOS)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/photos_mac.mm)
else()
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/photos_none.cpp)
endif()
# Cloud files in indexed folders (2026-10-05): OneDrive on Windows, iCloud
# Drive on the Mac (src/addons/ai/cloud_files.h).
if(WIN32)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/cloud_win.cpp)
elseif(APPLE AND NOT IOS)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/cloud_mac.mm)
else()
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/cloud_none.cpp)
endif()

# The model downloader's transport (#482 T3, src/addons/ai/fetch.h): WinHTTP on
# Windows, NSURLSession on the Mac. Its own library (mv_model_fetch below), so
# Enhance (#297) fetches its weights with the same code.
if(WIN32)
  set(MV_FETCH_PLATFORM ${R}/src/addons/ai/fetch_win.cpp)
elseif(APPLE AND NOT IOS)
  set(MV_FETCH_PLATFORM ${R}/src/addons/ai/fetch_mac.mm)
else()
  set(MV_FETCH_PLATFORM ${R}/src/addons/ai/fetch_none.cpp)
endif()

# Text in pictures (#403, src/addons/ai/text_reader.h): Apple Vision on the
# Mac, Windows.Media.Ocr (C++/WinRT from the Windows SDK) on Windows.
if(WIN32)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/ocr_win.cpp)
  # C++/WinRT throws hresult_error, which ocr_win.cpp catches at its edge
  # (/EHsc, as CMake's default); /bigobj for the projection's headers.
  set_source_files_properties(${R}/src/addons/ai/ocr_win.cpp PROPERTIES COMPILE_OPTIONS "/bigobj;/EHsc")
elseif(APPLE AND NOT IOS)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/ocr_mac.mm)
else()
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/ocr_none.cpp)
endif()

add_library(mv_infer STATIC
  ${R}/src/infer/clip_tokenizer.cpp
  ${R}/src/infer/clip_tokenizer.h
  ${R}/src/infer/preprocess.cpp
  ${R}/src/infer/preprocess.h
  ${R}/src/infer/ort.cpp
  ${R}/src/infer/ort.h
  ${R}/src/infer/models.cpp
  ${R}/src/infer/models.h
  ${R}/src/infer/model_files.h
  ${R}/src/infer/audio_features.cpp
  ${R}/src/infer/audio_features.h
  ${R}/src/infer/audio_models.cpp
  ${R}/src/infer/audio_models.h
  ${R}/src/infer/text_models.cpp
  ${R}/src/infer/text_models.h
  ${MV_INFER_DYLIB}
)
target_include_directories(mv_infer PUBLIC "${R}/src")
target_include_directories(mv_infer SYSTEM PRIVATE "${MV_ORT_INCLUDE}")
target_link_libraries(mv_infer PUBLIC mv_project_options)
if(NOT WIN32)
  target_link_libraries(mv_infer PRIVATE ${CMAKE_DL_LIBS})
endif()
if(IOS)
  target_link_libraries(mv_infer PUBLIC mv_ort_static)
endif()
set_target_properties(mv_infer PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# The catalogue's generic half and the shared SHA-256 (src/catalogue).
include("${CMAKE_CURRENT_LIST_DIR}/catalogue.cmake")

# ---- mv_model_fetch ---------------------------------------------------------------
# The model downloader (#482 T3): a plain, resumable, verified GET of the
# Hugging Face files an add-on's signed manifest pins (`remote`). Nothing in it
# knows about tiers or models: the AI pack and Enhance (#297) link it.
add_library(mv_model_fetch STATIC
  ${R}/src/addons/ai/fetch.cpp
  ${R}/src/addons/ai/fetch.h
  ${MV_FETCH_PLATFORM}
)
target_include_directories(mv_model_fetch PUBLIC "${R}/src")
target_link_libraries(mv_model_fetch PUBLIC mv_project_options mv_sha256)
if(APPLE)
  target_link_libraries(mv_model_fetch PRIVATE "-framework Foundation")
elseif(WIN32)
  target_link_libraries(mv_model_fetch PRIVATE winhttp)
endif()
set_target_properties(mv_model_fetch PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# ---- mv_ai_engine ---------------------------------------------------------------
add_library(mv_ai_engine STATIC
  ${R}/src/addons/ai/host.cpp
  ${R}/src/addons/ai/host.h
  ${R}/src/addons/ai/index_db.cpp
  ${R}/src/addons/ai/index_db.h
  ${R}/src/addons/ai/catalog.cpp
  ${R}/src/addons/ai/nfc.cpp
  ${R}/src/addons/ai/catalog.h
  ${R}/src/wire/secrets.h
  # iOS: mv_wire carries secrets_file.cpp and secrets_ios.cpp (linked below).
  $<$<NOT:$<PLATFORM_ID:iOS>>:${R}/src/wire/secrets_file.cpp>
  $<$<PLATFORM_ID:Windows>:${R}/src/wire/secrets_win.cpp>
  $<$<PLATFORM_ID:Darwin>:${R}/src/wire/secrets_mac.cpp>
  $<$<PLATFORM_ID:Linux>:${R}/src/wire/secrets_linux.cpp>
  ${R}/src/addons/ai/vectors.cpp
  ${R}/src/addons/ai/vectors.h
  ${R}/src/addons/ai/vocabulary.cpp
  ${R}/src/addons/ai/vocabulary.h
  ${R}/src/addons/ai/face_refine.cpp
  ${R}/src/addons/ai/face_refine.h
  ${R}/src/addons/ai/query.cpp
  ${R}/src/addons/ai/query.h
  ${R}/src/addons/ai/faces.cpp
  ${R}/src/addons/ai/faces.h
  ${R}/src/addons/ai/transfer.cpp
  ${R}/src/addons/ai/transfer.h
  ${R}/src/addons/ai/engine.cpp
  ${R}/src/addons/ai/engine.h
  ${R}/src/addons/ai/pack.cpp
  ${R}/src/addons/ai/pack.h
  ${R}/src/addons/ai/provider_verdict.cpp
  ${R}/src/addons/ai/provider_verdict.h
  ${R}/src/addons/ai/tiers.cpp
  ${R}/src/addons/ai/machine.cpp
  ${R}/src/addons/ai/machine.h
  ${R}/src/addons/ai/tiers.h
  ${R}/src/addons/ai/platform.h
  ${R}/src/addons/ai/photos_source.h
  ${R}/src/addons/ai/text_reader.h
  ${R}/src/addons/ai/passages.cpp
  ${R}/src/addons/ai/passages.h
  ${R}/src/addons/ai/pdf_reader.h
  ${MV_AI_PDF}
  ${MV_AI_PLATFORM}
  ${R}/src/abi/include/mediaviewer/mediaviewer_ai.h
)
target_include_directories(mv_ai_engine PUBLIC "${R}/src" "${R}/src/abi/include")
# PDFium's C headers for pdf_pdfium.cpp alone (the library is loaded at run time).
set_source_files_properties(${R}/src/addons/ai/pdf_pdfium.cpp PROPERTIES INCLUDE_DIRECTORIES "${MV_PDFIUM_INCLUDE}")
target_link_libraries(mv_ai_engine PUBLIC mv_infer mv_model_fetch mv_catalogue mv_project_options PRIVATE ${MV_SQLITE_TARGET})
find_package(Threads REQUIRED)
target_link_libraries(mv_ai_engine PUBLIC Threads::Threads)
# The catalogue's Ed25519 signature (docs/plans/mobile.md §5).
target_link_libraries(mv_ai_engine PRIVATE ${MV_SODIUM_TARGET})
if(UNIX AND NOT APPLE)
  target_link_libraries(mv_ai_engine PRIVATE ${CMAKE_DL_LIBS})  # secrets_linux.cpp
endif()
if(APPLE AND NOT IOS)
  target_link_libraries(mv_ai_engine PRIVATE "-framework Security" "-framework CoreFoundation" "-framework IOKit"
                        "-framework Foundation" "-framework AppKit" "-framework Photos"
                        "-framework AVFoundation" "-framework Network" "-framework Vision"
                        "-framework CoreGraphics")
endif()
if(IOS)
  target_link_libraries(mv_ai_engine PRIVATE mv_wire)  # secrets::os_store
endif()
if(WIN32)
  target_link_libraries(mv_ai_engine PRIVATE dxgi advapi32 crypt32)  # machine() (#482); the signing key
endif()
set_target_properties(mv_ai_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# iOS links the engine statically: no add-on module, no pack, no editing-app host.
if(IOS)
  return()
endif()

# ---- mv_ai (the add-on) -----------------------------------------------------------
add_library(mv_ai MODULE "${R}/src/addons/ai/addon_entry.cpp")
target_link_libraries(mv_ai PRIVATE mv_ai_engine)
target_compile_definitions(mv_ai PRIVATE MV_AI_VERSION="${MV_AI_VERSION}")
if(WIN32)
  # The Cloud Files API (OneDrive, cloud_win.cpp) is optional: a Windows
  # without cldapi.dll still loads the pack and searches (2026-10-07).
  target_link_options(mv_ai PRIVATE "/DELAYLOAD:cldapi.dll")
  target_link_libraries(mv_ai PRIVATE delayimp)
endif()
set_target_properties(mv_ai PROPERTIES
  OUTPUT_NAME "mv_ai"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/ai"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/ai")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${_cfg} _CFG)
  set_target_properties(mv_ai PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/ai"
    RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/ai")
endforeach()
if(APPLE)
  set_target_properties(mv_ai PROPERTIES PREFIX "lib" SUFFIX ".dylib")
  # ORT ships arm64 only: so does the pack (a universal app loads it on Apple
  # silicon; the Mac chrome never offers it on Intel).
  set_target_properties(mv_ai mv_ai_engine mv_infer mv_model_fetch PROPERTIES OSX_ARCHITECTURES "arm64")
elseif(WIN32)
  set_target_properties(mv_ai PROPERTIES PREFIX "")
endif()

# PDFium beside mv_ai, as the pack lays it out (#354; for tests and packing).
if(WIN32)
  set(_pdfium_staged "${CMAKE_BINARY_DIR}/addons/ai/pdfium.dll")
elseif(APPLE)
  set(_pdfium_staged "${CMAKE_BINARY_DIR}/addons/ai/libpdfium.dylib")
else()
  set(_pdfium_staged "${CMAKE_BINARY_DIR}/addons/ai/libpdfium.so")
endif()
add_custom_command(TARGET mv_ai POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MV_PDFIUM_LIB}" "${_pdfium_staged}"
  # Its parts' notices (FreeType, ICU, libjpeg-turbo, ...): addon-pack.py ships
  # them as LICENSES/pdfium.
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${MV_PDFIUM_DIR}/licenses" "${CMAKE_BINARY_DIR}/addons/ai/pdfium-licenses"
  COMMENT "Staging PDFium beside mv_ai")

# The runtime beside mv_ai, as the pack lays it out (for tests and packing).
if(WIN32)
  set(_ort_files
    "${MV_ORT_DIR}/lib/onnxruntime.dll"
    "${MV_ORT_DIR}/lib/onnxruntime_providers_shared.dll")
  add_custom_command(TARGET mv_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ort_files} "${CMAKE_BINARY_DIR}/addons/ai"
    COMMENT "Staging ONNX Runtime beside mv_ai.dll")
elseif(APPLE)
  add_custom_command(TARGET mv_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_ORT_DIR}/lib/libonnxruntime.dylib"
            "${CMAKE_BINARY_DIR}/addons/ai/libonnxruntime.dylib"
    COMMENT "Staging ONNX Runtime beside libmv_ai.dylib")
else()
  add_custom_command(TARGET mv_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_ORT_DIR}/lib/libonnxruntime.so"
            "${CMAKE_BINARY_DIR}/addons/ai/libonnxruntime.so"
    COMMENT "Staging ONNX Runtime beside mv_ai")
endif()

# ai-cuda: ORT's CUDA 13 build, its own piece (docs/design/17 "each its own optional
# sub-pack"), published on stable Windows releases (log 2026-10-03). The CUDA
# runtime and cuDNN are user-supplied: NVIDIA's EULA is not an OSI licence, so
# the piece carries only ORT's MIT files and the self-test falls back to CPU,
# saying why, when they are missing. Bundling them still needs the review.
if(WIN32 AND MV_AI_CUDA_PIECE)
  # Microsoft.ML.OnnxRuntime.Gpu.Windows 1.30.0 is the CUDA 13 build
  # (onnxruntime_providers_cuda.dll imports cublas64_13.dll).
  mv_fetch_ort("Microsoft.ML.OnnxRuntime.Gpu.Windows" "win-x64"
               "f86f88d2dcdbc1411a5a4f345751c37a85f965300e150da6d882bea41ea7296d" MV_ORT_CUDA_DIR)
  add_custom_target(mv_ai_cuda_piece ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/addons/ai-cuda"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_ORT_CUDA_DIR}/lib/onnxruntime.dll"
            "${MV_ORT_CUDA_DIR}/lib/onnxruntime_providers_shared.dll"
            "${MV_ORT_CUDA_DIR}/lib/onnxruntime_providers_cuda.dll"
            "${CMAKE_BINARY_DIR}/addons/ai-cuda"
    # ORT's MIT licence and notices go in the piece's LICENSES/ (addon-pack.py).
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_ORT_CUDA_DIR}/LICENSE" "${MV_ORT_CUDA_DIR}/ThirdPartyNotices.txt"
            "${CMAKE_BINARY_DIR}/addons/ai-cuda"
    COMMENT "Staging the ai-cuda piece (ORT CUDA 13 build)")
endif()

set(MV_ORT_LICENSE "${MV_ORT_DIR}/LICENSE")
set(MV_ORT_NOTICES "${MV_ORT_DIR}/ThirdPartyNotices.txt")

# docs/design/23: Local search from inside an editing app (portable half).
include("${CMAKE_CURRENT_LIST_DIR}/nle.cmake")
