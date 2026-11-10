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
import Foundation
import PackageDescription

// MobileCoreTests link the C++ core built for the Simulator by tools/ios/build-core.sh
// (tools/ios/run-mobilecore-tests.sh builds it first). Nothing else in the package does.
let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().path
let simCore = repo + "/build-ios-iphonesimulator"
let simTriplet = simCore + "/vcpkg_installed/arm64-ios-simulator/lib"
let coreLink: [String] = [
  simCore + "/libmv_mobile_test_fixture.a", simCore + "/libmv_mobile_search.a", simCore + "/libmv_ai_engine.a",
  simCore + "/libmv_infer.a", simCore + "/libmv_model_fetch.a", simCore + "/libmv_driftwire_engine.a",
  simTriplet + "/libsqlite3.a", simTriplet + "/libsodium.a", simTriplet + "/libblake3.a",
  simCore + "/_ort/pod-archive-onnxruntime-c-1.30.0/onnxruntime.xcframework/ios-arm64_x86_64-simulator/onnxruntime.framework/onnxruntime",
  "-lc++", "-framework", "CoreML", "-framework", "Network", "-framework", "Security", "-framework", "SystemConfiguration",
  "-framework", "CoreBluetooth",
]

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
    .target(name: "CMobileTestFixture", path: "Tests/CMobileTestFixture", publicHeadersPath: "include"),
    .testTarget(name: "MobileCoreTests", dependencies: ["MobileCore", "CMobileSearch", "CMobileTestFixture"],
                linkerSettings: [.unsafeFlags(coreLink)]),
  ],
  swiftLanguageModes: [.v5]
)
