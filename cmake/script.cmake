# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/design/25 §9 A (PR 57): the script runtime for add-ons from other makers.
# Included by the Windows root, cmake/darwin.cmake and cmake/portable after
# cmake/import.cmake (it needs mv_addon), so the three cannot drift.
#
# mv_lua    Lua 5.4.8, vendored (third_party/lua/VENDOR.txt), static, MIT. Built
#           as C++ so a Lua error is a C++ exception and the runtime's RAII
#           holds across it. Not our code: the project's warnings are off for it.
#           Without the io / os / package / debug libraries and the stand-alone
#           tools: they are not in the binary at all.
# mv_script the sandbox, its limits and watchdog, and the host's set of script
#           add-ons (src/addon/script_*).

if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_lua "${MV_SOURCE_ROOT}/third_party/lua/src")

set(MV_LUA_SOURCES
  ${_lua}/lapi.c ${_lua}/lauxlib.c ${_lua}/lbaselib.c ${_lua}/lcode.c
  ${_lua}/lcorolib.c ${_lua}/lctype.c ${_lua}/ldebug.c ${_lua}/ldo.c
  ${_lua}/ldump.c ${_lua}/lfunc.c ${_lua}/lgc.c ${_lua}/llex.c
  ${_lua}/lmathlib.c ${_lua}/lmem.c ${_lua}/lobject.c ${_lua}/lopcodes.c
  ${_lua}/lparser.c ${_lua}/lstate.c ${_lua}/lstring.c ${_lua}/lstrlib.c
  ${_lua}/ltable.c ${_lua}/ltablib.c ${_lua}/ltm.c ${_lua}/lundump.c
  ${_lua}/lutf8lib.c ${_lua}/lvm.c ${_lua}/lzio.c)
set_source_files_properties(${MV_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(mv_lua STATIC ${MV_LUA_SOURCES})
target_include_directories(mv_lua SYSTEM PUBLIC "${_lua}")
set_target_properties(mv_lua PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE CXX)
if(MSVC)
  target_compile_options(mv_lua PRIVATE /W0 /EHsc /TP)
  target_compile_definitions(mv_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(mv_lua PRIVATE -w -x c++)
endif()
if(MV_ASAN)
  if(MSVC)
    target_compile_options(mv_lua PRIVATE /fsanitize=address)
  else()
    target_compile_options(mv_lua PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endif()

if(WIN32)
  set(MV_SCRIPT_THREAD ${MV_SOURCE_ROOT}/src/addon/script_thread_win.cpp)
else()
  set(MV_SCRIPT_THREAD ${MV_SOURCE_ROOT}/src/addon/script_thread_posix.cpp)
endif()
add_library(mv_script STATIC
  ${MV_SOURCE_ROOT}/src/addon/script_runtime.cpp
  ${MV_SOURCE_ROOT}/src/addon/script_runtime.h
  ${MV_SOURCE_ROOT}/src/addon/script_host.cpp
  ${MV_SOURCE_ROOT}/src/addon/script_host.h
  ${MV_SOURCE_ROOT}/src/addon/script_thread.h
  ${MV_SCRIPT_THREAD}
)
target_include_directories(mv_script PUBLIC "${MV_SOURCE_ROOT}/src")
find_package(Threads REQUIRED)
target_link_libraries(mv_script PUBLIC mv_addon PRIVATE mv_lua mv_project_options Threads::Threads)
set_target_properties(mv_script PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(mv::script ALIAS mv_script)
