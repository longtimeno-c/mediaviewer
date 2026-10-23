# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Enhance add-on (#297, docs/plans/enhance.md): restored copies of photos,
# video and audio, with models from Hugging Face chosen by this machine's spec.
# Included by the Windows root, cmake/darwin.cmake (arm64 only, as ai.cmake:
# ONNX Runtime has no x86_64 macOS build, so an Intel Mac gets no Enhance) and
# the headless cmake/portable build, AFTER cmake/ai.cmake: it links
# mv_model_fetch, the downloader both add-ons share.
#
#   mv_enhance_engine  the catalogue, the local check, the rule, the download
#                      store and the Models page's view tree. Links
#                      mv_model_fetch and nothing of the core; the unit tests
#                      link it.
#   mv_enhance         the add-on: ONE shared library whose only export is
#                      mv_addon_get, reached through the host function table
#                      (v3: screens and slots). Built beside the app into
#                      build/addons/enhance, never installed with it:
#                      tools/package/addon-pack.py packs and signs it.
#
# Expects: mv_project_options, mv_model_fetch (cmake/ai.cmake), Threads.

set(MV_ENHANCE_VERSION "1.0.0" CACHE STRING "Enhance add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_E "${MV_SOURCE_ROOT}/src/addons/enhance")

# The local check's OS facts and the one-process lock, one TU per OS (D9).
if(WIN32)
  set(_enhance_port ${_E}/machine_win.cpp ${_E}/lock_win.cpp)
elseif(APPLE)
  set(_enhance_port ${_E}/machine_mac.cpp ${_E}/lock_posix.cpp)
else()
  set(_enhance_port ${_E}/machine_posix.cpp ${_E}/lock_posix.cpp)
endif()

add_library(mv_enhance_engine STATIC
  ${_E}/catalog.cpp ${_E}/catalog.h
  ${_E}/machine.cpp ${_E}/machine.h
  ${_E}/choose.cpp ${_E}/choose.h
  ${_E}/lock.h
  ${_E}/downloads.cpp ${_E}/downloads.h
  ${_E}/view.cpp ${_E}/view.h
  ${_enhance_port}
)
target_include_directories(mv_enhance_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
find_package(Threads REQUIRED)
target_link_libraries(mv_enhance_engine PUBLIC mv_project_options mv_model_fetch Threads::Threads)
set_target_properties(mv_enhance_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # machine_win.cpp: the adapters and their dedicated memory (DXGI).
  target_link_libraries(mv_enhance_engine PUBLIC dxgi)
elseif(APPLE)
  set_target_properties(mv_enhance_engine PROPERTIES OSX_ARCHITECTURES "arm64")
endif()

# ---- the add-on module ------------------------------------------------------------
# mv_enhance: the host table and the Models screen (addon_entry.cpp) over the
# engine, and the folder it was loaded from (self_dir_<os>.cpp), where its
# catalogue sits. Built beside the app, never installed with it; packing
# stages the catalogue (tools/package/enhance-models.py stage).
if(WIN32)
  set(_enhance_self ${_E}/self_dir_win.cpp)
else()
  set(_enhance_self ${_E}/self_dir_posix.cpp)
endif()
add_library(mv_enhance MODULE
  ${_E}/addon_entry.cpp
  ${_E}/self_dir.h
  ${_enhance_self}
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
# One folder for every configuration (MSVC would add Release/), as mv_ai.
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${_cfg} _CFG)
  set_target_properties(mv_enhance PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/enhance"
    RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/enhance")
endforeach()
if(APPLE)
  set_target_properties(mv_enhance PROPERTIES PREFIX "lib" SUFFIX ".dylib" OSX_ARCHITECTURES "arm64")
elseif(WIN32)
  set_target_properties(mv_enhance PROPERTIES PREFIX "")
endif()

# The engine's unit tests (tests/test_enhance.cpp: the catalogue, the rule on
# several machine classes, the store against a fake transport, the Models
# tree through the screen vocabulary) and the add-on itself through the real
# loader (tests/test_enhance_addon.cpp: a signed store, the fixture
# catalogue, the v3 host table). Called by each build's mv_import_tests
# block, which links mv_addon (screen.cpp, the loader) and libsodium.
function(mv_enhance_addon_tests target)
  target_sources(${target} PRIVATE
    "${MV_SOURCE_ROOT}/tests/test_enhance.cpp"
    "${MV_SOURCE_ROOT}/tests/test_enhance_addon.cpp")
  target_link_libraries(${target} PRIVATE mv_enhance_engine)
  target_compile_definitions(${target} PRIVATE
    MV_ENHANCE_TEST_DATA="${MV_SOURCE_ROOT}/tests/data/enhance"
    MV_ENHANCE_MODULE_PATH="$<TARGET_FILE:mv_enhance>"
    MV_ENHANCE_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/enhance/manifest.json")
  add_dependencies(${target} mv_enhance)
endfunction()
