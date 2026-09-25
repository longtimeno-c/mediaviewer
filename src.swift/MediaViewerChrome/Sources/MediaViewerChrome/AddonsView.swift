// SPDX-License-Identifier: GPL-2.0-or-later
// Settings → Add-ons and the one-time card hint (plan/18 "Add-ons: how Import
// is installed"), the Mac twin of IslandHost.Addons.cs.
//
// The download is a plain GET of fixed release-asset URLs: no query, no
// cookies, no identifier, nothing about the user's files (rule 6). The host
// (src/shell/addons_mac.mm) checks the signed manifest before the archive is
// requested and every extracted file before install, and again at every load.
import Foundation
import SwiftUI
import MVChromeBridge

@MainActor
final class AddonStore: ObservableObject {
  static let shared = AddonStore()

  @Published private(set) var installed = false
  @Published private(set) var version = ""
  @Published private(set) var state = ""
  @Published private(set) var loaded = false
  @Published private(set) var busy = false
  @Published var message = ""
  @Published private(set) var status = ""
  @Published private(set) var hint = false
  @Published var confirmingRemove = false

  /// Whether the release channel has an Import this build can install. Only a
  /// manifest that verifies counts: the button never offers a download that
  /// does not exist or that the host would refuse.
  enum Offer: Equatable {
    case unknown, checking, available(archiveBytes: Int), notPublished, needsNewerApp, unreachable
  }
  @Published private(set) var offer: Offer = .unknown
  private var probing = false
  /// The card hint checks the channel at most once a session.
  private var hintProbed = false

  nonisolated private static let base = "https://github.com/longtimeno-c/mediaviewer/releases/latest/download/"
  nonisolated private static let channel = base + "mediaviewer-addon-import-macos"
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    refresh()
  }

  private func poll() {
    let s = Self.readString { mv_addons_status($0, $1) }
    if s != status { status = s }
    let pending = mv_addons_hint_pending() && !installed
    if pending, !hintProbed, Self.automaticChecksOn(), offer == .unknown || offer == .unreachable {
      hintProbed = true
      probe()
    }
    // Only when there is something to install.
    let h: Bool
    if case .available = offer { h = pending } else { h = false }
    if h != hint { hint = h }
    let l = mv_addons_loaded()
    if l != loaded { loaded = l }
  }

  nonisolated static func readString(_ call: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> String {
    let need = Int(call(nil, 0))
    guard need > 0 else { return "" }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }

  func refresh() {
    Task.detached {
      let json = Self.readString { mv_addons_state_json($0, $1) }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      await MainActor.run {
        self.installed = obj["installed"] as? Bool ?? false
        self.version = obj["version"] as? String ?? ""
        self.state = obj["state"] as? String ?? ""
        self.loaded = mv_addons_loaded()
      }
    }
  }

  func dismissHint() { mv_addons_hint_done(true) }

  /// Checking the channel is a network call like the update check, so a card
  /// arriving waits on the same switch (Sparkle's, in the app menu). With it
  /// off, Import is offered from Settings only.
  nonisolated private static func automaticChecksOn() -> Bool {
    if let v = UserDefaults.standard.object(forKey: "SUEnableAutomaticChecks") as? Bool { return v }
    return Bundle.main.object(forInfoDictionaryKey: "SUEnableAutomaticChecks") as? Bool ?? false
  }

  static func sizeText(_ bytes: Int) -> String {
    "\(max(1, Int((Double(bytes) / 1_048_576).rounded()))) MB"
  }

  /// Settings opening, the hint, or Try again: two small GETs of fixed URLs
  /// (the same channel as updates), then the host verifies the manifest.
  func probe() {
    guard !probing, !installed else { return }
    probing = true
    offer = .checking
    Task.detached {
      let result = await Self.readOffer()
      await MainActor.run {
        self.probing = false
        self.offer = result
      }
    }
  }

  nonisolated private static func readOffer() async -> Offer {
    do {
      guard let manifest = try await getIfPresent(channel + ".json"),
            let sig = try await getIfPresent(channel + ".json.sig")
      else { return .notPublished }
      let obj = checkManifest(manifest, sig)
      if obj["ok"] as? Bool == true,
         let archive = obj["archive"] as? [String: Any], let size = archive["size"] as? Int {
        return .available(archiveBytes: size)
      }
      // A signed Import for a newer host API; anything else that does not
      // verify is, to this build, nothing to offer.
      return obj["why"] as? String == "needs_update" ? .needsNewerApp : .notPublished
    } catch {
      return .unreachable
    }
  }

  nonisolated private static func checkManifest(_ manifest: Data, _ sig: Data) -> [String: Any] {
    let json = manifest.withUnsafeBytes { m in
      sig.withUnsafeBytes { s in
        readString {
          mv_addons_check_manifest(m.bindMemory(to: UInt8.self).baseAddress, Int32(m.count),
                                   s.bindMemory(to: UInt8.self).baseAddress, Int32(s.count), $0, $1)
        }
      }
    }
    return (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
  }

  func install() {
    guard !busy else { return }
    busy = true
    message = "Downloading Import…"
    mv_addons_hint_done(false)
    Task.detached {
      let result: String
      do {
        try await Self.downloadAndInstall()
        result = "Import installed."
      } catch is NotPublished {
        result = ""
        await MainActor.run { self.offer = .notPublished }
      } catch let e as AddonError {
        result = e.text
      } catch {
        result = "Import could not be downloaded. Check the connection and try again."
      }
      await MainActor.run {
        self.busy = false
        self.message = result
        if result == "Import installed." && !mv_addons_load() {
          self.message = "Import is installed but did not pass verification, so it was not loaded."
        }
        self.refresh()
      }
    }
  }

  func remove(keepData: Bool) {
    confirmingRemove = false
    message = mv_addons_remove(keepData)
      ? "Import removed."
      : "Import will finish uninstalling the next time MediaViewer starts."
    refresh()
  }

  struct AddonError: Error { let text: String }
  /// The channel has no Import (a 404): the section says so rather than
  /// blaming the connection.
  struct NotPublished: Error {}

  nonisolated private static let session: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.httpCookieAcceptPolicy = .never
    c.httpShouldSetCookies = false
    c.urlCache = nil
    c.httpAdditionalHeaders = ["User-Agent": "MediaViewer"]
    return URLSession(configuration: c)
  }()

  /// nil for a 404; throws for anything else that is not a 200.
  nonisolated private static func getIfPresent(_ url: String) async throws -> Data? {
    let (data, response) = try await session.data(from: URL(string: url)!)
    let code = (response as? HTTPURLResponse)?.statusCode ?? 0
    if code == 404 { return nil }
    guard code == 200 else { throw URLError(.badServerResponse) }
    return data
  }

  // Manifest (signature first), archive (size + SHA-256 before it is
  // opened), extraction into staging with ditto, then the host's full
  // verify-and-install. Worker only.
  nonisolated private static func downloadAndInstall() async throws {
    guard let manifest = try await getIfPresent(channel + ".json"),
          let sig = try await getIfPresent(channel + ".json.sig")
    else { throw NotPublished() }
    let obj = checkManifest(manifest, sig)
    guard obj["ok"] as? Bool == true,
          let archive = obj["archive"] as? [String: Any],
          let name = archive["path"] as? String,
          let sha = archive["sha256"] as? String,
          let size = archive["size"] as? Int
    else {
      throw AddonError(text: "The download did not verify, so nothing was installed.")
    }
    let staging = readString { mv_addons_make_staging($0, $1) }
    guard !staging.isEmpty else { throw AddonError(text: "Could not prepare the add-ons folder.") }
    let zip = staging + ".zip"
    defer {
      try? FileManager.default.removeItem(atPath: zip)
      try? FileManager.default.removeItem(atPath: staging)
    }
    let (tmp, response) = try await session.download(from: URL(string: base + name)!)
    guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw NotPublished() }
    try FileManager.default.moveItem(at: tmp, to: URL(fileURLWithPath: zip))
    let attrs = try FileManager.default.attributesOfItem(atPath: zip)
    let got = readString { mv_addons_sha256(zip, $0, $1) }
    guard (attrs[.size] as? Int) == size, got == sha else {
      throw AddonError(text: "The download did not verify, so nothing was installed.")
    }
    // Authenticated bytes only from here. ditto keeps the bundle's code
    // signature intact, which library validation checks at load.
    let ditto = Process()
    ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
    ditto.arguments = ["-x", "-k", zip, staging]
    try ditto.run()
    ditto.waitUntilExit()
    guard ditto.terminationStatus == 0 else {
      throw AddonError(text: "The download could not be unpacked.")
    }
    try manifest.write(to: URL(fileURLWithPath: staging + "/manifest.json"))
    try sig.write(to: URL(fileURLWithPath: staging + "/manifest.json.sig"))
    guard mv_addons_install(staging) else {
      throw AddonError(text: "The download did not verify, so nothing was installed.")
    }
  }
}

/// The Settings section.
struct AddonsSection: View {
  @ObservedObject var store = AddonStore.shared
  @ObservedObject var settings = SettingsStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Add-ons").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
      Text("Import").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      Text("Copy a card or folder into your library: skips what is already there by content, verifies every copy, sorts by date. Never deletes from the card.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if !store.installed {
        switch store.offer {
        case .available(let bytes):
          Button("Install Import, \(AddonStore.sizeText(bytes))") { store.install() }.disabled(store.busy)
        case .notPublished:
          note("Import is not published for download yet. It will be offered here once a release carries it.")
        case .needsNewerApp:
          note("The published Import needs a newer MediaViewer. Update MediaViewer, then install it.")
        case .unreachable:
          note("Could not reach the download server.")
          Button("Try again") { store.probe() }
        case .unknown, .checking:
          note("Checking for Import…")
        }
      } else if store.confirmingRemove {
        Text("Remove Import? Its library index and import history can stay, so a reinstall still knows what was imported.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("Remove, keep history") { store.remove(keepData: true) }
          Button("Remove everything") { store.remove(keepData: false) }
          Button("Cancel") { store.confirmingRemove = false }
        }
      } else {
        Text(store.state == "ok" ? "Installed, version \(store.version)."
             : store.state == "needs_update" ? "Version \(store.version) needs an update to work with this MediaViewer."
             : "The installed copy did not pass verification and is not loaded.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        HStack {
          if store.state != "ok" { Button("Reinstall") { store.install() }.disabled(store.busy) }
          if store.loaded { Button("Open Import") { mv_addons_open_import() } }
          Button("Remove…") { store.confirmingRemove = true }
        }
      }
      if !store.message.isEmpty {
        Text(store.message).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
    }
    // Opening Settings asks the channel, whatever the automatic-check switch
    // says: the person is looking at what can be installed.
    .onAppear { if settings.visible { store.probe() } }
    .onChange(of: settings.visible) { _, visible in if visible { store.probe() } }
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }
}

/// The command bar's one-time hint and the running-import status line.
struct AddonBarItems: View {
  @ObservedObject var store = AddonStore.shared

  private var hintText: String {
    if case .available(let bytes) = store.offer {
      return "Card inserted — install Import (\(AddonStore.sizeText(bytes)))?"
    }
    return "Card inserted — install Import?"
  }

  var body: some View {
    HStack(spacing: 6) {
      if store.hint {
        Button(hintText) { store.install() }
          .help("Import copies a card into your library, skips what is already there, and verifies every copy. An optional add-on.")
        Button("Not now") { store.dismissHint() }
      }
      if !store.status.isEmpty {
        Text(store.status).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
    }
  }
}
