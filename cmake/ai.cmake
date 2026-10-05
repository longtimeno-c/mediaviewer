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
# ONNX Runtime 1.30.0 (MIT) is fetched per platform, pinned by SHA-256, into
# the build tree: the header for mv_infer, and the runtime files the pack
# stages beside mv_ai. macOS ships arm64 only (Microsoft publishes no x86_64
# macOS build of 1.30): mv_ai is arm64-only on the Mac and Intel Macs are not
# offered Local search (docs/design/17, 2026-09-26).
#
# Expects, already defined by the includer:
#   mv_project_options, MV_SQLITE_TARGET, mv_addon (cmake/import.cmake)

set(MV_AI_VERSION "1.0.0" CACHE STRING "AI pack version (its manifest's)")
set(MV_ORT_VERSION "1.30.0")
option(MV_AI_CUDA_PIECE "Stage the ai-cuda piece (ORT's CUDA 13 build) beside the Core pack" OFF)
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(R "${MV_SOURCE_ROOT}")

# ---- ONNX Runtime, pinned -----------------------------------------------------
set(_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${MV_ORT_VERSION}")
if(WIN32)
  set(_ort_name "onnxruntime-win-x64-${MV_ORT_VERSION}")
  set(_ort_ext "zip")
  set(_ort_sha "c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949")
elseif(APPLE)
  set(_ort_name "onnxruntime-osx-arm64-${MV_ORT_VERSION}")
  set(_ort_ext "tgz")
  set(_ort_sha "6ebb5062a934537c352937821f9fe9718e7de1a2db1122a93dd363ffd53a7012")
else()
  set(_ort_name "onnxruntime-linux-x64-${MV_ORT_VERSION}")
  set(_ort_ext "tgz")
  set(_ort_sha "a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd")
endif()

function(mv_fetch_ort name ext sha out_dir)
  set(_dl "${CMAKE_BINARY_DIR}/_ort/${name}.${ext}")
  set(_dir "${CMAKE_BINARY_DIR}/_ort/${name}")
  if(NOT EXISTS "${_dir}/include/onnxruntime_c_api.h")
    if(DEFINED ENV{MV_ORT_ARCHIVE_DIR} AND EXISTS "$ENV{MV_ORT_ARCHIVE_DIR}/${name}.${ext}")
      set(_dl "$ENV{MV_ORT_ARCHIVE_DIR}/${name}.${ext}")  # offline / CI cache
    else()
      message(STATUS "Fetching ${name}.${ext} (ONNX Runtime, MIT)")
      file(DOWNLOAD "${_ort_base}/${name}.${ext}" "${_dl}" EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
    endif()
    file(SHA256 "${_dl}" _got)
    if(NOT _got STREQUAL sha)
      message(FATAL_ERROR "${name}.${ext}: SHA-256 ${_got}, expected ${sha}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${CMAKE_BINARY_DIR}/_ort")
    # The debug symbols are most of the archive and never used here.
    file(GLOB _pdbs "${_dir}/lib/*.pdb")
    file(GLOB _dsyms LIST_DIRECTORIES true "${_dir}/lib/*.dSYM")
    if(_pdbs OR _dsyms)
      file(REMOVE_RECURSE ${_pdbs} ${_dsyms})
    endif()
  endif()
  set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()

mv_fetch_ort(${_ort_name} ${_ort_ext} ${_ort_sha} MV_ORT_DIR)
set(MV_ORT_INCLUDE "${MV_ORT_DIR}/include")

# ---- mv_infer -----------------------------------------------------------------
if(WIN32)
  set(MV_INFER_DYLIB ${R}/src/infer/ort_dylib_win.cpp)
  set(MV_AI_PLATFORM ${R}/src/addons/ai/platform_win.cpp)
else()
  set(MV_INFER_DYLIB ${R}/src/infer/ort_dylib_posix.cpp)
  set(MV_AI_PLATFORM ${R}/src/addons/ai/platform_posix.cpp)
endif()
# The Photos library source (issue #72): PhotoKit on the Mac, none elsewhere.
if(APPLE)
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/photos_mac.mm)
else()
  list(APPEND MV_AI_PLATFORM ${R}/src/addons/ai/photos_none.cpp)
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
  ${R}/src/infer/audio_features.cpp
  ${R}/src/infer/audio_features.h
  ${R}/src/infer/audio_models.cpp
  ${R}/src/infer/audio_models.h
  ${MV_INFER_DYLIB}
)
target_include_directories(mv_infer PUBLIC "${R}/src")
target_include_directories(mv_infer SYSTEM PRIVATE "${MV_ORT_INCLUDE}")
target_link_libraries(mv_infer PUBLIC mv_project_options)
if(NOT WIN32)
  target_link_libraries(mv_infer PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(mv_infer PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# ---- mv_ai_engine ---------------------------------------------------------------
add_library(mv_ai_engine STATIC
  ${R}/src/addons/ai/host.cpp
  ${R}/src/addons/ai/host.h
  ${R}/src/addons/ai/index_db.cpp
  ${R}/src/addons/ai/index_db.h
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
  ${R}/src/addons/ai/platform.h
  ${R}/src/addons/ai/photos_source.h
  ${MV_AI_PLATFORM}
  ${R}/src/abi/include/mediaviewer/mediaviewer_ai.h
)
target_include_directories(mv_ai_engine PUBLIC "${R}/src" "${R}/src/abi/include")
target_link_libraries(mv_ai_engine PUBLIC mv_infer mv_project_options PRIVATE ${MV_SQLITE_TARGET})
find_package(Threads REQUIRED)
target_link_libraries(mv_ai_engine PUBLIC Threads::Threads)
if(APPLE)
  target_link_libraries(mv_ai_engine PRIVATE "-framework CoreFoundation" "-framework IOKit"
                        "-framework Foundation" "-framework AppKit" "-framework Photos"
                        "-framework AVFoundation")
endif()
set_target_properties(mv_ai_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# ---- mv_ai (the add-on) -----------------------------------------------------------
add_library(mv_ai MODULE "${R}/src/addons/ai/addon_entry.cpp")
target_link_libraries(mv_ai PRIVATE mv_ai_engine)
target_compile_definitions(mv_ai PRIVATE MV_AI_VERSION="${MV_AI_VERSION}")
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
  set_target_properties(mv_ai mv_ai_engine mv_infer PROPERTIES OSX_ARCHITECTURES "arm64")
elseif(WIN32)
  set_target_properties(mv_ai PROPERTIES PREFIX "")
endif()

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
            "${MV_ORT_DIR}/lib/libonnxruntime.${MV_ORT_VERSION}.dylib"
            "${CMAKE_BINARY_DIR}/addons/ai/libonnxruntime.dylib"
    COMMENT "Staging ONNX Runtime beside libmv_ai.dylib")
else()
  add_custom_command(TARGET mv_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MV_ORT_DIR}/lib/libonnxruntime.so.${MV_ORT_VERSION}"
            "${CMAKE_BINARY_DIR}/addons/ai/libonnxruntime.so"
    COMMENT "Staging ONNX Runtime beside mv_ai")
endif()

# ai-cuda: ORT's CUDA 13 build, its own piece (docs/design/17 "each its own optional
# sub-pack"), published on stable Windows releases (log 2026-10-03). The CUDA
# runtime and cuDNN are user-supplied: NVIDIA's EULA is not an OSI licence, so
# the piece carries only ORT's MIT files and the self-test falls back to CPU,
# saying why, when they are missing. Bundling them still needs the review.
if(WIN32 AND MV_AI_CUDA_PIECE)
  mv_fetch_ort("onnxruntime-win-x64-gpu_cuda13-${MV_ORT_VERSION}" "zip"
               "8fa4b08359af682cd605892cb59077049700b640128bb93fd2c7776cf9f55bdc" MV_ORT_CUDA_DIR)
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
