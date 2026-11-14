// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access from the app's side (docs/plans/phone-access-addon.md,
// owner 2026-11-14): Phone access is an add-on. Installed, its card is the
// add-on's own view tree (its "settings" screen, drawn here by ScreenView);
// not installed, this section offers it, once, the first time someone opens
// Phone access. Nothing here talks to the network except the plain GETs of the
// add-on's fixed release assets every add-on install makes (AddonChannel), and
// only when the person asks. A desktop set up before the move is told that its
// phones keep working through its server. The Windows twin is
// IslandHost.PhoneAccess.cs.
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class PhoneAccessStore: ObservableObject {
  static let shared = PhoneAccessStore()
  nonisolated static let addonID = "phone-access"
  nonisolated private static let channel = AddonChannel(name: "mediaviewer-addon-phone-access-macos")

  enum Offer: Equatable { case unknown, checking, available(Int), notPublished, needsNewerApp, unreachable }
  @Published private(set) var offer: Offer = .unknown
  @Published private(set) var phase: AddonChannel.Phase?
  @Published private(set) var message = ""
  @Published private(set) var busy = false
  /// A library was set up here before Phone access was an add-on (its folder.json).
  @Published private(set) var setUpBefore = false

  private init() {}

  /// The add-on's page in Settings (its "settings" screen), once it is loaded.
  var screen: ScreenAddress? {
    SlotStore.shared.settingsScreens[Self.addonID].map { ScreenAddress(addon: Self.addonID, screen: $0) }
  }

  /// Two small GETs of the fixed release assets, then the host checks the
  /// signed manifest: Install is offered only for what would install. The
  /// folder from before is looked at off the main actor (one stat).
  func probe() {
    guard offer != .checking, !AddonStore.shared.elsewhere else { return }
    offer = .checking
    Task.detached {
      let before = Self.libraryFromBefore()
      let result = await Self.channel.probe()
      await MainActor.run {
        self.setUpBefore = before
        switch result {
        case .available(let bytes, _, _, _, _): self.offer = .available(bytes)
        case .notPublished: self.offer = .notPublished
        case .needsNewerApp: self.offer = .needsNewerApp
        case .unreachable: self.offer = .unreachable
        }
      }
    }
  }

  /// The feature's folder, where the app kept the library before the add-on
  /// (the add-on keeps it there: src/addons/phone/feature_dir_posix.cpp).
  nonisolated private static func libraryFromBefore() -> Bool {
    let dir = FileManager.default.homeDirectoryForCurrentUser
      .appendingPathComponent("Library/Application Support/MediaViewer/Phone Access/folder.json")
    return FileManager.default.fileExists(atPath: dir.path)
  }

  /// One click: download, verify, install, load; its card then appears here.
  func install() {
    guard !busy, !AddonStore.shared.elsewhere else { return }
    busy = true
    message = "Downloading Phone access\u{2026}"
    Task.detached {
      let text: String
      do {
        try await Self.channel.downloadAndInstall { p in Task { @MainActor in self.phase = p } }
        text = ""
      } catch is AddonChannel.NotPublished {
        text = "Phone access is not published for download yet."
      } catch let e as AddonChannel.AddonError {
        text = e.text
      } catch {
        text = "Phone access could not be downloaded. Check the connection and try again."
      }
      await MainActor.run {
        self.busy = false
        self.phase = nil
        self.message = text
        if text.isEmpty { mv_chrome_slot_addons_refresh() }  // loads it on the add-on queue
      }
    }
  }
}

/// Settings → Phone access: the add-on's card, or the offer to install it.
struct PhoneAccessSection: View {
  @ObservedObject private var store = PhoneAccessStore.shared
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    if let address = store.screen {
      AddonSettingsScreen(address: address)
    } else {
      PhoneAccessOffer()
    }
  }
}

/// The install offer, while Phone access is not installed.
struct PhoneAccessOffer: View {
  @ObservedObject private var store = PhoneAccessStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      Text("Reach your library from your phone, at home and away: through a Synology, through this Mac as your server, or through another computer you run. One scan pairs a phone.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if store.setUpBefore {
        note("Your paired phones keep working through your server. Install Phone access to add phones, remove them, and keep the copy this Mac serves at home.")
      }
      if AddonStore.shared.elsewhere {
        note(LocalSearchStore.elsewhereText)
      } else if let phase = store.phase {
        AddonProgressView(phase: phase)
      } else {
        switch store.offer {
        case .available(let bytes):
          Button("Install Phone access, \(AddonStore.sizeText(bytes))") { store.install() }
            .disabled(store.busy)
        case .notPublished:
          note("Phone access is not published for download yet. It will be offered here once a release carries it.")
        case .needsNewerApp:
          note("The published Phone access needs a newer MediaViewer. Update MediaViewer, then install it.")
        case .unreachable:
          note("Could not reach the download server.")
          Button("Try again") { store.probe() }
        case .unknown, .checking:
          note("Checking for Phone access\u{2026}")
        }
        if !store.message.isEmpty { note(store.message) }
      }
    }
    .animation(.easeOut(duration: 0.15), value: store.phase == nil)
    // Offered the first time someone opens Phone access (owner, 2026-11-14): asked when it shows, never at launch.
    .onAppear { if store.offer == .unknown || store.offer == .unreachable { store.probe() } }
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(13)).foregroundStyle(MVTheme.body).fixedSize(horizontal: false, vertical: true)
  }
}
