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
  // Chrome is pinned by main_mac.mm's Auto Layout constraints; the SwiftUI
  // content must never size the window. NSHostingView's default
  // sizingOptions ([.minSize, .intrinsicContentSize, .maxSize]) publish the
  // content's ideal size as constraints, and with a Spacer()-ending HStack
  // that collapsed the whole window to ~76pt wide (found on real hardware,
  // 2026-09-19). Empty options = the hosting view takes whatever frame it is
  // given.
  private static func host<V: View>(_ root: V) -> NSView {
    let hosting = NSHostingView(rootView: root)
    hosting.sizingOptions = []
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }

  @objc public static func makeCommandBarView() -> NSView {
    host(CommandBarView())
  }

  // PR 18 filmstrip/gallery follow-up (plan/12 2026-09-17). Same hosting
  // shape as makeCommandBarView above -- main_mac.mm only ever sees an
  // NSView*, never SwiftUI/NSHostingView types (plan/14-abi.md's boundary,
  // scoped to this lab).
  @objc public static func makeFilmstripView() -> NSView {
    host(FilmstripView())
  }

  @objc public static func makeGalleryView() -> NSView {
    host(GalleryView())
  }

  @objc public static func makeTransportView() -> NSView {
    host(TransportView())
  }

  @objc public static func makeHelpView() -> NSView {
    host(HelpView())
  }

  @objc public static func makeSettingsView() -> NSView {
    host(SettingsView())
  }

  /// Gallery `+` / `-` (plan/16): called from main_mac.mm's keyDown: on the
  /// main thread; `direction` is +1 or -1.
  @objc public static func adjustGalleryCellSize(_ direction: Int) {
    MainActor.assumeIsolated { FolderStore.shared.adjustGalleryCellSize(direction: direction) }
  }
}
