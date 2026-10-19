// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire from the app's side (issues #410, #413, #444; docs/design/28): the
// gallery's share icon, its one-click install, and Settings' row while it is
// not installed. The window itself is the add-on's view tree (ScreenView.swift);
// nothing here talks to the network except the plain GETs of the add-on's
// fixed release assets that every add-on install makes (AddonChannel), and
// only when the person asks. The Windows twin is IslandHost.Driftwire.cs.
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class DriftwireStore: ObservableObject {
  static let shared = DriftwireStore()
  nonisolated static let addonID = "driftwire"
  nonisolated static let screenID = "driftwire"
  nonisolated private static let channel = AddonChannel(name: "mediaviewer-addon-driftwire-macos")

  enum Offer: Equatable { case unknown, checking, available(Int), notPublished, needsNewerApp, unreachable }
  @Published private(set) var offer: Offer = .unknown
  @Published private(set) var phase: AddonChannel.Phase?
  @Published private(set) var message = ""
  @Published private(set) var busy = false
  /// Installed (loaded or not): Settings lists it with the other add-ons then.
  @Published private(set) var installed = false

  /// The files a share click asked for before Driftwire was there; sent the
  /// moment it attaches.
  private var pending: [String]?
  private var watch: Any?

  /// Loaded and attached: its window can open.
  var ready: Bool { SlotStore.shared.windowScreens[Self.addonID] != nil }

  private init() {
    watch = SlotStore.shared.$windowScreens.sink { [weak self] screens in
      guard screens[Self.addonID] != nil else { return }
      Task { @MainActor in self?.attached() }
    }
  }

  /// Driftwire's window with `paths` as the files to send: the gallery's
  /// selection (#413). False when it is not loaded.
  @discardableResult
  func open(paths: [String]) -> Bool {
    guard ready else { return false }
    SlotStore.shared.invoke(addon: Self.addonID, item: "send_files", paths: paths)
    SlotWindows.shared.bringForward(ScreenAddress(addon: Self.addonID, screen: Self.screenID))
    return true
  }

  private func attached() {
    installed = true
    guard let paths = pending else { return }
    pending = nil
    _ = open(paths: paths)
  }

  /// Whether it is installed (a read of the store's list, off the main actor).
  func refresh() {
    Task.detached(priority: .utility) {
      let need = Int(mv_chrome_slot_addons(nil, 0))
      var buf = [CChar](repeating: 0, count: need + 1)
      _ = buf.withUnsafeMutableBufferPointer { mv_chrome_slot_addons($0.baseAddress, Int32($0.count)) }
      let list = (try? JSONSerialization.jsonObject(with: Data(String(cString: buf).utf8))) as? [[String: Any]] ?? []
      let there = list.contains { $0["id"] as? String == Self.addonID }
      await MainActor.run { self.installed = there }
    }
  }

  /// Two small GETs of the fixed release assets, then the host checks the
  /// signed manifest: Install is offered only for what would install.
  func probe() {
    guard offer != .checking, !AddonStore.shared.elsewhere else { return }
    offer = .checking
    Task.detached {
      let result = await Self.channel.probe()
      await MainActor.run {
        switch result {
        case .available(let bytes, _, _, _, _): self.offer = .available(bytes)
        case .notPublished: self.offer = .notPublished
        case .needsNewerApp: self.offer = .needsNewerApp
        case .unreachable: self.offer = .unreachable
        }
      }
    }
  }

  /// One click: download, verify, install, load; then `paths` (if any) open in
  /// its window.
  func install(thenOpen paths: [String]? = nil) {
    guard !busy, !AddonStore.shared.elsewhere else { return }
    busy = true
    pending = paths
    message = "Downloading Driftwire\u{2026}"
    Task.detached {
      // A constant set on every path: nothing mutable crosses into the main actor.
      let text: String
      do {
        try await Self.channel.downloadAndInstall { p in Task { @MainActor in self.phase = p } }
        text = ""
      } catch is AddonChannel.NotPublished {
        text = "Driftwire is not published for download yet."
      } catch let e as AddonChannel.AddonError {
        text = e.text
      } catch {
        text = "Driftwire could not be downloaded. Check the connection and try again."
      }
      await MainActor.run {
        self.busy = false
        self.phase = nil
        self.message = text
        if text.isEmpty {
          self.installed = true
          mv_chrome_slot_addons_refresh()  // loads it on the add-on queue; attached() follows
        } else {
          self.pending = nil
        }
      }
    }
  }
}

/// The install offer, in Settings (while Driftwire is not installed) and under
/// the gallery's share icon.
struct DriftwireOffer: View {
  @ObservedObject private var store = DriftwireStore.shared
  /// The files to open with once it is installed (the share icon's selection).
  var paths: [String]? = nil

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("Driftwire").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      Text("Send photos, video and audio to a MediaViewer nearby, over Wi-Fi or Bluetooth. You type the code the other screen shows, and nothing moves until the other person accepts.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if AddonStore.shared.elsewhere {
        note(LocalSearchStore.elsewhereText)
      } else if let phase = store.phase {
        AddonProgressView(phase: phase)
      } else {
        switch store.offer {
        case .available(let bytes):
          Button("Install Driftwire, \(AddonStore.sizeText(bytes))") { store.install(thenOpen: paths) }
            .disabled(store.busy)
        case .notPublished:
          note("Driftwire is not published for download yet. It will be offered here once a release carries it.")
        case .needsNewerApp:
          note("The published Driftwire needs a newer MediaViewer. Update MediaViewer, then install it.")
        case .unreachable:
          note("Could not reach the download server.")
          Button("Try again") { store.probe() }
        case .unknown, .checking:
          note("Checking for Driftwire\u{2026}")
        }
        if !store.message.isEmpty { note(store.message) }
      }
    }
    .onAppear { if store.offer == .unknown || store.offer == .unreachable { store.probe() } }
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(13)).foregroundStyle(MVTheme.body).fixedSize(horizontal: false, vertical: true)
  }
}

/// The gallery's share icon (#413): one icon for the gallery's selection (its
/// shift-click range, else the marks, else the selected cell). Issue #444: it
/// opens the share menu, the one share command's (ShareMenu.swift): Driftwire
/// first, or its one-click install, then the system's targets.
struct GalleryShareButton: View {
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    Button {
      ShareMenu.shared.request(.menu, from: .gallery)
    } label: {
      Label(store.range.count > 1 ? "Share \(store.range.count)" : "Share", systemImage: "square.and.arrow.up")
        .font(MVTheme.font(13))
    }
    .buttonStyle(FlatButtonStyle(selected: false, compact: true))
    .background(ShareAnchor(from: .gallery))
    .help("Share the selection: Driftwire, Mail, Messages, AirDrop and more")
    .accessibilityLabel(store.range.count > 1 ? "Share \(store.range.count) files" : "Share the selection")
  }
}
