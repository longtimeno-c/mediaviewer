// swift-tools-version: 6.0
// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The iPhone and iPad app's code, apart from its two app shells (../App).
//
//   MobileKit       The small protocols the real core drops in behind, and their value types.
//   MobileUI        Every screen. Knows only MobileKit.
//   MobileCore      The real library, search and tiles, over the C ABI of src/mobile.
//   MobileFixtures  Stand-in services, generated pictures, the state gallery. Debug only: it does
//                   not compile outside DEBUG, and only the MediaViewerFixtures app links it.
import PackageDescription

let package = Package(
  name: "MobilePackage",
  platforms: [.iOS(.v18)],
  products: [
    .library(name: "MobileKit", targets: ["MobileKit"]),
    .library(name: "MobileUI", targets: ["MobileUI"]),
    .library(name: "MobileCore", targets: ["MobileCore"]),
    .library(name: "MobileFixtures", targets: ["MobileFixtures"]),
  ],
  targets: [
    .target(name: "MobileKit"),
    .target(name: "MobileUI", dependencies: ["MobileKit"]),
    .target(name: "CMobileSearch", path: "Sources/CMobileSearch", publicHeadersPath: "include"),
    .target(name: "MobileCore", dependencies: ["MobileKit", "CMobileSearch"]),
    .target(name: "MobileFixtures", dependencies: ["MobileKit", "MobileUI"]),
  ],
  swiftLanguageModes: [.v5]
)
