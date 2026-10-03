// swift-tools-version: 5.9
// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone H (plan/17): the AI pack's Mac chrome. Built as a dynamic library
// that cmake/darwin.cmake wraps into AI.bundle (principal class MVAIChrome),
// shipped in the signed AI pack beside libmv_ai.dylib and ONNX Runtime, never
// in MediaViewer.app. The host (src/shell/addons_mac.mm) loads it with NSBundle
// and reaches it by message send only, so this package links nothing of the
// app: the pack is reached through the mv.ai.1 function table the host hands
// to -attachWithTable:host:, and the viewer through selectors on the host
// object (openList:, viewerState, seekTo:, setMarkers:, viewerWindow).
import PackageDescription

let package = Package(
  name: "AIChrome",
  platforms: [.macOS(.v14)],
  products: [
    .library(name: "AIChrome", type: .dynamic, targets: ["AIChrome"])
  ],
  targets: [
    // The C view of mediaviewer_ai.h (the table's types).
    .target(name: "CAiApi"),
    .target(name: "AIChrome", dependencies: ["CAiApi"]),
    // The chrome's own measurements (docs/DEVELOPMENT.md "Test"): the People
    // grid at 200 people, with an empty table. `swift test -c release
    // -Xswiftc -enable-testing` here; not part of the cmake build.
    .testTarget(name: "AIChromeTests", dependencies: ["AIChrome"]),
  ]
)
