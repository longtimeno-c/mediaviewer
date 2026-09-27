// swift-tools-version: 5.9
// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PR 18's SwiftUI chrome (plan/10-roadmap.md, plan/15-platforms.md): "the
// canvas is not ported to SwiftUI" — PR 16/17's AppKit window and CAMetalLayer
// stay exactly as built, this package only supplies the command bar hosted
// inside them. `swift build` here is real compile verification; linking this
// into `mediaviewer_lab` still goes through cmake/darwin.cmake, which this
// sandbox cannot run (no cmake, no Xcode).
import PackageDescription

let package = Package(
  name: "MediaViewerChrome",
  platforms: [.macOS(.v14)],
  products: [
    .library(name: "MediaViewerChrome", type: .static, targets: ["MediaViewerChrome"])
  ],
  targets: [
    // Plain C target: the extern "C" functions Swift calls to reach
    // src/shell/input_state.h's input_snapshot. Defined in src/shell/main_mac.mm
    // (C++/ObjC++), not here — same "core never touches the dispatcher, host
    // pokes a POD snapshot" shape as plan/14-abi.md, scoped down to the lab.
    .target(name: "MVChromeBridge"),
    .target(
      name: "MediaViewerChrome",
      dependencies: ["MVChromeBridge"]
    ),
  ]
)
