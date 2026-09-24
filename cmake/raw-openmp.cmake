# LibRaw is built by vcpkg with OpenMP. Use the same runtime to bound its
# worker teams. On Windows vcpkg builds LibRaw with MSVC even in clang-cl CI;
# linking LLVM's runtime there would control a different thread pool.
add_library(mv_raw_openmp INTERFACE)
if(WIN32)
  set(MV_VCOMP_LIBRARY "$<IF:$<CONFIG:Debug>,vcompd,vcomp>")
  target_link_libraries(mv_raw_openmp INTERFACE "${MV_VCOMP_LIBRARY}")
  # LibRaw also exports this transitive target. Keep its runtime consistent
  # under clang-cl; our sources call the API but contain no OpenMP regions.
  if(NOT TARGET OpenMP::OpenMP_CXX)
    add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
    set_target_properties(OpenMP::OpenMP_CXX PROPERTIES
                          INTERFACE_LINK_LIBRARIES "${MV_VCOMP_LIBRARY}")
  endif()
  # A developer machine has this in System32; a clean machine may not.
  # Package the redistributable alongside the codecs (never copy System32).
  set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
  set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  set(MV_OPENMP_REDIST ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
  list(FILTER MV_OPENMP_REDIST INCLUDE REGEX "[Vv][Cc][Oo][Mm][Pp]140\\.dll$")
  if(NOT MV_OPENMP_REDIST)
    message(FATAL_ERROR "MSVC OpenMP redistributable missing; install the C++ toolset redist.")
  endif()
  foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    file(COPY ${MV_OPENMP_REDIST} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${cfg}")
  endforeach()
  set(MV_OPENMP_DEBUG ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
  list(FILTER MV_OPENMP_DEBUG INCLUDE REGEX "[Vv][Cc][Oo][Mm][Pp]140[dD]\\.dll$")
  if(MV_OPENMP_DEBUG)
    file(COPY ${MV_OPENMP_DEBUG} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug")
  endif()
else()
  if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    execute_process(COMMAND brew --prefix libomp OUTPUT_VARIABLE MV_LIBOMP_ROOT
                    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp")
    set(OpenMP_CXX_LIB_NAMES omp)
    set(OpenMP_CXX_INCLUDE_DIR "${MV_LIBOMP_ROOT}/include")
    set(OpenMP_omp_LIBRARY "${MV_LIBOMP_ROOT}/lib/libomp.dylib")
  endif()
  find_package(OpenMP REQUIRED COMPONENTS CXX)
  target_link_libraries(mv_raw_openmp INTERFACE OpenMP::OpenMP_CXX)
endif()
