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

  // Issue #486: a pane main_mac.mm creates hidden at launch. SwiftUI lays out a
  // hidden hosting view's graph all the same, and the dozen launch-time panes
  // cost ~200 ms of the window's first order-front on the main thread. This one
  // hosts nothing until it is first shown, then builds its content once.
  static func lazyHost<V: View>(_ make: @escaping () -> V) -> NSView {
    let hosting = LazyHostingView { AnyView(ThemedRoot(content: make())) }
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
    lazyHost { FilmstripView() }
  }

  @objc public static func makeGalleryView() -> NSView {
    lazyHost { GalleryView() }
  }

  @objc public static func makeTransportView() -> NSView {
    lazyHost { TransportView() }
  }

  /// #345: the AI-generated badge, bottom-right of the canvas. main_mac.mm shows,
  /// hides and places it; `setAIBadge` gives it its text.
  @objc public static func makeAIBadgeView() -> NSView {
    lazyHost { AIBadgeView() }
  }

  @objc public static var aiBadgeWidth: CGFloat { AIBadgeView.width }
  @objc public static var aiBadgeHeight: CGFloat { AIBadgeView.height }

  @MainActor @objc public static func setAIBadge(title: String, evidence: String) {
    AIBadgeStore.shared.set(title: title, evidence: evidence)
  }

  @objc public static func makeHelpView() -> NSView {
    lazyHost { HelpView() }
  }

  /// `?` opened: the sheet re-reads the live table (Settings may have remapped).
  /// `modes`: the mode_mask of the mode underneath; only its bindings are listed.
  @MainActor @objc public static func reloadHelp(modes: Int32) {
    HelpStore.shared.reload(modes: modes)
  }

  @objc public static func makeSettingsView() -> NSView {
    lazyHost { SettingsView() }
  }

  /// docs/design/25: an add-on package handed to the app (a drop, Open With, the
  /// command line). The host has opened Settings; this shows what the package
  /// is and asks. Nothing is installed without that answer.
  @MainActor @objc public static func offerAddonPackage(_ path: String) {
    OpenAddonStore.shared.offer(file: path)
  }

  /// docs/design/26: the welcome card's iCloud Photos row. The host has opened
  /// Settings; this brings its Photos Library section into view.
  @MainActor @objc public static func revealPhotosSettings() {
    PhotosBackupStore.shared.reveal()
  }

  /// The host's MV_SCRIPT_SELFTEST rig (main_mac.mm, docs/design/25 PR 57): Settings
  /// scrolled to From others, where a stopped script add-on says why.
  @MainActor @objc public static func revealOpenAddons() {
    OpenAddonStore.shared.reveal()
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

  /// PR 56's leg of the same rig: an add-on's settings page drawn by the real
  /// view, alone in an off-screen window, written as a PNG once it has read
  /// its page; and the Remove panel's "Keep its settings". Inert otherwise.
  @MainActor private static var pageSnapshots: [NSWindow] = []
  @MainActor @objc public static func addonSelfTestPage(_ addonID: String, path: String) {
    guard ProcessInfo.processInfo.environment["MV_ADDON_SELFTEST"] != nil else { return }
    let window = NSWindow(contentRect: NSRect(x: -10000, y: -10000, width: 600, height: 760),
                          styleMask: [.borderless], backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    let root = AddonSettingsPage(addonID: addonID, fallbackTitle: addonID)
      .frame(width: 560).padding(MVTheme.space(20)).frame(maxHeight: .infinity, alignment: .top)
      .background(MVTheme.canvas)
    window.contentView = NSHostingView(rootView: ThemedRoot(content: root))
    window.orderFrontRegardless()
    pageSnapshots.append(window)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
      guard let v = window.contentView else { return }
      v.layoutSubtreeIfNeeded()
      if let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
        v.cacheDisplay(in: v.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
      }
      window.orderOut(nil)
      pageSnapshots.removeAll { $0 === window }
    }
  }

  @MainActor @objc public static func addonSelfTestRemove(_ folder: String, keepSettings: Bool) {
    guard ProcessInfo.processInfo.environment["MV_ADDON_SELFTEST"] != nil else { return }
    let addons = OpenAddonStore.shared
    guard let addon = addons.installed.first(where: { $0.folder == folder }) else { return }
    addons.keepSettings = keepSettings
    addons.remove(addon)
  }

  @MainActor @objc public static func addonSelfTestOffer(_ path: String) {
    guard ProcessInfo.processInfo.environment["MV_ADDON_SELFTEST"] != nil else { return }
    OpenAddonStore.shared.offer(file: path)
  }

  /// docs/design/25, issue #402: the add-ons' context-menu entries for the file
  /// on the canvas, as its gallery tile lists them, then Share (issue #444).
  /// Nil when no file is on screen (the canvas then shows no menu).
  @MainActor @objc public static func canvasContextMenu() -> NSMenu? {
    TileMenu.make(tile: FolderStore.shared.currentIndex, event: NSApp.currentEvent)
  }

  /// Issue #444: the one share command's files are known (main_mac.mm's
  /// shareRequest:). `target` / `from` are shell::share_target / share_from.
  @MainActor @objc public static func presentShare(_ json: String, target: Int, from: Int) {
    ShareMenu.shared.present(json: json,
                             target: ShareMenu.Target(rawValue: Int32(clamping: target)) ?? .menu,
                             from: ShareMenu.From(rawValue: Int32(clamping: from)) ?? .key)
  }

  /// PR 9: the metadata pane and folder tree, floating over the canvas.
  @objc public static func makeMetadataView() -> NSView {
    lazyHost { MetadataView() }
  }

  @objc public static func makeFolderTreeView() -> NSView {
    lazyHost { FolderTreeView() }
  }

  /// PR 11: the adjust pane, on the metadata pane's edge.
  @objc public static func makeAdjustView() -> NSView {
    lazyHost { AdjustView() }
  }

  /// PR 10: the export sheet, a full-container overlay built fresh per open.
  @objc public static func makeExportView() -> NSView {
    host(ExportView())
  }

  /// PR 13 / 14: the Jobs pane, on the metadata / adjust pane's edge.
  @objc public static func makeJobsView() -> NSView {
    lazyHost { JobsView() }
  }

  /// PR 14: the clip tools sheet, built fresh per open like the export sheet.
  @objc public static func makeClipToolsView() -> NSView {
    host(ClipToolsView())
  }

  /// PR 29 (docs/design/20): the Edit workspace's strip and its Crop / Trim pane.
  @objc public static func makeEditStripView() -> NSView {
    lazyHost { EditStripView() }
  }

  @objc public static func makeEditPaneView() -> NSView {
    lazyHost { EditPaneView() }
  }

  /// PR 30 (docs/design/21): the Video Editor window's timeline, under its preview.
  @objc public static func makeVideoEditorView() -> NSView {
    host(VideoEditorView())
  }

  /// Issue #299: the bottom-right "Resume indexing Photos?" notice. Unlike the
  /// panes it sizes itself (a fixed-width card): main_mac.mm pins only its
  /// bottom-right corner. Hidden until there is a question; the store shows it.
  @MainActor @objc public static func makeResumeNoticeView() -> NSView {
    let hosting = NSHostingView(rootView: ThemedRoot(content: ResumeNoticeView()))
    hosting.sizingOptions = [.intrinsicContentSize]
    hosting.translatesAutoresizingMaskIntoConstraints = false
    hosting.isHidden = true
    ResumeNoticeStore.shared.host = hosting
    return hosting
  }

  /// The "Press T" filmstrip hint card (HintNoticeView.swift): sized like the
  /// resume notice, pinned by its bottom-right corner, hidden until shown.
  @MainActor @objc public static func makeHintNoticeView() -> NSView {
    let hosting = NSHostingView(rootView: ThemedRoot(content: HintNoticeView()))
    hosting.sizingOptions = [.intrinsicContentSize]
    hosting.translatesAutoresizingMaskIntoConstraints = false
    hosting.isHidden = true
    HintNoticeStore.shared.host = hosting
    return hosting
  }

  /// Show the hint card for a few seconds, or hide it with "".
  @MainActor @objc public static func showHint(_ text: String) {
    HintNoticeStore.shared.show(text)
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

  /// ⌘F while the Local search command is not there and the host did not
  /// start the pack itself (MvAddonsSearchOnDemand). True when something is on
  /// its way: the panel opens once it attaches, as the path icon's click does.
  /// False: nothing is, so the host opens file search instead.
  @objc public static func openLocalSearchWhenStarting() -> Bool {
    MainActor.assumeIsolated {
      let search = LocalSearchStore.shared
      guard search.searchAvailable else { return false }
      return search.openSearchWhenReady()
    }
  }

  /// Gallery `+` / `-` (docs/design/16): called from main_mac.mm's keyDown: on the
  /// main thread; `direction` is +1 or -1.
  @objc public static func adjustGalleryCellSize(_ direction: Int) {
    MainActor.assumeIsolated { FolderStore.shared.adjustGalleryCellSize(direction: direction) }
  }
}

/// Issue #486: hosts an empty root until it is first visible (shown, or laid
/// out while not hidden), then its real content, built once.
final class LazyHostingView: NSHostingView<AnyView> {
  private var make: (() -> AnyView)?

  init(_ make: @escaping () -> AnyView) {
    self.make = make
    super.init(rootView: AnyView(EmptyView()))
  }

  required init(rootView: AnyView) {
    super.init(rootView: rootView)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) {
    fatalError("init(coder:) is not used")
  }

  override var isHidden: Bool {
    didSet { if !isHidden { build() } }
  }

  override func layout() {
    if !isHidden { build() }
    super.layout()
  }

  private func build() {
    guard let make else { return }
    self.make = nil
    rootView = make()
  }
}
