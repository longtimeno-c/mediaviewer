// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Share everywhere (issue #444, docs/design/16 "Share"): one share command,
// four ways in. ⌘⇧S (the router), the gallery's share icon, the command bar's
// share button beside Home and the right-click Share submenu all run the
// command table's `share`; the host picks the files (shell/share_menu.h: the
// range, else the marks, else the current file, or the right-clicked file when
// it is not one of them) and this shows the one menu: Sharepoint first (or its
// one-click install), then the system's own targets through
// NSSharingServicePicker (Mail, Messages, AirDrop, Notes…). Nothing is read
// or sent until a target is picked (rule 6). The Windows twin is
// IslandHost.Share.cs.
import AppKit
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class ShareMenu {
  static let shared = ShareMenu()

  // shell::share_target / shell::share_from (src/shell/share_menu.h).
  enum Target: Int32 { case menu = 0, sharepoint = 1, system = 2 }
  enum From: Int32 { case key = 0, bar = 1, gallery = 2, context = 3 }

  /// The bar's share button and the gallery's icon, where the menu opens.
  weak var barAnchor: NSView?
  weak var galleryAnchor: NSView?

  /// The picker on screen and its delegate (the picker keeps a weak one).
  private var picker: NSSharingServicePicker?
  private var pickerDelegate: SharePickerDelegate?
  private var offer: NSPopover?
  private var watch: AnyCancellable?
  /// Where the last right-click menu opened, for what its Share entries show.
  private var contextAnchor: (view: NSView, rect: NSRect)?

  private init() {
    // The install offer goes once Sharepoint attaches (it then opens its window).
    watch = SlotStore.shared.$windowScreens.sink { [weak self] screens in
      guard screens[SharepointStore.addonID] != nil else { return }
      Task { @MainActor in self?.offer?.close() }
    }
  }

  /// The chrome's ways in: run the one share command.
  func request(_ target: Target, from: From, clicked: Int = -1) {
    mv_chrome_share(target.rawValue, from.rawValue, Int32(clicked))
  }

  /// The host's answer (MVChromeHost.presentShare): the files are known.
  func present(json: String, target: Target, from: From) {
    guard let files = Self.parse(json), !files.files.isEmpty else { return }
    guard let (view, rect) = anchor(for: from) else { return }
    switch target {
    case .sharepoint:
      sharepoint(files.files, view: view, rect: rect)
    case .menu:
      showPicker(files, view: view, rect: rect, withSharepoint: true)
    case .system:
      // Picked from the right-click submenu, which listed Sharepoint itself.
      showPicker(files, view: view, rect: rect, withSharepoint: false)
    }
  }

  /// The right-click Share submenu for the item at `clicked`, the menu's
  /// entries as on Windows: Sharepoint (or its install), then the system's
  /// targets. Nothing is gathered here: picking an entry runs the share
  /// command with it, and the host resolves the files then (the selection
  /// when the clicked item is part of it, else that item alone).
  func menuItem(clicked: Int, event: NSEvent?) -> NSMenuItem {
    if let event, let view = event.window?.contentView {
      let at = view.convert(event.locationInWindow, from: nil)
      contextAnchor = (view, NSRect(x: at.x - 1, y: at.y - 1, width: 2, height: 2))
    } else {
      contextAnchor = nil
    }
    let sub = NSMenu(title: "Share")
    let nearby = ShareMenuAction(title: Self.sharepointTitle) {
      ShareMenu.shared.request(.sharepoint, from: .context, clicked: clicked)
    }
    nearby.image = Self.appIcon(size: 16)
    nearby.toolTip = "Send to a MediaViewer nearby, over Wi-Fi or Bluetooth"
    sub.addItem(nearby)
    sub.addItem(.separator())
    let system = ShareMenuAction(title: "Mail, Messages, AirDrop and more\u{2026}") {
      ShareMenu.shared.request(.system, from: .context, clicked: clicked)
    }
    system.image = NSImage(systemSymbolName: "square.and.arrow.up", accessibilityDescription: nil)
    sub.addItem(system)
    let item = NSMenuItem(title: "Share", action: nil, keyEquivalent: "")
    item.image = NSImage(systemSymbolName: "square.and.arrow.up", accessibilityDescription: nil)
    item.submenu = sub
    return item
  }

  // MARK: - presenting

  /// Up to this many files the picker gets every file. Past it, opening the
  /// picker cost ~0.09 ms per file on the main thread (ShareKit stats and
  /// sandbox-checks each URL to match extensions: 65 ms warm for 500), so it
  /// is shown with one file per kind and each target it offers is handed all
  /// of them when picked (SharePickerDelegate). Measured, issue #444.
  static let exactLimit = 50

  private func showPicker(_ files: ShareFiles, view: NSView, rect: NSRect, withSharepoint: Bool) {
    guard !files.media.isEmpty else { return }
    let urls = files.media.map { URL(fileURLWithPath: $0, isDirectory: false) }
    let exact = urls.count <= Self.exactLimit
    let picker = NSSharingServicePicker(items: exact ? urls : Self.representatives(urls))
    let delegate = SharePickerDelegate(
      title: withSharepoint ? Self.sharepointTitle : nil,
      allItems: exact ? nil : urls,
      chooseSharepoint: { [weak self] in self?.sharepoint(files.files, view: view, rect: rect) },
      refused: { title, count in Self.refused(title, count: count, in: view.window) })
    picker.delegate = delegate
    self.picker = picker
    pickerDelegate = delegate
    picker.show(relativeTo: rect, of: view, preferredEdge: .minY)
  }

  /// One file per extension, in order: what the picker matches targets on.
  static func representatives(_ urls: [URL]) -> [URL] {
    var seen = Set<String>()
    return urls.filter { seen.insert($0.pathExtension.lowercased()).inserted }.prefix(8).map { $0 }
  }

  /// A target the system offered for the kinds that will not take this many
  /// (Mail and Messages cap attachments): say so, send nothing.
  private static func refused(_ title: String, count: Int, in window: NSWindow?) {
    NSSound.beep()
    let alert = NSAlert()
    alert.messageText = "\(title) cannot take \(count) files at once."
    alert.informativeText = "Select fewer files, or send them with Sharepoint or AirDrop."
    if let window {
      alert.beginSheetModal(for: window)
    } else {
      alert.runModal()
    }
  }

  /// Sharepoint's window with these files, or its one-click install.
  private func sharepoint(_ files: [String], view: NSView, rect: NSRect) {
    if SharepointStore.shared.open(paths: files) { return }
    offer?.close()
    let popover = NSPopover()
    popover.behavior = .transient
    popover.contentViewController = NSHostingController(
      rootView: ThemedRoot(content: SharepointOffer(paths: files).padding(14).frame(width: 320)))
    popover.show(relativeTo: rect, of: view, preferredEdge: .minY)
    offer = popover
  }

  /// The icon and the bar button open under themselves; the key at the bar's
  /// button while the bar is up, else at the top centre of the window.
  private func anchor(for from: From) -> (NSView, NSRect)? {
    if from == .context, let c = contextAnchor, c.view.window != nil {
      return (c.view, c.rect)
    }
    if from == .gallery, let v = galleryAnchor, v.window != nil, !v.isHiddenOrHasHiddenAncestor {
      return (v, v.bounds)
    }
    if let v = barAnchor, v.window != nil, !v.isHiddenOrHasHiddenAncestor, v.window?.isVisible == true {
      return (v, v.bounds)
    }
    guard let content = (NSApp.keyWindow ?? NSApp.mainWindow)?.contentView else { return nil }
    let b = content.bounds
    let top = content.isFlipped ? b.minY : b.maxY - 1
    return (content, NSRect(x: b.midX - 1, y: top, width: 2, height: 1))
  }

  /// An add-on context entry the Share submenu already carries: Sharepoint's
  /// "Send with Sharepoint…" (its manifest's `send_files`). Left out of the
  /// right-click menus, so sending goes one way, through the share command.
  nonisolated static func routesThroughShare(_ item: SlotItem) -> Bool {
    item.addon == SharepointStore.addonID && item.item == "send_files"
  }

  /// Sharepoint's entry (and its install) wears the MediaViewer icon, at
  /// menu size for a menu row and larger for the picker's list.
  static func appIcon(size: CGFloat) -> NSImage {
    let icon = (NSApp.applicationIconImage ?? NSImage()).copy() as? NSImage ?? NSImage()
    icon.size = NSSize(width: size, height: size)
    return icon
  }

  /// Sharepoint's entry: itself, or its one-click install.
  private static var sharepointTitle: String {
    SharepointStore.shared.ready || SharepointStore.shared.installed ? "Sharepoint" : "Install Sharepoint\u{2026}"
  }

  // MARK: - the host's files

  struct ShareFiles {
    var files: [String]  // Sharepoint's: the stops (it takes sidecars itself)
    var media: [String]  // the system picker's: media only
  }

  private static func parse(_ json: String) -> ShareFiles? {
    guard !json.isEmpty,
          let root = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] else { return nil }
    return ShareFiles(files: root["files"] as? [String] ?? [], media: root["media"] as? [String] ?? [])
  }
}

/// Puts Sharepoint at the top of the system's list, and, when the picker was
/// shown with one file per kind (`allItems`), hands each target every file
/// once it is picked (a target that cannot take that many says so).
final class SharePickerDelegate: NSObject, NSSharingServicePickerDelegate {
  private let title: String?
  private let allItems: [URL]?
  private let chooseSharepoint: @MainActor () -> Void
  private let refused: @MainActor (String, Int) -> Void

  init(title: String?, allItems: [URL]?, chooseSharepoint: @escaping @MainActor () -> Void,
       refused: @escaping @MainActor (String, Int) -> Void) {
    self.title = title
    self.allItems = allItems
    self.chooseSharepoint = chooseSharepoint
    self.refused = refused
  }

  func sharingServicePicker(_ picker: NSSharingServicePicker, sharingServicesForItems items: [Any],
                            proposedSharingServices proposedServices: [NSSharingService]) -> [NSSharingService] {
    var services = proposedServices
    if let all = allItems {
      let refused = refused
      services = proposedServices.map { service in
        NSSharingService(title: service.title, image: service.image, alternateImage: service.alternateImage) {
          // Only now, after the pick: the target's own check on every file.
          if service.canPerform(withItems: all) {
            service.perform(withItems: all)
          } else {
            let name = service.title
            MainActor.assumeIsolated { refused(name, all.count) }
          }
        }
      }
    }
    guard let title else { return services }
    let image = MainActor.assumeIsolated { ShareMenu.appIcon(size: 32) }
    let choose = chooseSharepoint
    let nearby = NSSharingService(title: title, image: image, alternateImage: nil) {
      MainActor.assumeIsolated { choose() }
    }
    return [nearby] + services
  }
}

/// A menu row that runs a closure (it is its own target; the menu keeps it).
final class ShareMenuAction: NSMenuItem {
  private let run: @MainActor () -> Void

  init(title: String, run: @escaping @MainActor () -> Void) {
    self.run = run
    super.init(title: title, action: #selector(choose(_:)), keyEquivalent: "")
    target = self
  }

  @available(*, unavailable)
  required init(coder: NSCoder) { fatalError("not from a nib") }

  @objc private func choose(_ sender: Any?) {
    MainActor.assumeIsolated { run() }
  }
}

/// Registers the NSView behind a SwiftUI button as the menu's anchor.
struct ShareAnchor: NSViewRepresentable {
  let from: ShareMenu.From

  func makeNSView(context: Context) -> NSView {
    let v = NSView()
    register(v)
    return v
  }

  func updateNSView(_ nsView: NSView, context: Context) { register(nsView) }

  private func register(_ v: NSView) {
    MainActor.assumeIsolated {
      switch from {
      case .gallery: ShareMenu.shared.galleryAnchor = v
      default: ShareMenu.shared.barAnchor = v
      }
    }
  }
}

/// The command bar's share button, beside Home: the current file in the
/// viewer, the selection with the gallery open. Off with nothing to share.
struct BarShareButton: View {
  @ObservedObject private var store = FolderStore.shared
  @State private var hover = false

  var body: some View {
    let enabled = !store.names.isEmpty && (store.names.indices.contains(store.currentIndex) || !store.range.isEmpty)
    Button { ShareMenu.shared.request(.menu, from: .bar) } label: {
      Image(systemName: "square.and.arrow.up")
        .font(.system(size: 12, weight: .semibold))
        .frame(width: 26, height: 26)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(4))
          .fill(Color.primary.opacity(hover && enabled ? 0.06 : 0)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
    .disabled(!enabled)
    .onHover { hover = $0 }
    .background(ShareAnchor(from: .bar))
    .help("Share: Sharepoint, Mail, Messages, AirDrop and more (⇧⌘S)")
    .accessibilityLabel("Share")
  }
}

/// Right-click (or Control-click) on a gallery or filmstrip tile: the add-ons'
/// entries, then Share (issue #444). SwiftUI's own context menu cannot hold
/// the system's Share submenu, so this is picked up below SwiftUI, as
/// GalleryDrag picks up a drag: tiles report the one under the pointer, and a
/// local monitor shows the menu for it. One comparison per right-click.
@MainActor
final class TileMenu {
  static let shared = TileMenu()
  private var hovered: (index: Int, gallery: Bool)?
  private var monitor: Any?

  func hover(_ index: Int, inside: Bool, gallery: Bool) {
    if inside {
      hovered = (index, gallery)
      install()
    } else if hovered?.index == index && hovered?.gallery == gallery {
      hovered = nil
    }
  }

  private func install() {
    guard monitor == nil else { return }
    monitor = NSEvent.addLocalMonitorForEvents(matching: [.rightMouseDown, .leftMouseDown]) { event in
      let box = TileMenuEvent(event: event)
      return MainActor.assumeIsolated { TileMenuEvent(event: TileMenu.shared.handle(box.event)) }.event
    }
  }

  private func handle(_ event: NSEvent?) -> NSEvent? {
    guard let event else { return nil }
    let secondary = event.type == .rightMouseDown ||
      (event.type == .leftMouseDown && event.modifierFlags.contains(.control))
    guard secondary, let tile = hovered, tile.gallery == mv_chrome_gallery_visible(),
          let view = event.window?.contentView,
          let menu = Self.make(tile: tile.index, event: event) else { return event }
    NSMenu.popUpContextMenu(menu, with: event, for: view)
    return nil
  }

  /// A tile's menu, also the canvas's (MVChromeHost.canvasContextMenu): the
  /// add-ons' entries for that file, then Share. Nil with no file there.
  static func make(tile: Int, event: NSEvent?) -> NSMenu? {
    guard FolderStore.shared.names.indices.contains(tile) else { return nil }
    let menu = SlotCanvasMenu.make(tile: tile) ?? NSMenu(title: "")
    if !menu.items.isEmpty { menu.addItem(.separator()) }
    menu.addItem(ShareMenu.shared.menuItem(clicked: tile, event: event))
    return menu
  }
}

private struct TileMenuEvent: @unchecked Sendable {
  let event: NSEvent?
}
