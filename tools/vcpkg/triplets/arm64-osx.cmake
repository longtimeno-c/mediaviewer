# The stock arm64-osx triplet, pinned to the app's deployment target.
#
# Without it vcpkg builds for the runner SDK's default (macOS 15 on the
# macos-15 images) and every static library warns "built for newer 'macOS'
# version (15.0) than being linked (14.0)". Keep in step with
# CMAKE_OSX_DEPLOYMENT_TARGET in cmake/darwin.cmake.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
