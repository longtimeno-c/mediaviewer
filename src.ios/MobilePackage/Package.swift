// swift-tools-version: 6.0
// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The iPhone and iPad app's code, apart from its two app shells (../App).
//
//   MobileKit       The small protocols the real core drops in behind, and their value types.
//   MobileUI        Every screen. Knows only MobileKit.
//   MobileLink      The real services over src/mobile's C ABI (mv_mobile_link.h).
//   MobileFixtures  Stand-in services, generated pictures, the state gallery. Debug only: it does
//                   not compile outside DEBUG, and only the MediaViewerFixtures app links it.
import PackageDescription

let package = Package(
  name: "MobilePackage",
  platforms: [.iOS(.v18)],
  products: [
    .library(name: "MobileKit", targets: ["MobileKit"]),
    .library(name: "MobileUI", targets: ["MobileUI"]),
    .library(name: "MobileFixtures", targets: ["MobileFixtures"]),
    .library(name: "MobileLink", targets: ["MobileLink"]),
  ],
  targets: [
    .target(name: "MobileKit"),
    .target(name: "MobileUI", dependencies: ["MobileKit"]),
    .target(name: "MobileFixtures", dependencies: ["MobileKit", "MobileUI"]),
    // The real connection, sync, pictures and clips (src/mobile/link*, sync*), linked by the app.
    .target(name: "CMobileLink", publicHeadersPath: "include"),
    .target(name: "MobileLinkSupport"),
    .target(name: "MobileLink", dependencies: ["MobileKit", "CMobileLink", "MobileLinkSupport"]),
    .testTarget(name: "MobileLinkSupportTests", dependencies: ["MobileLinkSupport"]),
  ],
  swiftLanguageModes: [.v5]
)
