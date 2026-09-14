# SPDX-License-Identifier: GPL-2.0-or-later
#
# libFuzzer harnesses, one per decoder entry point (PR 7, plan/09: "libFuzzer
# harness per decoder against the corpus, run in CI nightly").
#
#   cmake -S . -B build-fuzz -A x64 -T ClangCL -DMV_FUZZ=ON -DMV_BUILD_TESTS=OFF
#   cmake --build build-fuzz --config Release --target mv_fuzzers
#   ./tools/fuzz/run.ps1 -BuildDir build-fuzz -Seconds 60
#
# Instrumentation: every mv_* library (they all link mv_project_options) is
# compiled with -fsanitize=fuzzer-no-link and ASan, so coverage reaches our
# decoder glue. vcpkg dependencies (libjpeg-turbo, libspng, giflib, libwebp,
# libtiff, libheif, libavif, LibRaw) are prebuilt DLLs: ASan still catches our
# out-of-bounds use of their buffers through interceptors, but coverage inside
# them is not instrumented. That is the honest scope of these harnesses — they
# fuzz our code and how we drive the libraries, not the libraries' internals
# (upstream OSS-Fuzz does that).
#
# Only the mv_fuzzers target is expected to link in this configuration; the
# app, the lab and mv_tests are not linked against the sanitizer runtimes.

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
  message(FATAL_ERROR "MV_FUZZ needs clang-cl: configure with -T ClangCL (VS generator) "
                      "or -DCMAKE_CXX_COMPILER=clang-cl (Ninja).")
endif()

# The libFuzzer / ASan runtimes ship beside clang-cl:
#   <llvm>/bin/clang-cl.exe  ->  <llvm>/lib/clang/<major>/lib/windows
get_filename_component(_mv_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
string(REGEX MATCH "^[0-9]+" _mv_clang_major "${CMAKE_CXX_COMPILER_VERSION}")
find_path(MV_CLANG_RT_DIR clang_rt.fuzzer-x86_64.lib
  PATHS "${_mv_llvm_bin}/../lib/clang/${_mv_clang_major}/lib/windows"
        "${_mv_llvm_bin}/../lib/clang/${CMAKE_CXX_COMPILER_VERSION}/lib/windows"
  NO_DEFAULT_PATH)
if(NOT MV_CLANG_RT_DIR)
  message(FATAL_ERROR "clang_rt.fuzzer-x86_64.lib not found under ${_mv_llvm_bin}/../lib/clang. "
                      "Install the VS 'C++ Clang tools for Windows' component (LLVM 17+).")
endif()
message(STATUS "MV_FUZZ: clang runtime ${MV_CLANG_RT_DIR}")

# Current GitHub Windows images ship libFuzzer but may omit clang's static ASan
# archives. Detect them as a pair: fuzzing remains useful without them, while
# the separate MSVC-ASan CI leg still provides sanitizer coverage.
find_file(MV_CLANG_ASAN_RUNTIME clang_rt.asan-x86_64.lib
  PATHS "${MV_CLANG_RT_DIR}" NO_DEFAULT_PATH)
find_file(MV_CLANG_ASAN_CXX_RUNTIME clang_rt.asan_cxx-x86_64.lib
  PATHS "${MV_CLANG_RT_DIR}" NO_DEFAULT_PATH)
if(MV_CLANG_ASAN_RUNTIME AND MV_CLANG_ASAN_CXX_RUNTIME)
  set(MV_FUZZ_ASAN ON)
  message(STATUS "MV_FUZZ: static clang ASan enabled")
else()
  set(MV_FUZZ_ASAN OFF)
  message(WARNING "MV_FUZZ: static clang ASan runtimes unavailable; "
                  "building libFuzzer coverage without ASan")
endif()

# Instrument everything that links mv_project_options.
#
# CRT: the libFuzzer library Visual Studio ships (clang_rt.fuzzer-x86_64.lib)
# is built /MT, and lld-link refuses to mix it with /MD objects
# (/failifmismatch RuntimeLibrary). So the instrumented mv_* libraries and the
# harnesses use the static CRT and the static ASan runtime, while vcpkg's
# dependencies stay the /MD DLLs they are. That is sound only because no mv_*
# code frees memory a dependency allocated (or vice versa) — each library
# frees its own through its own API. Keep it that way in decoder code.
set(MV_FUZZ_INSTRUMENTED mv_core mv_codec mv_image mv_gfx mv_io)
set_target_properties(${MV_FUZZ_INSTRUMENTED} PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(mv_project_options INTERFACE
  -fsanitize=fuzzer-no-link
  /Zi
  -Wno-unused-command-line-argument)
target_compile_definitions(mv_project_options INTERFACE MV_FUZZING=1)
if(MV_FUZZ_ASAN)
  target_compile_options(mv_project_options INTERFACE /fsanitize=address)
  target_compile_definitions(mv_project_options INTERFACE
    # Same ABI reason as MV_ASAN: prebuilt C++ deps lack STL annotations.
    _DISABLE_VECTOR_ANNOTATION
    _DISABLE_STRING_ANNOTATION
    _DISABLE_OPTIONAL_ANNOTATION)
endif()

set(MV_FUZZ_HARNESSES
  jpeg png bmp gif webp tiff ico heic avif raw raw_preview decode animation)

add_custom_target(mv_fuzzers)

foreach(name IN LISTS MV_FUZZ_HARNESSES)
  set(target fuzz_${name})
  add_executable(${target} fuzz_${name}.cpp fuzz_common.h)
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE mv_codec)
  if(name STREQUAL "decode")
    target_link_libraries(${target} PRIVATE mv_image)
  endif()
  # The linker is not the clang driver, so name the runtimes the driver would
  # for -fsanitize=fuzzer,address with /MT: libFuzzer (with its main) and the
  # static ASan runtime, whole-archived so its interceptors are all present.
  target_link_directories(${target} PRIVATE "${MV_CLANG_RT_DIR}")
  target_link_libraries(${target} PRIVATE clang_rt.fuzzer-x86_64.lib)
  target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
  if(MV_FUZZ_ASAN)
    target_link_options(${target} PRIVATE
      "/wholearchive:${MV_CLANG_ASAN_RUNTIME}"
      "/wholearchive:${MV_CLANG_ASAN_CXX_RUNTIME}")
  endif()
  set_target_properties(${target} PROPERTIES
    FOLDER fuzz
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_dependencies(mv_fuzzers ${target})
endforeach()
