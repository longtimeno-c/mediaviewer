# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The AI agents add-on (docs/design/29): `MediaViewer --mcp`, an MCP server over
# stdio that edits the Final Cut Pro project dragged onto the panel (or an
# FCPXML file), scoped to the timeline's media and the folders the editor
# gave. Included by the Windows root and cmake/darwin.cmake after
# timeline.cmake, on every architecture.
#
#   mv_agent_engine  the server, its tools, the scope and the transcript
#                    layout over mv_nle_timeline. Portable; the tests drive it
#                    with a fake environment.
#   mv_agent         the add-on: ONE shared library exporting mv_addon_get; its
#                    mv.agent.1 interface is what `MediaViewer --mcp` calls, its
#                    mv.slots.1 the AI agents window. It links nothing of the
#                    core: the host reaches it through the host function table.
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/agent, signed.
#
# Expects: mv_project_options, mv_nle_timeline.

set(MV_AGENT_VERSION "1.0.0" CACHE STRING "AI agents add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_A "${MV_SOURCE_ROOT}/src/addons/agent")

add_library(mv_agent_engine STATIC
  ${_A}/environment.h
  ${_A}/scope.cpp ${_A}/scope.h
  ${_A}/transcript.cpp ${_A}/transcript.h
  ${_A}/server.cpp ${_A}/server.h
  ${_A}/install.cpp ${_A}/install.h
)
target_include_directories(mv_agent_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_agent_engine PUBLIC mv_nle_timeline mv_project_options)
set_target_properties(mv_agent_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  set(_agent_platform ${_A}/platform_win.cpp)
else()
  set(_agent_platform ${_A}/platform_posix.cpp)
endif()
add_library(mv_agent MODULE
  ${_A}/addon_entry.cpp
  ${_A}/platform.h
  ${_agent_platform}
)
target_link_libraries(mv_agent PRIVATE mv_agent_engine)
target_compile_definitions(mv_agent PRIVATE MV_AGENT_VERSION="${MV_AGENT_VERSION}")
if(APPLE)
  # The FCP panel's app group (cmake/darwin-fcp.cmake names it the same way):
  # its container's Timeline/ is what the panel shares with the agent.
  if(MV_FCP_TEAM_ID)
    set(_agent_group "${MV_FCP_TEAM_ID}.${MV_MAC_BUNDLE_ID}.fcp")
  else()
    set(_agent_group "dev.mediaviewer.fcp")
  endif()
  target_compile_definitions(mv_agent PRIVATE MV_AGENT_BRIDGE_GROUP="${_agent_group}")
elseif(WIN32)
  target_link_libraries(mv_agent PRIVATE shell32 ole32)
endif()
set_target_properties(mv_agent PROPERTIES
  OUTPUT_NAME "mv_agent"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/agent"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/agent")
# The skills (SKILL.md folders) beside the library: the package carries them,
# the server serves them as prompts, the AI agents window copies them to Claude Code.
add_custom_command(TARGET mv_agent POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${_A}/skills" "$<TARGET_FILE_DIR:mv_agent>/skills")
if(APPLE)
  set_target_properties(mv_agent PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_agent PROPERTIES PREFIX "")
endif()

# The add-on through the real loader (tests/test_agent_addon.cpp): its manifest,
# its window, and an MCP session over pipes. Called by each build's
# mv_import_tests block, which links the add-on host and libsodium.
function(mv_agent_addon_tests target)
  target_sources(${target} PRIVATE "${MV_SOURCE_ROOT}/tests/test_agent_addon.cpp")
  target_compile_definitions(${target} PRIVATE
    MV_AGENT_MODULE_PATH="$<TARGET_FILE:mv_agent>"
    MV_AGENT_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/agent/manifest.json"
    MV_TIMELINE_FIXTURES="${MV_SOURCE_ROOT}/tests/data/fcpxml")
  add_dependencies(${target} mv_agent)
endfunction()
