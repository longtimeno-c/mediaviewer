// swift-tools-version: 5.9
// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone G (plan/18): the Import add-on's Mac chrome. Built as a dynamic
// library that cmake/darwin.cmake wraps into Import.bundle (principal class
// MVImportChrome), shipped in the signed add-on archive beside
// libmv_import.dylib, never in MediaViewer.app. The host loads it with
// NSBundle and reaches it by message send only, so this package links
// nothing of the app: the Import engine is reached through the mv.import.1
// function table the host hands to -attachWithTable:host:.
import PackageDescription

let package = Package(
  name: "ImportChrome",
  platforms: [.macOS(.v14)],
  products: [
    .library(name: "ImportChrome", type: .dynamic, targets: ["ImportChrome"])
  ],
  targets: [
    // The C view of mediaviewer_import.h (the table's types).
    .target(name: "CImportApi"),
    .target(name: "ImportChrome", dependencies: ["CImportApi"]),
  ]
)
