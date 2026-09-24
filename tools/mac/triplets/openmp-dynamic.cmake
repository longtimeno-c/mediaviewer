set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
if(PORT STREQUAL "libraw")
  execute_process(COMMAND brew --prefix libomp OUTPUT_VARIABLE MV_LIBOMP_ROOT
                  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
       "-DOpenMP_ROOT=${MV_LIBOMP_ROOT}"
       "-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp"
       "-DOpenMP_CXX_LIB_NAMES=omp"
       "-DOpenMP_CXX_INCLUDE_DIR=${MV_LIBOMP_ROOT}/include"
       "-DOpenMP_omp_LIBRARY=${MV_LIBOMP_ROOT}/lib/libomp.dylib")
endif()
