// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The one thing src/shell/main_mac.mm (ObjC++) needs from Swift: a plain
// NSView hosting the command bar. Building the app target with
// `-emit-objc-header-path` generates MediaViewerChrome-Swift.h, which
// main_mac.mm imports to call this — Swift owns the SwiftUI/NSHostingView
// bridging, C++ only ever sees an NSView*, the same "host owns its own
// widget toolkit, core stays out of it" boundary the Windows XAML islands
// use (docs/design/14-abi.md's shape, D1).
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
    // docs/design/25: every root is rebuilt when the theme changes.
    let hosting = NSHostingView(rootView: ThemedRoot(content: root))
    hosting.sizingOptions = []
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }

  @objc public static func makeCommandBarView() -> NSView {
    host(CommandBarView())
  }

  // PR 18 filmstrip/gallery follow-up (docs/design/12 2026-09-17). Same hosting
  // shape as makeCommandBarView above -- main_mac.mm only ever sees an
  // NSView*, never SwiftUI/NSHostingView types (docs/design/14-abi.md's boundary,
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

  /// `?` opened: the sheet re-reads the live table (Settings may have remapped).
  @MainActor @objc public static func reloadHelp() {
    HelpStore.shared.reload()
  }

  @objc public static func makeSettingsView() -> NSView {
    host(SettingsView())
  }

  /// docs/design/25: an add-on package handed to the app (a drop, Open With, the
  /// command line). The host has opened Settings; this shows what the package
  /// is and asks. Nothing is installed without that answer.
  @MainActor @objc public static func offerAddonPackage(_ path: String) {
    OpenAddonStore.shared.offer(file: path)
  }

  /// The host's MV_ADDON_SELFTEST rig (main_mac.mm): the same calls the
  /// buttons make, and one line of state for its log. Inert otherwise.
  @MainActor @objc public static func addonSelfTest(_ action: String, argument: String) -> String {
    guard ProcessInfo.processInfo.environment["MV_ADDON_SELFTEST"] != nil else { return "" }
    let addons = OpenAddonStore.shared
    let theme = ThemeStore.shared
    switch action {
    case "offered": return addons.offer?.addon?.addonID ?? addons.installed.first?.addonID ?? ""
    case "confirm": addons.confirm()
    case "choose": theme.choose(argument)
    case "choose-first": theme.choose(addons.themes.first?.key ?? "")
    case "choose-last": theme.choose(addons.themes.last?.key ?? "")
    case "changed":
      addons.refresh()
      theme.addonsChanged()
    case "remove":
      if let addon = addons.installed.first(where: { $0.folder == argument }) { addons.remove(addon) }
    default: break
    }
    let offer = addons.offer.map {
      "offer(ok=\($0.ok) relation=\($0.relation) name=\($0.addon?.name ?? "") key=\($0.addon?.fingerprint ?? "") adds=\($0.addon?.adds ?? ""))"
    } ?? "offer(none)"
    let installed = addons.installed.map { "\($0.addonID) \($0.version) \($0.state)" }.joined(separator: ",")
    return "\(offer) installed=[\(installed)] themes=\(addons.themes.count) theme=\"\(theme.selection)\" "
      + "themed=\(MVTheme.themed != nil) note=\"\(theme.note)\" message=\"\(addons.message)\""
  }

  /// PR 9: the metadata pane and folder tree, floating over the canvas.
  @objc public static func makeMetadataView() -> NSView {
    host(MetadataView())
  }

  @objc public static func makeFolderTreeView() -> NSView {
    host(FolderTreeView())
  }

  /// PR 11: the adjust pane, on the metadata pane's edge.
  @objc public static func makeAdjustView() -> NSView {
    host(AdjustView())
  }

  /// PR 10: the export sheet, a full-container overlay built fresh per open.
  @objc public static func makeExportView() -> NSView {
    host(ExportView())
  }

  /// PR 13 / 14: the Jobs pane, on the metadata / adjust pane's edge.
  @objc public static func makeJobsView() -> NSView {
    host(JobsView())
  }

  /// PR 14: the clip tools sheet, built fresh per open like the export sheet.
  @objc public static func makeClipToolsView() -> NSView {
    host(ClipToolsView())
  }

  /// PR 29 (docs/design/20): the Edit workspace's strip and its Crop / Trim pane.
  @objc public static func makeEditStripView() -> NSView {
    host(EditStripView())
  }

  @objc public static func makeEditPaneView() -> NSView {
    host(EditPaneView())
  }

  /// PR 30 (docs/design/21): the Video Editor window's timeline, under its preview.
  @objc public static func makeVideoEditorView() -> NSView {
    host(VideoEditorView())
  }

  /// PR 11 verify only (MV_CRASH_TEST=nsexception, crash_reporter_mac.h): an
  /// NSException raised from the chrome, inside AppKit's event handling.
  @objc public static func crashTestException() {
    NSException(name: .internalInconsistencyException, reason: "MV_CRASH_TEST nsexception",
                userInfo: nil).raise()
  }

  /// PR 11 verify only (MV_CRASH_TEST=swift_trap): a Swift runtime trap in the
  /// chrome. The index is not a constant, so the compiler cannot fold it away.
  @objc public static func crashTestSwiftTrap() {
    let empty: [Int] = []
    _ = empty[Int.random(in: 1...2)]
  }

  /// File search (docs/design/16 "File search"): the field above the grid appears and
  /// takes the keyboard with its text selected. main_mac.mm has already shown
  /// the gallery and made its hosting view first responder.
  @objc public static func openFileSearch() {
    MainActor.assumeIsolated { FileSearchStore.shared.open() }
  }

  /// The gallery closed: so does file search, and its filter with it.
  @objc public static func closeFileSearch() {
    MainActor.assumeIsolated { FileSearchStore.shared.close() }
  }

  /// ⌘F while the Local search command is not there. True when the pack is
  /// verifying and starting at launch: its panel opens once it attaches, as
  /// the path icon's click does. False: nothing is on its way, so the host
  /// opens file search instead.
  @objc public static func openLocalSearchWhenStarting() -> Bool {
    MainActor.assumeIsolated {
      let search = LocalSearchStore.shared
      guard search.searchAvailable else { return false }
      search.openSearchWhenReady()
      return true
    }
  }

  /// Gallery `+` / `-` (docs/design/16): called from main_mac.mm's keyDown: on the
  /// main thread; `direction` is +1 or -1.
  @objc public static func adjustGalleryCellSize(_ direction: Int) {
    MainActor.assumeIsolated { FolderStore.shared.adjustGalleryCellSize(direction: direction) }
  }
}
