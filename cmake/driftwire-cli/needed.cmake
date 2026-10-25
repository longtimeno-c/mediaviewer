# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The packaging gate: the driftwire program may need only the C library,
# libm and the dynamic loader. Everything else (libsodium 1.0.22, BLAKE3,
# FTXUI, libstdc++, libgcc) is linked in, so the .deb and the .rpm depend on
# glibc alone, and a distro's libsodium (1.0.18 cannot do ML-KEM-768) is never
# the one that runs. libsecret and libsystemd are dlopen()ed and so never
# NEEDED.
#
#   cmake -DPROGRAM=build-cli/bin/driftwire [-DREADELF=readelf] -P needed.cmake

if(NOT PROGRAM)
  message(FATAL_ERROR "needed.cmake: pass -DPROGRAM=<the driftwire program>")
endif()
if(NOT READELF)
  find_program(READELF NAMES readelf REQUIRED)
endif()
execute_process(COMMAND "${READELF}" -d "${PROGRAM}"
  OUTPUT_VARIABLE _dynamic RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "readelf -d ${PROGRAM} failed: ${_err}")
endif()

string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" _lines "${_dynamic}")
set(_bad "")
set(_all "")
foreach(_line IN LISTS _lines)
  string(REGEX REPLACE ".*\\[([^]]+)\\].*" "\\1" _lib "${_line}")
  list(APPEND _all "${_lib}")
  if(NOT _lib MATCHES "^(libc\\.so\\.6|libm\\.so\\.6|ld-linux[-a-z0-9_]*\\.so\\.[0-9]+)$")
    list(APPEND _bad "${_lib}")
  endif()
endforeach()
if(NOT _all)
  message(FATAL_ERROR "readelf found no NEEDED entry in ${PROGRAM}: is it a dynamic ELF program?")
endif()
string(REPLACE ";" ", " _all_text "${_all}")
if(_bad)
  string(REPLACE ";" ", " _bad_text "${_bad}")
  message(FATAL_ERROR "driftwire needs ${_bad_text} (only libc, libm and the loader are allowed: "
                      "link the rest statically). NEEDED: ${_all_text}")
endif()
message(STATUS "driftwire NEEDED: ${_all_text}")
