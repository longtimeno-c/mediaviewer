# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone H (docs/design/17 "The AI pack"): the AI pack's Windows chrome. Included
# from the root CMakeLists.txt after cmake/ai.cmake (which defines mv_ai).
# Like mv_import_chrome, the chrome is published into the add-on's own folder
# (build/addons/ai, beside mv_ai.dll), never beside the app, so the base
# install tree stays byte-identical with the pack absent (docs/design/17 PR 20
# verify). tools/package/addon-pack.py packs that folder, signed.
#
# The work is deferred to the end of the top directory: this file is included
# before mediaviewer_lab exists, and the base chrome's new sources have to be
# on the lab's LINK_DEPENDS like the root's MV_CHROME_SOURCES.

if(NOT WIN32)
  return()
endif()

function(mv_ai_chrome_win)
  if(NOT DOTNET_EXE)
    find_program(DOTNET_EXE dotnet)
  endif()
  if(NOT DOTNET_EXE)
    return()
  endif()

  # The base chrome's Milestone H sources (the Settings section, the pill, the
  # scrub-bar dots, the interop mirror of mv.ai.1): republish beside the lab
  # when they change.
  if(TARGET mediaviewer_lab)
    set_property(TARGET mediaviewer_lab APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.LocalSearch.cs"
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Clip.cs"
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Chrome/IslandHost.Theme.cs"
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Interop/Ai.cs"
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Interop/MediaViewerSession.cs"
      "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Interop/NativeMethods.cs")
  endif()

  if(TARGET mv_ai)
    set(dir "${CMAKE_BINARY_DIR}/addons/ai")
    add_custom_target(mv_ai_chrome ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
      COMMAND ${DOTNET_EXE} publish
              "${CMAKE_SOURCE_DIR}/src.managed/MediaViewer.Ai.Chrome/MediaViewer.Ai.Chrome.csproj"
              --nologo --configuration $<IF:$<CONFIG:Debug>,Debug,Release>
              -r win-x64 --no-self-contained -p:Platform=x64
              -o "${dir}"
      COMMENT "Publishing MediaViewer.Ai.Chrome into addons/ai"
      VERBATIM)
    add_dependencies(mv_ai_chrome mv_ai)
  endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL mv_ai_chrome_win)
