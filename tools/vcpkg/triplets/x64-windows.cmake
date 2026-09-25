# The stock x64-windows triplet, minus compiler tracking.
#
# GitHub's windows-latest pool mixes image versions with different MSVC
# builds. With tracking on, a job whose MSVC differs from the one that filled
# the shared dependency cache gets different package ABIs and rebuilds every
# port (FFmpeg included) inside Configure, which blows the 40-minute job
# limit. Ports are pinned by the vcpkg baseline and the cache key; the
# compiler patch level is not part of what we ship.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_DISABLE_COMPILER_TRACKING ON)
