# SPDX-License-Identifier: GPL-2.0-or-later
#
# PR 20: MediaViewer.app, its Quick Look thumbnail extension, and Sparkle 2.
# Included from cmake/darwin.cmake after the host sources are defined.
#
#   cmake --build build --target mediaviewer_app   ->  build/MediaViewer.app
#
# The bundle is assembled by tools/mac/macpack.py (copy, bundle the LGPL
# dylibs into Contents/Frameworks, rewrite install names, ad-hoc sign), not by
# CMake's MACOSX_BUNDLE: the .appex inside it is a second bundle CMake's
# Makefile/Ninja generators cannot express, and one script doing both keeps the
# layout in one place. Developer ID signing, the disk image, notarization, and
# the update archive are `macpack.py release` (README "Ship a Mac build").
# plan/13 "macOS first install", plan/15 PR 20.

set(MV_MAC_BUNDLE_ID "io.github.longtimeno-c.mediaviewer" CACHE STRING
  "CFBundleIdentifier of MediaViewer.app; the Quick Look extension appends .thumbnails")
set(MV_MAC_BUILD_NUMBER "${PROJECT_VERSION}" CACHE STRING
  "CFBundleVersion. Sparkle compares it, so every shipped build must raise it")

# Sparkle is off unless the build is given the EdDSA public key that matches
# the owner's private signing key (Sparkle's generate_keys; the private half
# never enters the repo). A build with a feed URL and no key would download
# updates it cannot verify, so there is no half-configured state.
set(MV_SPARKLE_PUBLIC_ED_KEY "" CACHE STRING
  "Sparkle SUPublicEDKey (base64). Empty = MediaViewer.app is built without an updater")
set(MV_SPARKLE_FEED_URL
  "https://github.com/longtimeno-c/mediaviewer/releases/latest/download/appcast.xml"
  CACHE STRING "Sparkle SUFeedURL (HTTPS)")

# Pinned, hash-checked (CLAUDE.md: reproducible builds beat fresh deps). MIT.
set(MV_SPARKLE_VERSION "2.9.6")
set(MV_SPARKLE_SHA256 "52bf9e88cdd972fc0c81501377a880e90d47031bd8ca5462488f843e2609e192")

if(MV_SPARKLE_PUBLIC_ED_KEY)
  if(NOT MV_SPARKLE_FEED_URL MATCHES "^https://")
    message(FATAL_ERROR "MV_SPARKLE_FEED_URL must be https:// (plan/13 Signing)")
  endif()
  set(MV_SPARKLE_DIR "${CMAKE_BINARY_DIR}/_deps/sparkle-${MV_SPARKLE_VERSION}")
  set(MV_SPARKLE_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/Sparkle-${MV_SPARKLE_VERSION}.tar.xz")
  if(NOT EXISTS "${MV_SPARKLE_DIR}/Sparkle.framework")
    file(DOWNLOAD
      "https://github.com/sparkle-project/Sparkle/releases/download/${MV_SPARKLE_VERSION}/Sparkle-${MV_SPARKLE_VERSION}.tar.xz"
      "${MV_SPARKLE_ARCHIVE}"
      EXPECTED_HASH SHA256=${MV_SPARKLE_SHA256}
      TLS_VERIFY ON
      SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${MV_SPARKLE_ARCHIVE}" DESTINATION "${MV_SPARKLE_DIR}")
  endif()
  # Info.plist keys, spliced into packaging/macos/Info.plist.in. plan/13:
  #  - check on launch and every 6 h, download and stage in the background;
  #  - SUEnableAutomaticChecks: no Sparkle permission prompt stacked on first
  #    run; the app menu turns checking off and it stays off;
  #  - SURequireSignedFeed + SUVerifyUpdateBeforeExtraction: the appcast and
  #    the archive must both verify against the pinned key, or nothing happens;
  #  - no system profile is ever sent (rule 6).
  set(MV_SPARKLE_PLIST_KEYS "
  <key>SUFeedURL</key>
  <string>${MV_SPARKLE_FEED_URL}</string>
  <key>SUPublicEDKey</key>
  <string>${MV_SPARKLE_PUBLIC_ED_KEY}</string>
  <key>SURequireSignedFeed</key>
  <true/>
  <key>SUVerifyUpdateBeforeExtraction</key>
  <true/>
  <key>SUEnableAutomaticChecks</key>
  <true/>
  <key>SUAllowsAutomaticUpdates</key>
  <true/>
  <key>SUAutomaticallyUpdate</key>
  <true/>
  <key>SUScheduledCheckInterval</key>
  <integer>21600</integer>
  <key>SUEnableSystemProfiling</key>
  <false/>")
else()
  set(MV_SPARKLE_PLIST_KEYS "")
  message(STATUS "MediaViewer.app: no MV_SPARKLE_PUBLIC_ED_KEY, building without the updater")
endif()

configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/MediaViewer-Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/QuickLook-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/MediaViewerThumbnails-Info.plist" @ONLY)

# --- MediaViewer: the executable inside MediaViewer.app -----------------------
mv_mac_host(MediaViewer)
target_compile_definitions(MediaViewer PRIVATE MV_APP_BUNDLE=1)
if(MV_SPARKLE_PUBLIC_ED_KEY)
  target_compile_definitions(MediaViewer PRIVATE MV_WITH_SPARKLE=1)
  target_compile_options(MediaViewer PRIVATE "-F${MV_SPARKLE_DIR}")
  target_link_options(MediaViewer PRIVATE "-F${MV_SPARKLE_DIR}")
  target_link_libraries(MediaViewer PRIVATE "-framework Sparkle")
endif()

# --- MediaViewerThumbnails: the Quick Look thumbnail extension -----------------
# An app extension's entry point is NSExtensionMain; the principal class is
# MVThumbnailProvider (packaging/macos/QuickLook-Info.plist.in).
add_executable(MediaViewerThumbnails src/shell/quicklook_mac.mm)
set_source_files_properties(src/shell/quicklook_mac.mm
  PROPERTIES COMPILE_FLAGS "-fobjc-arc -fapplication-extension")
target_include_directories(MediaViewerThumbnails PRIVATE src)
target_link_options(MediaViewerThumbnails PRIVATE "-e" "_NSExtensionMain" "-fapplication-extension")
target_link_libraries(MediaViewerThumbnails PRIVATE
  mv_image
  "-framework Foundation"
  "-framework AppKit"
  "-framework CoreGraphics"
  "-framework ImageIO"
  "-framework QuickLookThumbnailing")

# --- MediaViewer.app ----------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MV_APP_BUNDLE "${CMAKE_BINARY_DIR}/MediaViewer.app")
set(MV_ASSEMBLE_ARGS
  assemble
  --app "${MV_APP_BUNDLE}"
  --exe "$<TARGET_FILE:MediaViewer>"
  --appex-exe "$<TARGET_FILE:MediaViewerThumbnails>"
  --info-plist "${CMAKE_BINARY_DIR}/packaging/MediaViewer-Info.plist"
  --appex-plist "${CMAKE_BINARY_DIR}/packaging/MediaViewerThumbnails-Info.plist"
  --appex-entitlements "${CMAKE_SOURCE_DIR}/packaging/macos/QuickLook.entitlements"
  --icon-png "${CMAKE_SOURCE_DIR}/assets/mediaviewer-icon.png"
  --font "${CMAKE_SOURCE_DIR}/assets/fonts/CozetteVector.ttf"
  --dylib-dir "${MV_VCPKG_DYNAMIC_PREFIX}/lib")
if(MV_SPARKLE_PUBLIC_ED_KEY)
  list(APPEND MV_ASSEMBLE_ARGS --sparkle "${MV_SPARKLE_DIR}/Sparkle.framework")
endif()
# PR 11: Crashpad's out-of-process handler, to Contents/Helpers.
if(EXISTS "${MV_CRASHPAD_HANDLER}")
  list(APPEND MV_ASSEMBLE_ARGS --crashpad-handler "${MV_CRASHPAD_HANDLER}")
endif()
# PR 13 / 14: encode and decode jobs out of process, to Contents/Helpers.
list(APPEND MV_ASSEMBLE_ARGS --clipjob "$<TARGET_FILE:MediaViewerClipJob>")
add_custom_target(mediaviewer_app
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/mac/macpack.py" ${MV_ASSEMBLE_ARGS}
  DEPENDS MediaViewer MediaViewerThumbnails MediaViewerClipJob
  COMMENT "Assemble MediaViewer.app (PR 20)"
  VERBATIM)
