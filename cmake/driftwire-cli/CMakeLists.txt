# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The driftwire command line's Linux packages (docs/design/28 "From a
# terminal"): one program, /usr/bin/driftwire, as a .deb, an .rpm and a
# tarball. A project of its own, not cmake/portable: that build carries the
# Import engine, the AI pack's x64-only downloads and ASan, none of which a
# package wants. It includes cmake/driftwire.cmake, as the three app builds do,
# so the program is the one they build and test.
#
#   cmake -S cmake/driftwire-cli -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
#     -DVCPKG_MANIFEST_DIR=tools/portable
#   cmake --build build-cli
#   cmake --build build-cli --target driftwire_needed   # the NEEDED gate
#   (cd build-cli && cpack -G "DEB;RPM;TGZ")
#
# Everything but the C library is linked in: libsodium (1.0.22 for ML-KEM-768;
# a distro's 1.0.18 cannot build it), BLAKE3 and FTXUI from vcpkg's static
# x64-linux / arm64-linux triplets, and libstdc++ / libgcc (-static-libstdc++
# -static-libgcc, under the GCC Runtime Library Exception). libsecret and
# libsystemd are opened at run time when they are there (secrets_linux.cpp,
# power_linux.cpp), so the packages only recommend libsecret. The gate
# (driftwire_needed) fails the build if anything but libc, libm or the dynamic
# loader is NEEDED.
#
# -DMV_DRIFTWIRE_CLI_TESTS=ON adds mv_driftwire_cli_tests (Catch2, the "[cli]"
# suites; never installed) and the CLI's two test programs.

cmake_minimum_required(VERSION 3.28)

get_filename_component(MV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The version is the app's (the root project's), so one release tag names both.
file(READ "${MV_ROOT}/CMakeLists.txt" _mv_root_cmake)
string(REGEX MATCH "project\\(mediaviewer[^)]*VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _mv_match "${_mv_root_cmake}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "No project(mediaviewer VERSION x.y.z) in ${MV_ROOT}/CMakeLists.txt")
endif()
set(_mv_version "${CMAKE_MATCH_1}")
unset(_mv_root_cmake)
unset(_mv_match)

project(driftwire
  VERSION ${_mv_version}
  DESCRIPTION "Send files to a computer nearby, or anywhere with a code"
  HOMEPAGE_URL "https://github.com/longtimeno-c/mediaviewer"
  LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "cmake/driftwire-cli packages the Linux program; Windows and macOS build "
                      "driftwire in their own app builds (cmake/driftwire.cmake)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MV_DRIFTWIRE_CLI_TESTS "Build the command line's tests (never installed)" OFF)

# GCC only on Linux: clang with libstdc++ rejects core/json.h's recursive
# vector (docs/design/09).
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(WARNING "The driftwire packages are built and tested with GCC; ${CMAKE_CXX_COMPILER_ID} is not")
endif()

# ---- hardening: every target of this project (the engine and the command
# line's libraries are linked into the program); vcpkg builds its ports with
# its own flags ------------------------------------------------------------
include(CheckPIESupported)
check_pie_supported(LANGUAGES C CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-fstack-protector-strong -fstack-clash-protection
  # Ubuntu's GCC fortifies by itself and Fedora's flags at 3: one level,
  # stated, and only with optimisation (glibc warns, -Werror fails, without).
  "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>"
  "$<$<CONFIG:Debug>:-D_GLIBCXX_ASSERTIONS>")
# Control-flow protection: CET on x86-64, PAC and BTI on arm64 (both what
# Debian's and Fedora's own flags use).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_compile_options(-fcf-protection)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  add_compile_options(-mbranch-protection=standard)
endif()
add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,--as-needed)

add_library(mv_project_options INTERFACE)
target_compile_options(mv_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-rtti>
  # As cmake/portable: GCC's -O2 flow analysis reports std::optional members
  # read only behind has_value() as "may be used uninitialized".
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>
  # GCC 12's inlined std::string concatenation ("x" + str) reports a false
  # -Wrestrict overlap (GCC PR 105329, fixed in 13).
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,13>>:-Wno-restrict>)

# ---- dependencies (vcpkg's static ports; a static prefix on a bare machine) --
find_package(Threads REQUIRED)

find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  set(MV_SODIUM_TARGET unofficial-sodium::sodium)
else()
  # A libsodium built from source with --disable-shared, found by pkg-config
  # (PKG_CONFIG_PATH). A distro's libsodium.so is refused below.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(MV_SODIUM_TARGET PkgConfig::SODIUM)
endif()

find_package(blake3 CONFIG QUIET)
if(TARGET BLAKE3::blake3)
  set(MV_BLAKE3_TARGET BLAKE3::blake3)
else()
  message(FATAL_ERROR "BLAKE3 not found: install it (vcpkg `blake3`, tools/portable/vcpkg.json)")
endif()

# cmake/driftwire.cmake finds FTXUI (or MV_FTXUI_SOURCE_DIR) itself.
set(MV_SOURCE_ROOT "${MV_ROOT}")
set(MV_DRIFTWIRE_CLI_VERSION "${PROJECT_VERSION}")
include("${MV_ROOT}/cmake/driftwire.cmake")

# The add-on module, the relay tool and the gallery are the app builds'; here
# they are built only when asked for by name.
set_target_properties(mv_driftwire mv_driftwire_relay mv_driftwire_cli_gallery PROPERTIES EXCLUDE_FROM_ALL ON)
if(NOT MV_DRIFTWIRE_CLI_TESTS)
  set_target_properties(mv_driftwire_cli_test PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# The C++ runtime linked in, so one package runs on every glibc distribution
# from the build machine's glibc up, whatever its GCC.
target_link_options(mv_driftwire_cli PRIVATE -static-libstdc++ -static-libgcc)
target_link_options(mv_driftwire_cli_test PRIVATE -static-libstdc++ -static-libgcc)

# ---- the NEEDED gate ----------------------------------------------------------
# Only the C library, libm and the loader: a libsodium.so (a distro's, too old
# for protocol 2), libstdc++ or libgcc_s here would make the package depend on
# what a distribution happens to ship.
add_custom_target(driftwire_needed ALL
  COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:mv_driftwire_cli> -DREADELF=${CMAKE_READELF}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/needed.cmake"
  DEPENDS mv_driftwire_cli
  COMMENT "driftwire links only libc, libm and the loader"
  VERBATIM)

# ---- tests (CI's driftwire-cli-linux job) -------------------------------------
if(MV_DRIFTWIRE_CLI_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(mv_driftwire_cli_tests
    "${MV_ROOT}/tests/test_driftwire_cli_ui.cpp"
    "${MV_ROOT}/tests/test_driftwire_cli.cpp"
    "${MV_ROOT}/tests/test_driftwire_cli_e2e.cpp"
    "${MV_ROOT}/src/addons/driftwire/bt_none.cpp"
    "${MV_ROOT}/src/io/file_port.cpp"
    "${MV_ROOT}/src/io/content_hash.cpp"
    "${MV_ROOT}/src/io/file_port_mac.cpp")
  target_include_directories(mv_driftwire_cli_tests PRIVATE "${MV_ROOT}/src" "${MV_ROOT}/tests")
  target_link_libraries(mv_driftwire_cli_tests PRIVATE mv_driftwire_cli_core Catch2::Catch2WithMain)
  target_compile_definitions(mv_driftwire_cli_tests PRIVATE
    MV_DRIFTWIRE_CLI_PATH="$<TARGET_FILE:mv_driftwire_cli>"
    MV_DRIFTWIRE_CLI_TEST_PATH="$<TARGET_FILE:mv_driftwire_cli_test>")
  add_dependencies(mv_driftwire_cli_tests mv_driftwire_cli mv_driftwire_cli_test)
  add_test(NAME driftwire_cli COMMAND mv_driftwire_cli_tests "[cli]")
  set_tests_properties(driftwire_cli PROPERTIES ENVIRONMENT "TZ=UTC" TIMEOUT 900)
endif()

# ---- install ------------------------------------------------------------------
include(GNUInstallDirs)
set(_pkg "${MV_ROOT}/packaging/linux")

install(TARGETS mv_driftwire_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The manual page, compressed as both package formats expect (gzip -n: no
# name or time in the header, so the package is reproducible).
find_program(MV_GZIP gzip REQUIRED)
set(_man_gz "${CMAKE_BINARY_DIR}/driftwire.1.gz")
add_custom_command(OUTPUT "${_man_gz}"
  COMMAND ${MV_GZIP} -9 -n -c "${_pkg}/driftwire.1" > "${_man_gz}"
  DEPENDS "${_pkg}/driftwire.1"
  COMMENT "driftwire.1.gz"
  VERBATIM)
add_custom_target(driftwire_man ALL DEPENDS "${_man_gz}")
install(FILES "${_man_gz}" DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Completions: bash-completion's folder; zsh's vendor folder on Debian and
# Ubuntu, site-functions on Fedora and openSUSE (each is on the other's fpath
# only by accident, so the file is in both).
install(FILES "${_pkg}/completions/driftwire.bash"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME driftwire)
install(FILES "${_pkg}/completions/_driftwire" DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/vendor-completions)
install(FILES "${_pkg}/completions/_driftwire" DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)

# firewalld's service for the beacon (firewall-cmd --add-service=driftwire).
install(FILES "${_pkg}/firewalld/driftwire.xml" DESTINATION lib/firewalld/services)

# A Debian-format changelog that points at the release page (Debian's policy
# asks every package for one). Its date is SOURCE_DATE_EPOCH's when set, so a
# rebuild of one commit is the same package.
set(CPACK_PACKAGE_CONTACT "longtimeno-c <longtimeno-c@users.noreply.github.com>")
string(TIMESTAMP MV_CHANGELOG_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)
configure_file("${_pkg}/changelog.in" "${CMAKE_BINARY_DIR}/changelog" @ONLY)
set(_changelog_gz "${CMAKE_BINARY_DIR}/changelog.gz")
add_custom_command(OUTPUT "${_changelog_gz}"
  COMMAND ${MV_GZIP} -9 -n -c "${CMAKE_BINARY_DIR}/changelog" > "${_changelog_gz}"
  DEPENDS "${CMAKE_BINARY_DIR}/changelog"
  COMMENT "changelog.gz"
  VERBATIM)
add_custom_target(driftwire_changelog ALL DEPENDS "${_changelog_gz}")
install(FILES "${_changelog_gz}" DESTINATION ${CMAKE_INSTALL_DOCDIR})

# The licences: the Debian copyright file (DEP-5: every component and its
# notice), and the GPL text and NOTICE beside it for the rpm and the tarball.
install(FILES "${_pkg}/copyright" DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES "${MV_ROOT}/LICENSE" "${MV_ROOT}/NOTICE" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/driftwire)

# ---- packages -----------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_deb_arch amd64)
  set(_rpm_arch x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_deb_arch arm64)
  set(_rpm_arch aarch64)
else()
  message(FATAL_ERROR "No package architecture for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CPACK_GENERATOR "DEB;RPM;TGZ")
set(CPACK_PACKAGE_NAME driftwire)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "longtimeno-c")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
# Wrapped by hand: a .deb's extended description is shown as written.
set(CPACK_PACKAGE_DESCRIPTION
  "Driftwire sends files to a computer nearby over Wi-Fi, or anywhere with a\n"
  "code, from a terminal. Each transfer is sealed end to end and every file\n"
  "is checked when it arrives. Nothing is sent or saved until the person at\n"
  "the terminal answers. It talks to MediaViewer's Driftwire add-on on\n"
  "Windows and macOS, and to another driftwire.")
string(REPLACE ";" "" CPACK_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
# The .deb and the .rpm install under /usr (their default prefix); the
# tarball is relative, driftwire-<version>-linux-<arch>/bin/driftwire and
# share/..., to run where it is unpacked or to unpack into ~/.local
# (--strip-components=1), where bash-completion and man look too.
set(CPACK_PACKAGE_FILE_NAME "driftwire-${PROJECT_VERSION}-linux-${_rpm_arch}")
set(CPACK_RESOURCE_FILE_LICENSE "${MV_ROOT}/LICENSE")
set(CPACK_STRIP_FILES ON)
set(CPACK_THREADS 0)

# driftwire_<version>_<amd64|arm64>.deb
set(CPACK_DEBIAN_FILE_NAME "driftwire_${PROJECT_VERSION}_${_deb_arch}.deb")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${_deb_arch})
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)  # dpkg-shlibdeps: libc6 (>= the newest symbol used)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libsecret-1-0")
set(CPACK_DEBIAN_COMPRESSION_TYPE xz)

# driftwire-<version>-1.<x86_64|aarch64>.rpm
set(CPACK_RPM_FILE_NAME "driftwire-${PROJECT_VERSION}-1.${_rpm_arch}.rpm")
set(CPACK_RPM_PACKAGE_ARCHITECTURE ${_rpm_arch})
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later AND ISC AND MIT AND CC0-1.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
set(CPACK_RPM_COMPRESSION_TYPE xz)
# No /usr/lib/.build-id links: they would clash with another package that
# happens to carry the same binary, and nothing here ships debug symbols.
# CPack (3.28) writes Suggests but not Recommends, so it goes in by hand.
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none\nRecommends: libsecret")
# Folders the base system owns (Fedora's `filesystem`): listed as its own,
# rpm would claim them. The completion and firewalld folders are left in:
# their packages (zsh, bash-completion, firewalld) may not be installed, and
# zsh's vendor-completions is Debian's, which nothing else owns on Fedora, so
# the package co-owns them and rpm takes them away, when empty, on erase.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/share/licenses /usr/share/man /usr/share/man/man1 /usr/share/doc)

# driftwire-<version>-linux-<x86_64|aarch64>.tar.gz (CPACK_PACKAGE_FILE_NAME).
set(CPACK_ARCHIVE_THREADS 0)

include(CPack)
