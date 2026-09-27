# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# clang-cl shim for find_dependency(OpenMP) inside librawConfig.cmake.
# CMake's own FindOpenMP probes /openmp, which makes clang-cl link libomp.lib
# (not shipped with VS). vcpkg builds LibRaw against MSVC's vcomp, and
# cmake/raw-openmp.cmake already defines OpenMP::OpenMP_CXX for it, so just
# report success. Only put on CMAKE_MODULE_PATH under clang-cl.
set(OpenMP_FOUND TRUE)
set(OpenMP_C_FOUND TRUE)
set(OpenMP_CXX_FOUND TRUE)
if(NOT TARGET OpenMP::OpenMP_CXX)
  message(FATAL_ERROR "OpenMP shim: include(cmake/raw-openmp.cmake) first")
endif()
