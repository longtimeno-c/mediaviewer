# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mv_wire (src/wire): Driftwire's protocol, apart from the add-on, so the app's
# phone feature can link it (docs/plans/phone-in-the-app.md, 2026-11-12):
# the wire format, Noise and CPace, the sealed channel, the NAS service and
# its client, the secret store, remembered devices, the pairing check, the
# Bonjour advert, and the network ports. No DHT, mapping, relay, transfer or
# Bluetooth: those stay in mv_driftwire_engine. Portable (D9): one port file
# per OS for the network, the secret store and the advert.
#
# Included by cmake/driftwire.cmake, so every build that has Driftwire (the
# app on each OS, cmake/portable, iOS, the command line) has this too. The
# file port (io/file_port) is the linker's to find, as before the move: the
# module compiles it in, the core and the tests link mv_io.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, Threads, MV_SOURCE_ROOT; on iOS MV_BLAKE3_TARGET.

if(TARGET mv_wire)
  return()
endif()
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_W "${MV_SOURCE_ROOT}/src/wire")

# Protocol 2's handshake is a post-quantum hybrid (noise.cpp): ML-KEM-768
# arrived in libsodium 1.0.21. A distro libsodium (Ubuntu 24.04 ships 1.0.18)
# fails here, at configure, rather than deep inside noise.cpp.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES ${MV_SODIUM_TARGET})
check_cxx_source_compiles("
#include <sodium.h>
static_assert(crypto_kem_mlkem768_PUBLICKEYBYTES == 1184);
int main() { return 0; }" MV_SODIUM_HAS_MLKEM768)
cmake_pop_check_state()
if(NOT MV_SODIUM_HAS_MLKEM768)
  message(FATAL_ERROR "Driftwire needs libsodium 1.0.21 or newer (ML-KEM-768): use the pinned vcpkg port, "
                      "or a libsodium built from source")
endif()

# The Tunnel looking like HTTPS (slice 4, docs/plans/driftwire-tls-spike.md):
# BoringSSL (Apache-2.0), Chrome's own TLS, for a browser's ClientHello. The
# vcpkg port installs it as the OpenSSL package; it must be BoringSSL (the
# ClientHello calls are its own). Without it, tls_none.cpp: the Tunnel runs on
# the relay's plain port only.
# The fuzz build (MV_FUZZ) leaves it out: its harnesses are /MT and libFuzzer's
# runtime will not link with a static /MD library, and it fuzzes Driftwire's
# parsers, not TLS.
find_package(OpenSSL CONFIG QUIET)
set(MV_DRIFTWIRE_TLS OFF)
if(TARGET OpenSSL::SSL AND NOT MV_FUZZ)
  cmake_push_check_state()
  set(CMAKE_REQUIRED_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
  check_cxx_source_compiles("
#include <openssl/ssl.h>
#if !defined(OPENSSL_IS_BORINGSSL)
#error not BoringSSL
#endif
int main() { SSL_CTX* c = SSL_CTX_new(TLS_method()); SSL_CTX_set_permute_extensions(c, 1); return 0; }"
    MV_HAS_BORINGSSL)
  cmake_pop_check_state()
  if(MV_HAS_BORINGSSL)
    set(MV_DRIFTWIRE_TLS ON)
  endif()
endif()
if(MV_DRIFTWIRE_TLS)
  set(_driftwire_tls ${_W}/tls_boringssl.cpp)
else()
  message(STATUS "Driftwire: no BoringSSL; the Tunnel's HTTPS look (slice 4) is left out")
  set(_driftwire_tls ${_W}/tls_none.cpp)
endif()

if(WIN32)
  set(_wire_port ${_W}/net_win.cpp ${_W}/secrets_win.cpp ${_W}/advert_win.cpp)
elseif(IOS)
  # No app or module on iOS links the core's io, so the archive carries the file
  # port the NAS service reads through (and BLAKE3 hashing, which the
  # Driftwire engine uses), and nothing in it reaches back into the engine.
  set(_wire_port ${_W}/net_posix.cpp ${_W}/machine_ios.cpp ${_W}/secrets_ios.cpp ${_W}/advert_none.cpp
      ${MV_SOURCE_ROOT}/src/io/file_port.cpp ${MV_SOURCE_ROOT}/src/io/file_port_mac.cpp
      ${MV_SOURCE_ROOT}/src/io/content_hash.cpp)
elseif(APPLE)
  set(_wire_port ${_W}/net_posix.cpp ${_W}/machine_mac.cpp ${_W}/secrets_mac.cpp ${_W}/advert_mac.cpp)
else()
  set(_wire_port ${_W}/net_posix.cpp ${_W}/secrets_linux.cpp ${_W}/advert_none.cpp)
endif()

add_library(mv_wire STATIC
  ${_W}/wire.cpp ${_W}/wire.h
  ${_W}/link.h
  ${_W}/keys.cpp ${_W}/keys.h
  ${_W}/noise.cpp ${_W}/noise.h
  ${_W}/cpace.cpp ${_W}/cpace.h
  ${_W}/stages.cpp ${_W}/stages.h
  ${_W}/channel.cpp ${_W}/channel.h
  ${_W}/net.h ${_W}/net_address.cpp
  ${_W}/admission.h
  ${_W}/secrets.h ${_W}/secrets_file.cpp
  ${_W}/known.cpp ${_W}/known.h
  ${_W}/serve_wire.cpp ${_W}/serve_wire.h
  ${_W}/owner_pass.cpp ${_W}/owner_pass.h
  ${_W}/grants.cpp ${_W}/grants.h
  ${_W}/serve.cpp ${_W}/serve.h
  ${_W}/serve_backup.cpp ${_W}/serve_backup.h
  ${_W}/serve_client.cpp ${_W}/serve_client.h
  ${_W}/pair_check.cpp ${_W}/pair_check.h
  ${_W}/splice.cpp ${_W}/splice.h
  ${_W}/tls.h ${_W}/tls_common.cpp ${_driftwire_tls}
  ${_W}/advert.h
  ${_wire_port}
)
target_include_directories(mv_wire PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_wire PUBLIC mv_project_options ${MV_SODIUM_TARGET} Threads::Threads)
if(MV_DRIFTWIRE_TLS)
  target_link_libraries(mv_wire PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(mv_wire PUBLIC MV_DRIFTWIRE_TLS=1)
endif()
set_target_properties(mv_wire PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # ws2_32 / iphlpapi: sockets and this machine's addresses. shell32 / ole32:
  # the Downloads folder (net_win.cpp). advapi32 / crypt32: Credential Manager
  # and DPAPI (secrets_win.cpp). dnsapi: the advert.
  target_link_libraries(mv_wire PUBLIC ws2_32 iphlpapi shell32 ole32 advapi32 crypt32 dnsapi)
elseif(IOS)
  target_link_libraries(mv_wire PUBLIC "-framework CoreFoundation" "-framework Security" ${MV_BLAKE3_TARGET})
elseif(APPLE)
  target_link_libraries(mv_wire PUBLIC "-framework CoreFoundation" "-framework SystemConfiguration"
    "-framework Security")
else()
  # dlopen: libsecret (secrets_linux.cpp).
  target_link_libraries(mv_wire PUBLIC ${CMAKE_DL_LIBS})
endif()
