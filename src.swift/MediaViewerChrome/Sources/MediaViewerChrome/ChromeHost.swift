// SPDX-License-Identifier: GPL-2.0-or-later
// The one thing src/shell/main_mac.mm (ObjC++) needs from Swift: a plain
// NSView hosting the command bar. Building the app target with
// `-emit-objc-header-path` generates MediaViewerChrome-Swift.h, which
// main_mac.mm imports to call this — Swift owns the SwiftUI/NSHostingView
// bridging, C++ only ever sees an NSView*, the same "host owns its own
// widget toolkit, core stays out of it" boundary the Windows XAML islands
// use (plan/14-abi.md's shape, D1).
import AppKit
import SwiftUI

@objc(MVChromeHost)
public final class MVChromeHost: NSObject {
  @objc public static func makeCommandBarView() -> NSView {
    let hosting = NSHostingView(rootView: CommandBarView())
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }

  // PR 18 filmstrip/gallery follow-up (plan/12 2026-09-17). Same hosting
  // shape as makeCommandBarView above -- main_mac.mm only ever sees an
  // NSView*, never SwiftUI/NSHostingView types (plan/14-abi.md's boundary,
  // scoped to this lab).
  @objc public static func makeFilmstripView() -> NSView {
    let hosting = NSHostingView(rootView: FilmstripView())
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }

  @objc public static func makeGalleryView() -> NSView {
    let hosting = NSHostingView(rootView: GalleryView())
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }
}
