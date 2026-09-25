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
    let h = mv_addons_hint_pending() && !installed
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

  nonisolated private static let session: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.httpCookieAcceptPolicy = .never
    c.httpShouldSetCookies = false
    c.urlCache = nil
    c.httpAdditionalHeaders = ["User-Agent": "MediaViewer"]
    return URLSession(configuration: c)
  }()

  nonisolated private static func get(_ url: String) async throws -> Data {
    let (data, response) = try await session.data(from: URL(string: url)!)
    guard (response as? HTTPURLResponse)?.statusCode == 200 else {
      throw AddonError(text: "Import is not available for download right now.")
    }
    return data
  }

  // Manifest (signature first), archive (size + SHA-256 before it is
  // opened), extraction into staging with ditto, then the host's full
  // verify-and-install. Worker only.
  nonisolated private static func downloadAndInstall() async throws {
    let manifest = try await get(channel + ".json")
    let sig = try await get(channel + ".json.sig")
    let check = manifest.withUnsafeBytes { m in
      sig.withUnsafeBytes { s in
        readString {
          mv_addons_check_manifest(m.bindMemory(to: UInt8.self).baseAddress, Int32(m.count),
                                   s.bindMemory(to: UInt8.self).baseAddress, Int32(s.count), $0, $1)
        }
      }
    }
    guard let obj = (try? JSONSerialization.jsonObject(with: Data(check.utf8))) as? [String: Any],
          obj["ok"] as? Bool == true,
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
    guard (response as? HTTPURLResponse)?.statusCode == 200 else {
      throw AddonError(text: "Import is not available for download right now.")
    }
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

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Add-ons").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
      Text("Import").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      Text("Copy a card or folder into your library: skips what is already there by content, verifies every copy, sorts by date. Never deletes from the card.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if !store.installed {
        Button("Install Import, 3 MB") { store.install() }.disabled(store.busy)
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
  }
}

/// The command bar's one-time hint and the running-import status line.
struct AddonBarItems: View {
  @ObservedObject var store = AddonStore.shared

  var body: some View {
    HStack(spacing: 6) {
      if store.hint {
        Button("Card inserted — install Import (3 MB)?") { store.install() }
          .help("Import copies a card into your library, skips what is already there, and verifies every copy. An optional add-on.")
        Button("Not now") { store.dismissHint() }
      }
      if !store.status.isEmpty {
        Text(store.status).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
    }
  }
}
