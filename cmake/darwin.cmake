# SPDX-License-Identifier: GPL-2.0-or-later
#
# Darwin / Apple Silicon present lab (PR 16). Included from the root
# CMakeLists.txt and then returns, so none of the Windows targets are defined.
#
# plan/15: AppKit + CAMetalLayer + CAMetalDisplayLink. No SwiftUI, no
# VideoToolbox, no FFmpeg on this slice.

enable_language(OBJCXX)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  message(FATAL_ERROR
    "MediaViewer on macOS is Apple Silicon only (plan/15, D9). "
    "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-rtti>
  $<$<COMPILE_LANGUAGE:OBJCXX>:-Wall -Wextra -Werror -fno-rtti -fobjc-arc>
)
target_compile_definitions(mv_project_options INTERFACE MV_DARWIN=1)

add_library(mv_core STATIC
  src/core/job_system_posix.cpp
  src/core/trace_posix.cpp
  src/core/job_system.h
  src/core/result.h
  src/core/spsc_ring.h
  src/core/status.h
  src/core/trace.h
)
target_include_directories(mv_core PUBLIC src)
target_link_libraries(mv_core PUBLIC mv_project_options)
add_library(mv::core ALIAS mv_core)

add_library(mv_gfx STATIC
  src/gfx/metal_pacer.cpp
  src/gfx/device_mac.mm
  src/gfx/metal_layer.mm
  src/gfx/metal_pacer.h
  src/gfx/present_policy.h
  src/gfx/pace_json.h
  src/gfx/device_mac.h
  src/gfx/metal_layer.h
)
target_link_libraries(mv_gfx PUBLIC mv_core)
target_link_libraries(mv_gfx PRIVATE
  "-framework Foundation"
  "-framework AppKit"
  "-framework Metal"
  "-framework QuartzCore")
add_library(mv::gfx ALIAS mv_gfx)

find_package(imgui CONFIG REQUIRED)

add_executable(mediaviewer_lab
  src/shell/main_mac.mm
  src/shell/present_lab_mac.mm
  src/shell/present_lab_mac.h
  src/shell/input_state.h
)
target_link_libraries(mediaviewer_lab PRIVATE
  mv_gfx
  imgui::imgui
  "-framework Foundation"
  "-framework AppKit"
  "-framework Metal"
  "-framework QuartzCore")
target_include_directories(mediaviewer_lab PRIVATE src)
set_source_files_properties(
  src/shell/main_mac.mm
  src/shell/present_lab_mac.mm
  PROPERTIES COMPILE_FLAGS "-fobjc-arc")

add_custom_command(TARGET mediaviewer_lab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
          "$<TARGET_FILE_DIR:mediaviewer_lab>/CozetteVector.ttf"
  COMMENT "Copy CozetteVector.ttf beside mediaviewer_lab")

add_executable(mv_frametime
  tools/frametime/main_mac.cpp
)
target_link_libraries(mv_frametime PRIVATE mv_core)
target_include_directories(mv_frametime PRIVATE src tools)
set_target_properties(mv_frametime PROPERTIES OUTPUT_NAME "frametime")
add_dependencies(mv_frametime mediaviewer_lab)

if(MV_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(mv_tests
    tests/test_result.cpp
    tests/test_spsc_ring.cpp
    tests/test_job_system.cpp
    tests/test_input_state.cpp
    tests/test_present_policy.cpp
    tests/test_metal_pacer.cpp
    tests/test_frametime_report.cpp
  )
  target_link_libraries(mv_tests PRIVATE
    mv_core
    mv_gfx
    Catch2::Catch2WithMain)
  target_include_directories(mv_tests PRIVATE src tools)
  include(Catch)
  catch_discover_tests(mv_tests)
endif()

message(STATUS "MediaViewer ${PROJECT_VERSION} — Darwin present lab (PR 16), GPL-2.0-or-later")
