// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Add-ons and the one-time card hint (docs/design/18 "Add-ons: how Import
// is installed"), the Mac twin of IslandHost.Addons.cs.
//
// The download is a plain GET of fixed release-asset URLs: no query, no
// cookies, no identifier, nothing about the user's files (rule 6). The host
// (src/shell/addons_mac.mm) checks the signed manifest before the archive is
// requested and every extracted file before install, and again at every load.
//
// Milestone H: the channel steps live in AddonChannel so Settings → Local
// search (LocalSearchView.swift) installs the AI pack's pieces the same way.
// Import's behaviour is unchanged.
import Foundation
import SwiftUI
import MVChromeBridge

/// One release channel: the fixed URLs of an add-on or piece and the steps
/// every add-on shares. Manifest and signature first; the archive only after
/// the host trusts them; size and SHA-256 before it is opened; the host's full
/// verify-and-install last. A plain GET of a fixed URL: no query, no cookies,
/// no identifier (rule 6, docs/design/17 "The AI pack").
struct AddonChannel: Sendable {
  static let base = "https://github.com/longtimeno-c/mediaviewer/releases/latest/download/"
  /// "mediaviewer-addon-import-macos", "mediaviewer-addon-ai-macos", …
  let name: String
  private var url: String { Self.base + name }

  enum Probe: Equatable, Sendable {
    case available(archiveBytes: Int, installedBytes: Int, version: String, description: String = "",
                   hintText: String = "")
    case notPublished, needsNewerApp, unreachable

    var version: String? {
      if case .available(_, _, let v, _, _) = self { return v }
      return nil
    }
  }

  /// Dotted numeric versions ("0.1.10" > "0.1.9"), as the store compares
  /// them; a published version is an update only when strictly newer.
  static func isNewer(_ published: String, than installed: String) -> Bool {
    let a = published.split(separator: ".").map { Int($0) ?? 0 }
    let b = installed.split(separator: ".").map { Int($0) ?? 0 }
    for i in 0..<max(a.count, b.count) {
      let x = i < a.count ? a[i] : 0, y = i < b.count ? b[i] : 0
      if x != y { return x > y }
    }
    return false
  }

  struct AddonError: Error { let text: String }

  /// What an install is doing, for Settings' progress bar.
  enum Phase: Equatable, Sendable {
    case downloading(done: Int64, total: Int64)
    case checking     // size + SHA-256 of the archive
    case installing   // unpack, then the host verifies every file

    /// nil until the first bytes arrive, or while the size is unknown: the
    /// bar pulses instead of sitting at 0 %.
    var fraction: Double? {
      if case .downloading(let done, let total) = self, total > 0, done > 0 { return min(1, Double(done) / Double(total)) }
      return nil
    }
    var text: String {
      switch self {
      case .downloading(let done, _) where done <= 0:
        return "Connecting…"
      case .downloading(let done, let total):
        return total > 0 ? "\(AddonChannel.mbText(done)) of \(AddonChannel.mbText(total))" : AddonChannel.mbText(done)
      case .checking: return "Checking the download…"
      case .installing: return "Installing…"
      }
    }
  }

  static func mbText(_ bytes: Int64) -> String {
    bytes >= 1_000_000_000 ? String(format: "%.2f GB", Double(bytes) / 1e9)
                           : "\(max(0, Int((Double(bytes) / 1e6).rounded()))) MB"
  }

  /// The channel has no such add-on (a 404): say so rather than blaming the
  /// connection.
  struct NotPublished: Error {}

  /// No cookies, no cache, one fixed User-Agent: a plain request (rule 6).
  static var configuration: URLSessionConfiguration {
    let c = URLSessionConfiguration.ephemeral
    c.httpCookieAcceptPolicy = .never
    c.httpShouldSetCookies = false
    c.urlCache = nil
    c.httpAdditionalHeaders = ["User-Agent": "MediaViewer"]
    return c
  }
  static let session = URLSession(configuration: configuration)

  /// nil for a 404; throws for anything else that is not a 200.
  static func getIfPresent(_ url: String) async throws -> Data? {
    let (data, response) = try await session.data(from: URL(string: url)!)
    let code = (response as? HTTPURLResponse)?.statusCode ?? 0
    if code == 404 { return nil }
    guard code == 200 else { throw URLError(.badServerResponse) }
    return data
  }

  static func checkManifest(_ manifest: Data, _ sig: Data) -> [String: Any] {
    let json = manifest.withUnsafeBytes { m in
      sig.withUnsafeBytes { s in
        AddonStore.readString {
          mv_addons_check_manifest(m.bindMemory(to: UInt8.self).baseAddress, Int32(m.count),
                                   s.bindMemory(to: UInt8.self).baseAddress, Int32(s.count), $0, $1)
        }
      }
    }
    return (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
  }

  /// Two small GETs, then the host verifies the manifest. Only a manifest that
  /// verifies counts: nothing is offered that does not exist or that the host
  /// would refuse.
  func probe() async -> Probe {
    do {
      guard let manifest = try await Self.getIfPresent(url + ".json"),
            let sig = try await Self.getIfPresent(url + ".json.sig")
      else { return .notPublished }
      let obj = Self.checkManifest(manifest, sig)
      if obj["ok"] as? Bool == true,
         let archive = obj["archive"] as? [String: Any], let size = archive["size"] as? Int {
        return .available(archiveBytes: size, installedBytes: obj["installed_size"] as? Int ?? 0,
                          version: obj["version"] as? String ?? "",
                          description: obj["description"] as? String ?? "",
                          hintText: obj["hint_text"] as? String ?? "")
      }
      // A signed add-on for a newer host API; anything else that does not
      // verify is, to this build, nothing to offer.
      return obj["why"] as? String == "needs_update" ? .needsNewerApp : .notPublished
    } catch {
      return .unreachable
    }
  }

  // Manifest (signature first), archive (size + SHA-256 before it is
  // opened), extraction into staging with ditto, then the host's full
  // verify-and-install. Worker only.
  func downloadAndInstall(progress: (@Sendable (Phase) -> Void)? = nil) async throws {
    guard let manifest = try await Self.getIfPresent(url + ".json"),
          let sig = try await Self.getIfPresent(url + ".json.sig")
    else { throw NotPublished() }
    let obj = Self.checkManifest(manifest, sig)
    guard obj["ok"] as? Bool == true,
          let archive = obj["archive"] as? [String: Any],
          let name = archive["path"] as? String,
          let sha = archive["sha256"] as? String,
          let size = archive["size"] as? Int
    else {
      throw AddonError(text: "The download did not verify, so nothing was installed.")
    }
    let staging = AddonStore.readString { mv_addons_make_staging($0, $1) }
    guard !staging.isEmpty else { throw AddonError(text: "Could not prepare the add-ons folder.") }
    let zip = staging + ".zip"
    defer {
      try? FileManager.default.removeItem(atPath: zip)
      try? FileManager.default.removeItem(atPath: staging)
    }
    progress?(.downloading(done: 0, total: Int64(size)))
    // Byte progress from the download task's own delegate (AddonDownload.swift),
    // ~10 updates a second. The signed manifest's size stands in when the
    // server sends no length.
    let expected = Int64(size)
    var report: (@Sendable (Int64, Int64) -> Void)?
    if let progress {
      report = { @Sendable done, total in progress(.downloading(done: done, total: total > 0 ? total : expected)) }
    }
    let code = try await AddonDownload.run(URL(string: Self.base + name)!, to: URL(fileURLWithPath: zip),
                                           configuration: Self.configuration, progress: report)
    guard code == 200 else { throw NotPublished() }
    progress?(.checking)
    let attrs = try FileManager.default.attributesOfItem(atPath: zip)
    let got = AddonStore.readString { mv_addons_sha256(zip, $0, $1) }
    guard (attrs[.size] as? Int) == size, got == sha else {
      throw AddonError(text: "The download did not verify, so nothing was installed.")
    }
    // Authenticated bytes only from here. ditto keeps the bundle's code
    // signature intact, which library validation checks at load.
    progress?(.installing)
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

/// Add-on updates that installed beside a running copy and take over only at
/// the next start ("Import 0.2.0", "Local search 0.3.1"): a loaded bundle
/// cannot be replaced in the running app. While any is here the command bar
/// and Settings offer a restart (owner, 2026-10-05). Never a forced restart
/// (docs/design/13 "Never interrupt"). The Mac twin of IslandHost.Update.cs.
@MainActor
final class AddonRestart: ObservableObject {
  static let shared = AddonRestart()
  @Published private(set) var pending: [String] = []

  func needed(_ what: String) {
    if !pending.contains(what) { pending.append(what) }
  }

  var text: String {
    let one = pending.count == 1
    return "\(pending.joined(separator: " and ")) \(one ? "is" : "are") installed. "
      + "Restart MediaViewer to use \(one ? "it" : "them")."
  }

  /// Onto the same folder and file: through Sparkle when an app update is
  /// staged too (it installs both), else MediaViewer opens again once this
  /// process has exited.
  func restart() { mv_chrome_restart_for_addons() }
}

/// Settings' "Restart now", under an add-on's line, while an update waits on it.
struct AddonRestartButton: View {
  @ObservedObject var restart = AddonRestart.shared

  var body: some View {
    if !restart.pending.isEmpty {
      Button("Restart now") { restart.restart() }.help(restart.text)
    }
  }
}

@MainActor
final class AddonStore: ObservableObject {
  static let shared = AddonStore()

  @Published private(set) var installed = false
  /// The first installed-state read has landed. Until then nothing offers an
  /// install: that read verifies every installed file (seconds), and a probe
  /// of the channel answering first made Settings offer Import again on every
  /// start (owner report, 2026-09-27).
  @Published private(set) var stateKnown = false
  @Published private(set) var version = ""
  @Published private(set) var state = ""
  /// docs/design/25: Settings' line and the card hint, from the installed manifest
  /// or the channel's; empty for one from before it said any.
  @Published private(set) var description = ""
  @Published private(set) var hintText = ""
  @Published private(set) var loaded = false
  @Published private(set) var busy = false
  /// The install in progress, for the bar under the button.
  @Published private(set) var phase: AddonChannel.Phase?
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
  /// The version the channel offers, once probed.
  @Published private(set) var published = ""
  /// A newer Import than the installed one, or nil.
  var updateVersion: String? {
    guard installed, !published.isEmpty, AddonChannel.isNewer(published, than: version) else { return nil }
    return published
  }
  private var probing = false
  /// The card hint checks the channel at most once a session.
  private var hintProbed = false

  nonisolated private static let channel = AddonChannel(name: "mediaviewer-addon-import-macos")
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    // Issue #178: an install's progress moves off the main thread, so this one
    // still ticks; the tolerance lets the system fire it with other wake-ups.
    timer?.tolerance = 0.1
    refresh()
  }

  private func poll() {
    let s = Self.readString { mv_addons_status($0, $1) }
    if s != status { status = s }
    let pending = mv_addons_hint_pending() && stateKnown && !installed
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
        if let text = obj["description"] as? String, !text.isEmpty { self.description = text }
        if let text = obj["hint_text"] as? String, !text.isEmpty { self.hintText = text }
        self.loaded = mv_addons_loaded()
        self.stateKnown = true
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
    // Installed too: Settings offers a newer version (docs/design/18).
    guard !probing else { return }
    probing = true
    offer = .checking
    Task.detached {
      let result = await Self.channel.probe()
      await MainActor.run {
        self.probing = false
        switch result {
        case .available(let archiveBytes, _, let v, let description, let hintText):
          self.offer = .available(archiveBytes: archiveBytes)
          self.published = v
          if self.description.isEmpty, !description.isEmpty { self.description = description }
          if self.hintText.isEmpty, !hintText.isEmpty { self.hintText = hintText }
        case .notPublished: self.offer = .notPublished
        case .needsNewerApp: self.offer = .needsNewerApp
        case .unreachable: self.offer = .unreachable
        }
      }
    }
  }

  func install() {
    guard !busy, stateKnown else { return }
    busy = true
    // An update of a running Import installs beside it and takes over at the
    // next start: a loaded bundle cannot be replaced in the running app.
    let update = updateVersion
    let running = loaded
    message = update.map { "Downloading Import \($0)…" } ?? "Downloading Import…"
    mv_addons_hint_done(false)
    Task.detached {
      let result: String
      do {
        try await Self.channel.downloadAndInstall { p in
          Task { @MainActor in self.phase = p }
        }
        result = "Import installed."
      } catch is AddonChannel.NotPublished {
        result = ""
        await MainActor.run { self.offer = .notPublished }
      } catch let e as AddonChannel.AddonError {
        result = e.text
      } catch {
        result = "Import could not be downloaded. Check the connection and try again."
      }
      await MainActor.run {
        self.busy = false
        self.phase = nil
        self.message = result
        if result == "Import installed." {
          if let v = update, running {
            self.message = "Import \(v) is installed. Restart MediaViewer to use it."
            AddonRestart.shared.needed("Import \(v)")
          } else if !mv_addons_load() {
            self.message = "Import is installed but did not pass verification, so it was not loaded."
          } else if let v = update {
            self.message = "Import updated to \(v)."
          }
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
}

/// The Settings section.
struct AddonsSection: View {
  @ObservedObject var store = AddonStore.shared
  @ObservedObject var settings = SettingsStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Add-ons").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
      Text("Import").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      // docs/design/25: the manifest's line; the built-in one for a manifest from
      // before it carried any.
      Text(store.description.isEmpty
           ? "Copy a card or folder into your library: skips what is already there by content, verifies every copy, sorts by date. Never deletes from the card."
           : store.description)
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if !store.stateKnown {
        // Never an Install button while what is installed is unknown.
        note("Checking installed add-ons…")
      } else if !store.installed {
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
          if let v = store.updateVersion, case .available(let bytes) = store.offer {
            Button("Update to \(v), \(AddonStore.sizeText(bytes))") { store.install() }
              .disabled(store.busy)
          }
          if store.state != "ok" { Button("Reinstall") { store.install() }.disabled(store.busy) }
          if store.loaded { Button("Open Import") { mv_addons_open_import() } }
          Button("Remove…") { store.confirmingRemove = true }
        }
      }
      if let phase = store.phase {
        AddonProgressView(phase: phase)
      } else if !store.message.isEmpty {
        Text(store.message).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
      AddonRestartButton()
      // The second add-on: install-only until Core is loaded, then the pack's
      // own management view (LocalSearchView.swift).
      LocalSearchSection().padding(.top, 16)
      // docs/design/25: add-ons from other makers, under their own heading.
      OpenAddonsSection().padding(.top, 16)
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
    // docs/design/25: the manifest's own words, with the size.
    let base = store.hintText.isEmpty ? "Card inserted — install Import?" : store.hintText
    if case .available(let bytes) = store.offer {
      let size = "(\(AddonStore.sizeText(bytes)))"
      return base.hasSuffix("?") ? "\(base.dropLast()) \(size)?" : "\(base) \(size)"
    }
    return base
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

/// An add-on install's progress: a determinate bar and "412 MB of 1.08 GB"
/// while downloading, then the checking / installing steps (indeterminate:
/// hashing a gigabyte takes seconds and has no fraction to show). Whatever
/// the phase the bar shows activity: an indeterminate phase sweeps a pulse
/// across it (a still, dimmer bar with Reduce Motion), so a row never sits
/// on a frozen 0 % (owner report, 2026-09-28).
struct AddonProgressView: View {
  let phase: AddonChannel.Phase
  var width: CGFloat? = 320

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      ActivityBar(fraction: phase.fraction)
      HStack {
        Text(phase.text)
        Spacer()
        if let f = phase.fraction { Text("\(Int((f * 100).rounded(.down))) %").monospacedDigit() }
      }
      .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
    }
    .frame(maxWidth: width, alignment: .leading)
    .accessibilityElement(children: .combine)
  }
}

/// A thin capsule bar. With a fraction it fills to it; without one it is
/// indeterminate: a soft accent pulse sweeps across (TimelineView, so it only
/// ticks while on screen), or with Reduce Motion a still, half-strength fill.
struct ActivityBar: View {
  let fraction: Double?
  var height: CGFloat = 6
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(Color.primary.opacity(0.10))
        if let f = fraction {
          Capsule().fill(MVTheme.accent)
            .frame(width: max(height, geo.size.width * f))
            .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: f)
        } else if reduceMotion {
          Capsule().fill(MVTheme.accent.opacity(0.45))
        } else {
          TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
            let period = 1.4
            let t = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: period) / period
            let pulse = geo.size.width * 0.35
            // An eased sweep from off the left edge to off the right edge.
            let eased = 0.5 - 0.5 * cos(t * .pi)
            LinearGradient(colors: [MVTheme.accent.opacity(0), MVTheme.accent, MVTheme.accent.opacity(0)],
                           startPoint: .leading, endPoint: .trailing)
              .frame(width: pulse)
              .offset(x: -pulse + (geo.size.width + pulse) * eased)
          }
        }
      }
      .clipShape(Capsule())
    }
    .frame(height: height)
    .accessibilityElement()
    .accessibilityLabel("Progress")
    .accessibilityValue(fraction.map { "\(Int(($0 * 100).rounded(.down))) percent" } ?? "In progress")
  }
}
