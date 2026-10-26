# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Enhance add-on (#297): a restored copy of the photo, clip or audio file
# on screen, from a model downloaded on the person's click. Included by the
# Windows root, cmake/darwin.cmake and cmake/portable after ai.cmake (it links
# the model downloader, mv_model_fetch), so the builds cannot drift.
#
#   mv_enhance_engine  the models' specs and knobs, the downloads, the sheet
#                      and settings trees, the engine. ABI-free; the unit
#                      tests link it.
#   mv_enhance         the add-on: ONE shared library whose only export is
#                      mv_addon_get. It links nothing of the core: the core
#                      reaches it only through the host function table (v3:
#                      screens and slots). Its weights are not in its zip:
#                      its signed manifest pins them (`remote`) and it fetches
#                      them into its data folder when asked.
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/enhance, signed.
#
# Expects: mv_project_options, mv_model_fetch (cmake/ai.cmake), Threads.

set(MV_ENHANCE_VERSION "0.1.0" CACHE STRING "Enhance add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_E "${MV_SOURCE_ROOT}/src/addons/enhance")

if(WIN32)
  set(_enhance_memory ${_E}/memory_win.cpp)
elseif(APPLE)
  set(_enhance_memory ${_E}/memory_mac.cpp)
else()
  set(_enhance_memory ${_E}/memory_linux.cpp)
endif()

add_library(mv_enhance_engine STATIC
  ${_E}/downloads.cpp
  ${_E}/downloads.h
  ${_E}/engine.cpp
  ${_E}/engine.h
  ${_E}/prefs.cpp
  ${_E}/prefs.h
  ${_E}/screens.cpp
  ${_E}/screens.h
  ${_E}/sniff.cpp
  ${_E}/sniff.h
  ${_E}/sound_job.cpp
  ${_E}/sound_job.h
  ${_E}/picture_job.cpp
  ${_E}/picture_job.h
  ${_E}/spec.cpp
  ${_E}/spec.h
  ${_E}/runner.cpp
  ${_E}/runner.h
  ${_E}/host_port.cpp
  ${_E}/host_port.h
  ${_E}/stream_model.cpp
  ${_E}/stream_model.h
  ${_E}/memory.h
  ${_enhance_memory}
)
target_include_directories(mv_enhance_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
find_package(Threads REQUIRED)
# mv_infer: ONNX Runtime behind src/infer, loaded at run time from the
# add-on's own folder (never linked).
target_link_libraries(mv_enhance_engine PUBLIC mv_project_options mv_model_fetch mv_infer Threads::Threads)
set_target_properties(mv_enhance_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  set(_enhance_port ${MV_SOURCE_ROOT}/src/addons/places/self_dir_win.cpp)
else()
  set(_enhance_port ${MV_SOURCE_ROOT}/src/addons/places/self_dir_posix.cpp)
endif()

add_library(mv_enhance MODULE
  ${_E}/addon_entry.cpp
  ${MV_SOURCE_ROOT}/src/addons/places/self_dir.h
  ${_enhance_port}
)
target_link_libraries(mv_enhance PRIVATE mv_enhance_engine)
target_compile_definitions(mv_enhance PRIVATE MV_ENHANCE_VERSION="${MV_ENHANCE_VERSION}")
if(NOT WIN32)
  target_link_libraries(mv_enhance PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(mv_enhance PROPERTIES
  OUTPUT_NAME "mv_enhance"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/enhance"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/enhance")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${_cfg} _CFG)
  set_target_properties(mv_enhance PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/enhance"
    RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/enhance")
endforeach()
if(APPLE)
  set_target_properties(mv_enhance PROPERTIES PREFIX "lib" SUFFIX ".dylib")
  # The downloader is arm64-only on the Mac with the AI pack, and the models
  # will run on ONNX Runtime, which ships no x86_64 macOS build (docs/design/17).
  set_target_properties(mv_enhance mv_enhance_engine PROPERTIES OSX_ARCHITECTURES "arm64")
elseif(WIN32)
  set_target_properties(mv_enhance PROPERTIES PREFIX "")
endif()

# Laid out as the package is (for tests, sideloading and addon-pack.py): ONNX
# Runtime and its notices beside the library, each kind's model.json under
# models/, and remote.json, the weights' pins addon-pack.py signs into the
# manifest (never a file of the package).
set(_enhance_out "${CMAKE_BINARY_DIR}/addons/enhance")
if(WIN32)
  set(_enhance_ort "${MV_ORT_DIR}/lib/onnxruntime.dll" "${MV_ORT_DIR}/lib/onnxruntime_providers_shared.dll")
  add_custom_command(TARGET mv_enhance POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_enhance_ort} "${_enhance_out}")
elseif(APPLE)
  add_custom_command(TARGET mv_enhance POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MV_ORT_DIR}/lib/libonnxruntime.${MV_ORT_VERSION}.dylib"
            "${_enhance_out}/libonnxruntime.dylib")
else()
  add_custom_command(TARGET mv_enhance POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MV_ORT_DIR}/lib/libonnxruntime.so.${MV_ORT_VERSION}"
            "${_enhance_out}/libonnxruntime.so")
endif()
add_custom_command(TARGET mv_enhance POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MV_ORT_LICENSE}" "${MV_ORT_NOTICES}" "${_enhance_out}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${_E}/models" "${_enhance_out}/models"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_E}/remote.json" "${_enhance_out}/remote.json"
  COMMENT "Staging ONNX Runtime, the models' specs and their pins beside mv_enhance")

# The engine's own cases (tests/test_enhance.cpp) and the add-on through the
# real loader (tests/test_enhance_addon.cpp): its manifest accepted, then
# loaded from a signed store whose manifest pins a fixture weights file,
# the settings page and sheet answered through the v3 host table.
function(mv_enhance_tests target)
  target_sources(${target} PRIVATE "${MV_SOURCE_ROOT}/tests/test_enhance.cpp")
  target_link_libraries(${target} PRIVATE mv_enhance_engine)
  target_compile_definitions(${target} PRIVATE
    MV_ENHANCE_AUDIO_SPEC="${MV_SOURCE_ROOT}/src/addons/enhance/models/audio/model.json")
endfunction()
function(mv_enhance_addon_tests target)
  target_sources(${target} PRIVATE "${MV_SOURCE_ROOT}/tests/test_enhance_addon.cpp")
  target_compile_definitions(${target} PRIVATE
    MV_ENHANCE_MODULE_PATH="$<TARGET_FILE:mv_enhance>"
    MV_ENHANCE_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/enhance/manifest.json")
  add_dependencies(${target} mv_enhance)
endfunction()

# End to end over the real host table (tests/test_enhance_media.cpp): the
# sound rows' thunks over the hosts' FFmpeg reader and writer. Called where
# the AI suite is built (it links mv_addon_media).
function(mv_enhance_media_tests target)
  target_sources(${target} PRIVATE "${MV_SOURCE_ROOT}/tests/test_enhance_media.cpp")
  # mv_edit: the photo case's JPEG and PNG fixtures (edit::encode).
  target_link_libraries(${target} PRIVATE mv_enhance_engine mv_edit)
  # FFmpeg itself for the synthetic clips (clip_fixture.h), as mv_clipjob_test.
  target_include_directories(${target} SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(${target} PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(${target} PRIVATE ${FFMPEG_LIBRARIES})
  target_compile_definitions(${target} PRIVATE
    MV_ENHANCE_AUDIO_SPEC="${MV_SOURCE_ROOT}/src/addons/enhance/models/audio/model.json")
endfunction()
