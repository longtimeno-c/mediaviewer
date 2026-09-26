# SPDX-License-Identifier: GPL-2.0-or-later
# encprobe — plan/20 spike S1. An opt-in tool, deliberately not part of the app
# build: point it at the vcpkg tree the app was configured with.
#
#   cmake -S tools/encprobe -B build-encprobe -G Ninja \
#         -DMV_FFMPEG_PREFIX=<vcpkg_installed>/<triplet>
#   cmake --build build-encprobe && build-encprobe/encprobe > encprobe.md
cmake_minimum_required(VERSION 3.28)
project(encprobe CXX)

set(MV_FFMPEG_PREFIX "" CACHE PATH "vcpkg installed/<triplet> holding the app's FFmpeg")
if(NOT MV_FFMPEG_PREFIX)
  message(FATAL_ERROR "Set MV_FFMPEG_PREFIX to the app's vcpkg installed/<triplet> folder")
endif()

set(MV_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
if(WIN32)
  set(port ${MV_SRC}/edit/hwencode_win.cpp)
elseif(APPLE)
  set(port ${MV_SRC}/edit/hwencode_mac.cpp)
else()
  set(port ${MV_SRC}/edit/hwencode_none.cpp)
endif()

add_executable(encprobe main.cpp ${port})
target_compile_features(encprobe PRIVATE cxx_std_20)
target_include_directories(encprobe PRIVATE ${MV_SRC})
target_include_directories(encprobe SYSTEM PRIVATE ${MV_FFMPEG_PREFIX}/include)
foreach(lib avcodec avfilter avutil)
  find_library(MV_${lib} ${lib} PATHS ${MV_FFMPEG_PREFIX}/lib NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(encprobe PRIVATE ${MV_${lib}})
endforeach()
if(APPLE)
  set_target_properties(encprobe PROPERTIES BUILD_RPATH ${MV_FFMPEG_PREFIX}/lib)
endif()
