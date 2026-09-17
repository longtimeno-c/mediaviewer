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

# Which compiler-rt archives a toolchain ships changes between LLVM releases,
# and the link below depends on it. Put the real list in the configure log so a
# link failure here is one read, not one CI round, away from an answer.
file(GLOB _mv_rt_archives RELATIVE "${MV_CLANG_RT_DIR}" "${MV_CLANG_RT_DIR}/clang_rt.*-x86_64.lib")
message(STATUS "MV_FUZZ: clang runtime ${MV_CLANG_RT_DIR}")
message(STATUS "MV_FUZZ: archives ${_mv_rt_archives}")

# Pick the ASan runtime the installed toolchain actually has, rather than
# hard-coding one.
#
# LLVM dropped the static Windows ASan runtime (clang_rt.asan-x86_64.lib) in
# favour of the DLL that MSVC's /fsanitize=address also uses, so a current
# Visual Studio clang has only clang_rt.asan_dynamic-x86_64.lib plus a thunk
# per CRT flavour. Naming the static archives unconditionally is what made
# every fuzz build fail at lld-link the day the runner image moved on.
#
# CRT: libFuzzer comes in a /MT build (clang_rt.fuzzer-x86_64.lib) and a /MD
# one (clang_rt.fuzzer_MD-x86_64.lib), and lld-link refuses to mix a /MT
# archive with /MD objects (/failifmismatch RuntimeLibrary). So the CRT is
# chosen with the runtime, not before it. Either way the instrumented mv_*
# libraries and the harnesses agree, while vcpkg's dependencies stay the /MD
# DLLs they are. That is sound only because no mv_* code frees memory a
# dependency allocated (or vice versa) — each library frees its own through
# its own API. Keep it that way in decoder code.
set(_mv_rt "${MV_CLANG_RT_DIR}")
if(EXISTS "${_mv_rt}/clang_rt.asan-x86_64.lib" AND EXISTS "${_mv_rt}/clang_rt.asan_cxx-x86_64.lib")
  set(MV_FUZZ_MODE "static ASan, /MT")
  set(MV_FUZZ_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(MV_FUZZ_RT_LIBS "${_mv_rt}/clang_rt.fuzzer-x86_64.lib")
  # Whole-archived so every interceptor is present, not only the ones a
  # referenced object happens to pull in.
  set(MV_FUZZ_RT_WHOLE "${_mv_rt}/clang_rt.asan-x86_64.lib"
                       "${_mv_rt}/clang_rt.asan_cxx-x86_64.lib")
elseif(EXISTS "${_mv_rt}/clang_rt.asan_dynamic-x86_64.lib"
       AND EXISTS "${_mv_rt}/clang_rt.asan_static_runtime_thunk-x86_64.lib")
  set(MV_FUZZ_MODE "dynamic ASan, /MT")
  set(MV_FUZZ_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(MV_FUZZ_RT_LIBS "${_mv_rt}/clang_rt.fuzzer-x86_64.lib"
                      "${_mv_rt}/clang_rt.asan_dynamic-x86_64.lib")
  set(MV_FUZZ_RT_WHOLE "${_mv_rt}/clang_rt.asan_static_runtime_thunk-x86_64.lib")
elseif(EXISTS "${_mv_rt}/clang_rt.asan_dynamic-x86_64.lib"
       AND EXISTS "${_mv_rt}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
       AND EXISTS "${_mv_rt}/clang_rt.fuzzer_MD-x86_64.lib")
  set(MV_FUZZ_MODE "dynamic ASan, /MD")
  set(MV_FUZZ_CRT "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
  set(MV_FUZZ_RT_LIBS "${_mv_rt}/clang_rt.fuzzer_MD-x86_64.lib"
                      "${_mv_rt}/clang_rt.asan_dynamic-x86_64.lib")
  set(MV_FUZZ_RT_WHOLE "${_mv_rt}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
else()
  message(FATAL_ERROR
    "MV_FUZZ: no linkable ASan runtime in ${MV_CLANG_RT_DIR}. Found: ${_mv_rt_archives}. "
    "Expected either clang_rt.asan-x86_64.lib + clang_rt.asan_cxx-x86_64.lib, or "
    "clang_rt.asan_dynamic-x86_64.lib with clang_rt.asan_static_runtime_thunk-x86_64.lib "
    "(/MT) or clang_rt.asan_dynamic_runtime_thunk-x86_64.lib + clang_rt.fuzzer_MD-x86_64.lib "
    "(/MD). Fuzzing without ASan is not the fallback: the harnesses exist to catch memory "
    "errors (plan/09).")
endif()
if(NOT MV_FUZZ_MODE MATCHES "^static"
   AND EXISTS "${_mv_rt}/clang_rt.asan_static-x86_64.lib")
  # The dynamic runtime has a static half: the interceptors clang inlines into
  # the instrumented objects resolve against it.
  list(APPEND MV_FUZZ_RT_LIBS "${_mv_rt}/clang_rt.asan_static-x86_64.lib")
endif()
message(STATUS "MV_FUZZ: ${MV_FUZZ_MODE}")

# The dynamic runtime needs its DLL beside the harness — the runtime directory
# is not on PATH on a CI runner.
if(NOT MV_FUZZ_MODE MATCHES "^static")
  find_file(MV_FUZZ_ASAN_DLL clang_rt.asan_dynamic-x86_64.dll
    PATHS "${MV_CLANG_RT_DIR}" "${_mv_llvm_bin}" "$ENV{VCToolsInstallDir}/bin/Hostx64/x64"
    NO_DEFAULT_PATH)
  if(MV_FUZZ_ASAN_DLL)
    message(STATUS "MV_FUZZ: ASan runtime DLL ${MV_FUZZ_ASAN_DLL}")
  else()
    message(WARNING "MV_FUZZ: clang_rt.asan_dynamic-x86_64.dll not found next to the runtime "
                    "archives. The harnesses will not start unless it is on PATH.")
  endif()
endif()

# Instrument everything that links mv_project_options.
set(MV_FUZZ_INSTRUMENTED mv_core mv_codec mv_image mv_gfx mv_io)
set_target_properties(${MV_FUZZ_INSTRUMENTED} PROPERTIES
  MSVC_RUNTIME_LIBRARY "${MV_FUZZ_CRT}")
target_compile_options(mv_project_options INTERFACE
  -fsanitize=fuzzer-no-link
  /fsanitize=address
  /Zi
  -Wno-unused-command-line-argument)
target_compile_definitions(mv_project_options INTERFACE
  MV_FUZZING=1
  # Same ABI reason as MV_ASAN: prebuilt C++ deps lack the STL annotations.
  _DISABLE_VECTOR_ANNOTATION
  _DISABLE_STRING_ANNOTATION
  _DISABLE_OPTIONAL_ANNOTATION)

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
  # for -fsanitize=fuzzer,address: libFuzzer (with its main) and the ASan
  # runtime selected above.
  target_link_directories(${target} PRIVATE "${MV_CLANG_RT_DIR}")
  target_link_libraries(${target} PRIVATE ${MV_FUZZ_RT_LIBS})
  target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
  foreach(archive IN LISTS MV_FUZZ_RT_WHOLE)
    target_link_options(${target} PRIVATE "/wholearchive:${archive}")
  endforeach()
  set_target_properties(${target} PROPERTIES
    FOLDER fuzz
    MSVC_RUNTIME_LIBRARY "${MV_FUZZ_CRT}")
  if(MV_FUZZ_ASAN_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${MV_FUZZ_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
  # Every DLL this harness actually imports, beside it. vcpkg's own applocal
  # deployment covers the common case, and for eleven of these harnesses it
  # evidently does; fuzz_decode and fuzz_animation — the two with the widest
  # import closure — die at 0xC0000142 (STATUS_DLL_INIT_FAILED) before
  # libFuzzer runs a unit, which is the shape of a dependency the loader could
  # not bring up. This is CMake's own answer to that question and it no-ops
  # for anything already deployed.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:${target}>" "$<TARGET_FILE_DIR:${target}>"
    COMMAND_EXPAND_LISTS)
  add_dependencies(mv_fuzzers ${target})
endforeach()
