// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access, part of the app (docs/plans/phone-in-the-app.md,
// owner 2026-11-12): the NAS folder, what the phone searches, the service on
// the NAS (claim, addresses, named folders), the code a phone scans with its
// check code, the paired phones, and handing the library to or from another
// computer. Works with Local search or without it. The work is the app's own
// thread's (phone/link.h, through mv_chrome_phone_*); this only asks and
// watches. Keys and addresses shown here never reach a log (rule 6). The
// Windows twin is IslandHost.Phone.cs, with the same words (Shared/PhoneWords.cs).
import AppKit
import CoreImage
import Foundation
import MVChromeBridge
import SwiftUI

/// The QR for a phone: error correction M, black modules enlarged without
/// smoothing, on white (the card draws the quiet zone).
enum PhoneQR {
  static func image(for text: String, side: CGFloat) -> CGImage? {
    guard let filter = CIFilter(name: "CIQRCodeGenerator") else { return nil }
    filter.setValue(Data(text.utf8), forKey: "inputMessage")
    filter.setValue("M", forKey: "inputCorrectionLevel")
    guard let output = filter.outputImage else { return nil }
    let scale = max(1, (side / output.extent.width).rounded(.down))
    let big = output.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
    return CIContext(options: [.useSoftwareRenderer: true]).createCGImage(big, from: big.extent)
  }
}

@MainActor
final class PhoneStore: ObservableObject {
  static let shared = PhoneStore()

  struct Phone: Identifiable, Equatable {
    let name: String
    let key: String
    var limited = false
    var expires: Int64 = 0  // unix on the NAS's clock, 0 = no end
    var expired = false
    var guest = false
    /// May back up to the backup folders (phone backup, docs/plans/mobile.md §10b.1, B2).
    var backup = true
    var id: String { key }
  }
  struct Folder: Identifiable, Equatable {
    let id: String
    let label: String
    let path: String
    var isCatalogue: Bool { label == "Catalogue" }
    // Phone backup (1.1): phones may create files here, under these sizes.
    var backup = false
    var limit: Int64 = 0
    var reserve: Int64 = 0
    var used: Int64 = 0
    var room: Int64 = 0
    var indexed = false
  }
  /// Local search's catalogue rows (its phone_json, as the link passes it on).
  struct Catalogue: Equatable {
    var running = false
    var phase = ""
    var fraction = 0.0
    var error = ""
    var published: Int64 = 0
    var assets: Int64 = 0
  }
  struct State: Equatable {
    var starting = false
    var secondary = false
    var driftwire = true
    var busy = ""
    var error = ""
    var folder = ""
    var libraryName = ""
    var localSearch = false
    var catalogue: Catalogue?
    var claimed = false
    var reachable = true
    var local = ""
    var publicName = ""
    var warning = ""
    var phones: [Phone] = []
    var folders: [Folder] = []
    var qr = ""
    var qrLibrary = ""
    var qrFingerprint = ""
    var qrExpires: Int64 = 0
    var added: [String] = []
    var otherPublisher = false
    var mismatch = false
    var otherFingerprint = ""
    var askingName = ""
    var askingKey = ""
    var askingFingerprint = ""
    var nasPassphrase = ""  // "set" | "none" | "broken" | "" (a NAS too old to say)
    var nasFeatures: [String] = []
    var unlockedSeconds: Int64 = 0
    var lockedUntil: Int64 = 0
    var qrForSeconds: Int64 = 0
    var relay = ""  // the person's own relay, "host:port"; its key stays on the NAS
    /// The NAS takes guest codes, time limits and a passphrase.
    var nasHasFeatures: Bool { nasFeatures.contains("passphrase") }
    /// The NAS can wait at the person's own relay (docs/plans/nas-relay.md).
    var nasHasRelay: Bool { nasFeatures.contains("relay") }
  }

  @Published private(set) var state = State()
  private var timer: Timer?

  private init() {}

  /// While Settings shows the card: read now and every half second (a small
  /// JSON from memory; nothing touches the network on this thread).
  func appeared() {
    poll()
    guard timer == nil else { return }
    let t = Timer(timeInterval: 0.5, repeats: true) { _ in
      Task { @MainActor in PhoneStore.shared.poll() }
    }
    RunLoop.main.add(t, forMode: .common)  // goes on while Settings is scrolled
    timer = t
  }

  func disappeared() {
    timer?.invalidate()
    timer = nil
  }

  func poll() {
    var size = 16 * 1024
    var text = ""
    for _ in 0..<3 {
      var buf = [CChar](repeating: 0, count: size)
      let need = Int(mv_chrome_phone_state(&buf, Int32(buf.count)))
      if need < size { text = String(cString: buf); break }  // need counts no NUL
      size = need + 1024
    }
    guard let o = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] else { return }
    let s = Self.parse(o)
    if s != state { state = s }
  }

  nonisolated static func parse(_ o: [String: Any]) -> State {
    var s = State()
    s.starting = o["starting"] as? Bool ?? false
    s.secondary = o["secondary"] as? Bool ?? false
    s.driftwire = o["driftwire"] as? Bool ?? true
    s.busy = o["busy"] as? String ?? ""
    s.error = o["error"] as? String ?? ""
    s.folder = o["folder"] as? String ?? ""
    s.libraryName = o["library_name"] as? String ?? ""
    s.localSearch = o["local_search"] as? Bool ?? false
    if let c = o["catalogue"] as? [String: Any] {
      var cat = Catalogue()
      cat.running = c["running"] as? Bool ?? false
      cat.phase = c["phase"] as? String ?? ""
      cat.fraction = (c["fraction"] as? NSNumber)?.doubleValue ?? 0
      cat.error = c["error"] as? String ?? ""
      cat.published = (c["published"] as? NSNumber)?.int64Value ?? 0
      cat.assets = (c["assets"] as? NSNumber)?.int64Value ?? 0
      s.catalogue = cat
    }
    s.claimed = o["claimed"] as? Bool ?? false
    s.reachable = o["reachable"] as? Bool ?? true
    s.local = o["local"] as? String ?? ""
    s.publicName = o["public"] as? String ?? ""
    s.warning = o["warning"] as? String ?? ""
    s.phones = (o["devices"] as? [[String: Any]] ?? []).compactMap {
      guard let n = $0["name"] as? String, let k = $0["key"] as? String else { return nil }
      let guest = $0["guest"] as? Bool ?? false
      return Phone(name: n, key: k, limited: $0["limited"] as? Bool ?? false,
                   expires: ($0["expires"] as? NSNumber)?.int64Value ?? 0,
                   expired: $0["expired"] as? Bool ?? false, guest: guest,
                   backup: $0["backup"] as? Bool ?? !guest)
    }
    s.folders = (o["folders"] as? [[String: Any]] ?? []).compactMap {
      guard let i = $0["id"] as? String, let l = $0["label"] as? String, let p = $0["path"] as? String else { return nil }
      var f = Folder(id: i, label: l, path: p)
      f.backup = $0["backup"] as? Bool ?? false
      f.limit = ($0["limit"] as? NSNumber)?.int64Value ?? 0
      f.reserve = ($0["reserve"] as? NSNumber)?.int64Value ?? 0
      f.used = ($0["used"] as? NSNumber)?.int64Value ?? 0
      f.room = ($0["room"] as? NSNumber)?.int64Value ?? 0
      f.indexed = $0["indexed"] as? Bool ?? false
      return f
    }
    s.qr = o["qr"] as? String ?? ""
    s.qrLibrary = o["qr_library"] as? String ?? ""
    s.qrFingerprint = o["qr_fingerprint"] as? String ?? ""
    s.qrExpires = (o["qr_expires"] as? NSNumber)?.int64Value ?? 0
    s.added = o["added"] as? [String] ?? []
    s.otherPublisher = o["other_publisher"] as? Bool ?? false
    s.mismatch = o["mismatch"] as? Bool ?? false
    s.otherFingerprint = o["other_fingerprint"] as? String ?? ""
    s.askingName = o["asking_name"] as? String ?? ""
    s.askingKey = o["asking_key"] as? String ?? ""
    s.askingFingerprint = o["asking_fingerprint"] as? String ?? ""
    s.nasPassphrase = o["nas_passphrase"] as? String ?? ""
    s.nasFeatures = o["nas_features"] as? [String] ?? []
    s.unlockedSeconds = (o["unlocked_s"] as? NSNumber)?.int64Value ?? 0
    s.lockedUntil = (o["locked_until"] as? NSNumber)?.int64Value ?? 0
    s.qrForSeconds = (o["qr_for_s"] as? NSNumber)?.int64Value ?? 0
    s.relay = o["relay"] as? String ?? ""
    return s
  }

  func send(_ request: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: request),
          let text = String(data: data, encoding: .utf8) else { return }
    _ = mv_chrome_phone_request(text)
    poll()
  }

  func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.prompt = "Choose"
    panel.message = "Choose a folder on the NAS that holds your library."
    guard panel.runModal() == .OK, let url = panel.url else { return }
    send(["do": "folder", "path": url.path])
  }

  // ---- words (the Windows twin's are Shared/PhoneWords.cs) -----------------------------

  /// What a new named folder is called unless the person says otherwise: its
  /// share's name, which is how a phone matches a catalogue's roots to the
  /// service's. On a Synology a share is the folder right under /volumeN.
  nonisolated static func shareLabel(forPath path: String) -> String {
    let parts = path.split(separator: "/").map(String.init)
    if parts.count >= 2, parts[0].hasPrefix("volume"), Int(parts[0].dropFirst(6)) != nil { return parts[1] }
    return parts.last ?? ""
  }

  var errorText: String? {
    switch state.error {
    case "": return nil
    case "refused": return "The service said no. It takes a claim only from its own network, and only once."
    case "unreachable": return "Couldn't reach the service."
    case "bad_address": return "That isn't an address. Use name:port."
    case "too_many": return "Too many phones are paired. Remove one first."
    case "no_folder": return "Choose the folder on your NAS first."
    case "not_published": return "Choose the folder on your NAS first."
    case "other_library": return "That folder holds another library's catalogue."
    case "no_key_store": return "This copy of MediaViewer can't keep a signing key."
    case "revoke_waits": return "Removed here. The NAS will be told when it can be reached."
    case "no_driftwire": return "This copy of MediaViewer can't keep phone keys."
    case "no_local_search": return "Install Local search to publish a catalogue."
    case "needs_unlock": return "Enter the NAS passphrase to do that."
    case "passphrase": return "That is not the NAS passphrase."
    case "passphrase_short": return "A passphrase needs at least 10 characters."
    case "no_passphrase": return "This NAS has no passphrase yet. Enter a new one twice to choose it."
    case "slow_down": return "Try again in a moment."
    case "wrong_service": return "That is a different NAS from the one this computer claimed."
    case "locked_out":
      return "Too many tries. Try again at \(Self.clock(state.lockedUntil)), or reset it on the NAS with driftwire serve passphrase reset."
    case "nas_too_old":
      if state.claimed && (state.nasPassphrase.isEmpty || state.nasPassphrase == "none") {
        return "Claimed. Update the NAS service to protect it with a passphrase."
      }
      return "Update the NAS service to use this."
    default: return "That didn't work."
    }
  }

  nonisolated static func clock(_ unix: Int64) -> String {
    Date(timeIntervalSince1970: TimeInterval(unix)).formatted(date: .omitted, time: .shortened)
  }

  /// The NAS passphrase row's line.
  var passphraseDetail: String {
    switch state.nasPassphrase {
    case "set":
      return state.unlockedSeconds > 0 ? "Unlocked · \(max(1, (state.unlockedSeconds + 59) / 60)) min" : "Locked"
    case "none": return "None. Choose one so nobody else on your network can claim or change this service."
    case "broken": return "The NAS can't read it. Reset it on the NAS with driftwire serve passphrase reset."
    default: return "Update the NAS service to use this."
    }
  }

  /// The durations a code or a phone's access can have, in the menus' order.
  nonisolated static let durations: [(label: String, seconds: Int64)] = [
    ("1 hour", 3600), ("24 hours", 86400), ("7 days", 7 * 86400), ("30 days", 30 * 86400),
  ]
  nonisolated static let unlimited = "Until I remove it"

  /// What Extend offers a timed phone: lengths that end after its end now.
  nonisolated static func extendDurations(_ p: Phone, now: Date = Date()) -> [(label: String, seconds: Int64)] {
    let t = Int64(now.timeIntervalSince1970)
    return durations.filter { !p.limited || p.expires == 0 || t + $0.seconds > p.expires }
  }

  /// There is no public relay (owner, 2026-11-12): the row says so plainly.
  nonisolated static func relayDetail(_ relay: String, available: Bool) -> String {
    if !available { return "Update the NAS service to reach it away from home without a forwarded port." }
    if relay.isEmpty {
      return "Away from home without a forwarded port, phones need a relay you run yourself (driftwire-relay on your own server). There is no public one."
    }
    return "Phones away from home go through \(relay) when they can't reach the NAS directly. It passes sealed bytes only."
  }

  /// The relay's key is at least 16 characters, as driftwire-relay takes it.
  static let relayKeyMinimum = 16

  nonisolated static func durationText(_ seconds: Int64) -> String {
    switch seconds {
    case 3600: return "1 hour"
    case 86400: return "24 hours"
    case _ where seconds % 86400 == 0: return "\(seconds / 86400) days"
    case _ where seconds % 3600 == 0: return "\(seconds / 3600) hours"
    default: return "\(max(1, seconds / 60)) minutes"
    }
  }

  nonisolated static func guestCodeLine(_ seconds: Int64) -> String {
    "Guest code · access for \(durationText(seconds)) from when it is scanned"
  }

  /// A backup folder's line: its path, sizes, and whether this Mac indexes it (§10b.1, B7).
  nonisolated static func backupDetail(_ f: Folder) -> String {
    let sizes = "\(gigabytes(f.used)) of \(gigabytes(f.limit)) GB used, keeps \(gigabytes(f.reserve)) GB free"
    let indexed = f.indexed
      ? "Indexed by this Mac: what phones send reaches search after the next scan."
      : "Not in Local search on this Mac yet: add this folder there so what phones send reaches search."
    return "\(f.path) · \(sizes). \(indexed)"
  }

  /// Bytes as GB for the card's fields, "500" or "1.5".
  nonisolated static func gigabytes(_ bytes: Int64) -> String {
    let gb = Double(bytes) / 1e9
    if gb >= 10 { return String(Int(gb.rounded())) }
    let text = String(format: "%.2f", gb)
    return text.hasSuffix("0") ? String(text.dropLast()) : text
  }

  /// "500" or "1,5" (GB) to bytes; nil when it is not a number.
  nonisolated static func bytes(_ gigabytes: String) -> Int64? {
    let t = gigabytes.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".")
    guard let v = Double(t), v >= 0, v < 1e6 else { return nil }
    return Int64((v * 1e9).rounded())
  }

  /// A phone row's line: "Guest · Until Fri 14:00", "Guest · Expired yesterday", or its key.
  nonisolated static func phoneDetail(_ p: Phone, now: Date = Date()) -> String {
    // Backup is said only where it is not the usual: a guest that may back up, a phone that may not.
    let lead = p.guest ? (p.backup ? "Guest · Backs up · " : "Guest · ") : (p.backup ? "" : "No backup · ")
    guard p.limited, p.expires != 0 else { return lead + "Key \(p.key.prefix(8))" }
    let end = Date(timeIntervalSince1970: TimeInterval(p.expires))
    if p.expired || end <= now {
      return lead + "Expired " + RelativeDateTimeFormatter().localizedString(for: end, relativeTo: now)
    }
    return lead + "Until " + end.formatted(.dateTime.weekday(.abbreviated).hour().minute())
  }

  var busyText: String? {
    switch state.busy {
    case "claim": return "Claiming the service"
    case "pair": return "Asking the NAS for a code"
    case "refresh": return "Checking the NAS"
    case "revoke": return "Removing the phone"
    case "add_folder", "remove_folder": return "Changing the shared folders"
    case "set_addresses": return "Checking the address"
    case "folder": return "Setting up the folder"
    case "takeover_ask", "takeover_allow", "takeover_restart": return "Updating the library"
    case "unlock": return "Unlocking"
    case "set_passphrase": return "Setting the passphrase"
    case "grant": return "Changing the phone's access"
    default: return nil
    }
  }

  var warningText: String? {
    switch state.warning {
    case "private": return "This name points to a private address, so your phone can't reach it from outside."
    case "carrier": return "This name points to a carrier-grade address, so a forwarded port won't work. Ask your provider for a public address."
    case "unresolved": return "This name doesn't resolve yet."
    default: return nil
    }
  }

  /// The phone's search, said once: smart with Local search, by name and tag without.
  var searchDetail: String {
    guard state.localSearch, let c = state.catalogue else {
      return "Your phone searches file names and tags. Install Local search and index this library for words, faces, places and sounds."
    }
    if c.running { return c.phase == "sending" ? "Sending to the NAS" : "Building the catalogue" }
    switch c.error {
    case "unreachable": return "Couldn't reach that folder. It will need to be mounted."
    case "other_library": return "That folder holds another library's catalogue."
    case "no_space": return "Not enough space on the share."
    case "no_index": return "Nothing is indexed yet. Your phone searches file names and tags until it is."
    case "no_folders": return "None of your search folders are on that NAS."
    case "no_key_store": return "This copy of MediaViewer can't keep a signing key, so it can't publish."
    case "failed": return "The catalogue could not be built."
    case "cancelled": return "Stopped."
    default: break
    }
    guard c.published > 0 else { return "Smart search starts once the catalogue is published." }
    let ago = RelativeDateTimeFormatter().localizedString(for: Date(timeIntervalSince1970: TimeInterval(c.published)),
                                                          relativeTo: Date())
    return "Smart search is on. Published \(ago) · \(c.assets.formatted()) files"
  }
}

struct PhoneAccessSection: View {
  @ObservedObject private var model = PhoneStore.shared
  @ObservedObject private var settings = SettingsStore.shared
  @State private var local = ""
  @State private var publicName = ""
  @State private var newLabel = ""
  @State private var newPath = ""
  @State private var newIsBackup = false
  @State private var newLimitGB = ""
  @State private var newReserveGB = "1"
  @State private var editing: String?  // a backup folder whose sizes are being changed
  @State private var editLimitGB = ""
  @State private var editReserveGB = ""
  @State private var confirmRestart = false
  @State private var confirmHandOver = false
  @State private var passphrase = ""
  @State private var passphraseAgain = ""
  @State private var unlockWith = ""
  @State private var changing = false
  @State private var codeFor: Int64 = 0  // 0: until I remove it
  /// A refused action asked for the passphrase: the field stays until an unlock
  /// lands or the person cancels (the core clears its error as each request starts).
  @State private var unlockWanted = false
  @State private var relayEditing = false
  @State private var relayAddress = ""
  @State private var relayKey = ""
  private var s: PhoneStore.State { model.state }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      if s.secondary {
        note("Set up in the first MediaViewer window.")
      } else if s.starting {
        note("Starting…")
      } else if !s.driftwire {
        note("This copy of MediaViewer can't keep phone keys.")
      } else {
        content
      }
    }
    .onChange(of: s.local) { _, v in if local.isEmpty { local = v } }
    .onChange(of: s.publicName) { _, v in if publicName.isEmpty { publicName = v } }
    .onAppear { if settings.visible { model.appeared() } }  // Settings stays built while hidden
    .onChange(of: s.error) { _, e in if e == "needs_unlock" { unlockWanted = true } }
    .onChange(of: s.unlockedSeconds) { _, left in
      if left > 0 && unlockWanted {
        unlockWanted = false
        unlockWith = ""
      }
    }
    .onDisappear {
      model.disappeared()
      forgetSecrets()
    }
    .onChange(of: settings.visible) { _, visible in
      if visible { model.appeared() } else {
        model.disappeared()
        forgetSecrets()  // typed and never sent: gone when Settings closes
      }
    }
  }

  @ViewBuilder private var content: some View {
    if let line = model.busyText { ProgressLine(verb: line, fraction: nil, timed: false) }
    SettingsRow(title: "Folder on your NAS",
                detail: s.folder.isEmpty ? "Where your phone reads your library. Nothing is written there until you choose it." : s.folder,
                labelsContent: false) {
      SettingsButton(title: s.folder.isEmpty ? "Choose…" : "Change…") { model.chooseFolder() }
        .disabled(!s.busy.isEmpty || (s.catalogue?.running ?? false))
        .accessibilityLabel("Choose the folder on your NAS")
    }
    if !s.folder.isEmpty {
      search
      if s.mismatch {
        SettingsRow(title: "The share doesn't match this Mac",
                    detail: "Its library.json names another library or an older epoch, perhaps after a restore. This Mac goes on publishing its own; phones will refuse what doesn't match until it is fixed.") { EmptyView() }
      }
      if s.otherPublisher { otherPublisher }
      if !s.askingName.isEmpty { asking }
      if !s.claimed { claim } else { claimed }
    }
    if let e = model.errorText {
      Text(e).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  // MARK: what the phone searches

  @ViewBuilder private var search: some View {
    SettingsRow(title: "Search on your phone", detail: model.searchDetail, labelsContent: false) {
      if !s.localSearch {
        SettingsButton(title: "Get Local search…") { AddonNav.shared.show(AddonNav.localSearchPage) }
      } else if !(s.catalogue?.running ?? false) {
        SettingsButton(title: "Publish now") { model.send(["do": "publish"]) }
          .disabled(!s.busy.isEmpty)
      }
    }
    if let c = s.catalogue, c.running {
      // One progress line (docs/design/20 "Craft rules"), ending in a quiet Cancel.
      ProgressLine(verb: c.phase == "sending" ? "Sending to the NAS" : "Building the catalogue",
                   fraction: min(1, max(0, c.fraction)), key: "phone-publish",
                   onCancel: { model.send(["do": "cancel"]) })
        .padding(.horizontal, MVTheme.space(12))
    }
  }

  // MARK: claiming

  private var claim: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      SettingsRow(title: "Service on your NAS",
                  detail: "Claim it from the NAS's own network, once. Your Mac writes a one-time token to the share to prove it can.") {
        SettingsButton(title: "Claim") {
          model.send(claimRequest())
          passphrase = ""
          passphraseAgain = ""
        }
          .accessibilityLabel("Claim the service")
          .disabled(!claimable)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local)
      publicField
      secure("NAS passphrase",
             detail: "The one you set on the NAS with driftwire serve init. If it has none, choose one here: enter it twice, at least 10 characters.",
             text: $passphrase)
      secure("Again, to choose it", detail: "Leave empty when the NAS already has one.", text: $passphraseAgain)
    }
  }

  private func claimRequest() -> [String: Any] {
    var r: [String: Any] = ["do": "claim", "local": local, "public": publicName]
    if !passphrase.isEmpty {
      r["passphrase"] = passphrase
      r["new_passphrase"] = !passphraseAgain.isEmpty
    }
    return r
  }

  private var claimable: Bool {
    s.busy.isEmpty && !local.isEmpty && (passphraseAgain.isEmpty || passphraseAgain == passphrase)
  }

  private var publicField: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(4)) {
      field("Address from outside", hint: "photos.example.com:7878", text: $publicName, onCommit: saveAddresses)
      if let w = model.warningText {
        Text(w).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
  }

  private func saveAddresses() {
    guard s.claimed else { return }
    model.send(["do": "set_addresses", "local": local, "public": publicName])
  }

  // MARK: claimed

  private var claimed: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      passphraseRow
      if unlockWanted && s.nasPassphrase == "set" { unlockRow }
      if s.qr.isEmpty {
        SettingsRow(title: "Add a phone",
                    detail: s.added.last.map { "Added \($0)." } ?? "One scan pairs a phone with this Mac and your NAS.",
                    labelsContent: false) {
          HStack(spacing: MVTheme.space(8)) {
            Picker("Access", selection: $codeFor) {
              Text(PhoneStore.unlimited).tag(Int64(0))
              ForEach(PhoneStore.durations, id: \.seconds) { d in Text(d.label).tag(d.seconds) }
            }
            .pickerStyle(.menu).frame(width: 160)
            .disabled(!s.nasHasFeatures)
            .help(s.nasHasFeatures ? "How long the phone that scans it may use this library" : "Update the NAS service to use this")
            .accessibilityLabel("How long the phone may use this library")
            SettingsButton(title: "Show code") { model.send(["do": "pair", "for_s": s.nasHasFeatures ? codeFor : 0]) }
              .disabled(!s.busy.isEmpty)
          }
        }
      } else {
        codeBlock
      }
      ForEach(s.phones) { phone in
        SettingsRow(title: phone.name, detail: PhoneStore.phoneDetail(phone), labelsContent: false) {
          HStack(spacing: MVTheme.space(8)) {
            Menu(phone.limited ? "Extend" : "Limit…") {
              // A grant counts from now: Extend offers only lengths that end later than now's end.
              ForEach(PhoneStore.extendDurations(phone), id: \.seconds) { d in
                Button("\(d.label) from now") { model.send(["do": "grant", "key": phone.key, "for_s": d.seconds]) }
              }
              if phone.limited {
                Button(PhoneStore.unlimited) { model.send(["do": "grant", "key": phone.key, "for_s": 0]) }
              }
              Divider()
              // Phone backup (B2): turning it on adds access (the passphrase, once set); off never needs it.
              Button(phone.backup ? "Stop backups from this phone" : "Let this phone back up") {
                model.send(["do": "phone_backup", "key": phone.key, "on": !phone.backup])
              }
            }
            .menuStyle(.borderlessButton).fixedSize()
            .disabled(!s.busy.isEmpty || !s.nasHasFeatures)
            .help(s.nasHasFeatures ? "" : "Update the NAS service to use this")
            .accessibilityLabel(phone.limited ? "Extend \(phone.name)'s access" : "Limit \(phone.name)'s access")
            SettingsButton(title: "Remove") { model.send(["do": "revoke", "key": phone.key]) }
              .disabled(!s.busy.isEmpty)
              .accessibilityLabel("Remove \(phone.name)")
          }
        }
      }
      if !s.reachable {
        Text("Couldn't reach the NAS. These are the phones it knew about last.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local, onCommit: saveAddresses)
      publicField
      relayRow
      folders
    }
  }

  // "NAS passphrase · Locked", Lock, and Change… or Choose…, with its fields inline.
  @ViewBuilder private var passphraseRow: some View {
    SettingsRow(title: "NAS passphrase", detail: model.passphraseDetail, labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        if s.nasPassphrase == "set" && s.unlockedSeconds > 0 {
          SettingsButton(title: "Lock") { model.send(["do": "lock"]) }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel("Lock: forget the NAS key on this Mac")
        } else if s.nasPassphrase == "set" && !unlockWanted {
          SettingsButton(title: "Unlock…") { unlockWanted = true }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel("Unlock with the NAS passphrase")
        }
        if s.nasPassphrase == "set" || s.nasPassphrase == "none" {
          SettingsButton(title: s.nasPassphrase == "set" ? "Change…" : "Choose…") { changing.toggle() }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel(s.nasPassphrase == "set" ? "Change the NAS passphrase" : "Choose a NAS passphrase")
        }
      }
    }
    if changing {
      secure("New passphrase", detail: "At least 10 characters. Nothing on this Mac keeps it.", text: $passphrase)
      secure("Again", detail: "", text: $passphraseAgain)
      HStack(spacing: MVTheme.space(8)) {
        Spacer()
        SettingsButton(title: "Cancel") {
          changing = false
          passphrase = ""
          passphraseAgain = ""
        }
        SettingsButton(title: "Set passphrase") {
          model.send(["do": "set_passphrase", "passphrase": passphrase])
          changing = false
          passphrase = ""
          passphraseAgain = ""
        }
        .disabled(!s.busy.isEmpty || passphrase.count < 10 || passphrase != passphraseAgain)
      }
    }
  }

  // A refused action asks for the passphrase here; unlocked, the person does it again.
  private var unlockRow: some View {
    SettingsRow(title: "Unlock", detail: "Enter the NAS passphrase, then do that again. This Mac keeps the key for 10 minutes and never the passphrase.",
                labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        SecureField("NAS passphrase", text: $unlockWith)
          .onSubmit(unlock)
          .textFieldStyle(.roundedBorder).frame(width: 200).font(MVTheme.font(12))
          .accessibilityLabel("NAS passphrase")
        SettingsButton(title: "Unlock", action: unlock)
          .disabled(unlockWith.isEmpty || !s.busy.isEmpty)
        SettingsButton(title: "Cancel") {
          unlockWanted = false
          unlockWith = ""
        }
        .accessibilityLabel("Cancel the unlock")
      }
    }
  }

  private func unlock() {
    guard !unlockWith.isEmpty else { return }
    model.send(["do": "unlock", "passphrase": unlockWith])
    unlockWith = ""
  }

  private var codeBlock: some View {
    HStack(alignment: .top, spacing: MVTheme.space(16)) {
      PhoneQRImage(text: s.qr)
        .frame(width: 240, height: 240)
        .accessibilityElement()
        .accessibilityAddTraits(.isImage)
        .accessibilityLabel("Code for your phone to scan")
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
        Text("Scan with MediaViewer on your phone").font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        if s.qrForSeconds > 0 {
          Text(PhoneStore.guestCodeLine(s.qrForSeconds)).font(MVTheme.font(12)).foregroundStyle(MVTheme.title)
        }
        Text("Your phone will show \(s.qrLibrary) and this code. Check they match before you confirm.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
        Text(s.qrFingerprint).font(MVTheme.font(13).monospacedDigit()).foregroundStyle(MVTheme.title)
          .textSelection(.enabled)
          .accessibilityLabel("Check code \(s.qrFingerprint)")
        TimelineView(.periodic(from: .now, by: 1)) { context in
          Text(countdown(context.date)).font(MVTheme.font(12).monospacedDigit()).foregroundStyle(MVTheme.body)
        }
        SettingsButton(title: "Cancel") { model.send(["do": "pair_stop"]) }
          .accessibilityLabel("Cancel the code")
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .padding(MVTheme.space(12))
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
    .settingsCardOutline(MVTheme.radius(8))
  }

  private func countdown(_ now: Date) -> String {
    let left = max(0, Int(s.qrExpires) - Int(now.timeIntervalSince1970))
    return String(format: "Good for %d:%02d", left / 60, left % 60)
  }

  private var folders: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      ForEach(s.folders) { f in
        SettingsRow(title: f.backup ? "\(f.label) · phone backup" : f.label,
                    detail: f.backup ? PhoneStore.backupDetail(f) : f.path, labelsContent: false) {
          HStack(spacing: MVTheme.space(8)) {
            if f.backup {
              SettingsButton(title: "Change limit") {
                editing = f.id
                editLimitGB = PhoneStore.gigabytes(f.limit)
                editReserveGB = PhoneStore.gigabytes(f.reserve)
              }
              .disabled(!s.busy.isEmpty)
              .accessibilityLabel("Change the storage limit of \(f.label)")
            }
            if !f.isCatalogue {
              SettingsButton(title: "Stop sharing") { model.send(["do": "remove_folder", "id": f.id]) }
                .disabled(!s.busy.isEmpty)
                .accessibilityLabel("Stop sharing \(f.label)")
            }
          }
        }
        if editing == f.id {
          HStack(spacing: MVTheme.space(8)) {
            Text("Limit").foregroundStyle(MVTheme.body)
            TextField("GB", text: $editLimitGB).textFieldStyle(.roundedBorder).frame(width: 70)
              .accessibilityLabel("Storage limit in gigabytes")
            Text("GB, keep free").foregroundStyle(MVTheme.body)
            TextField("GB", text: $editReserveGB).textFieldStyle(.roundedBorder).frame(width: 60)
              .accessibilityLabel("Free space to keep on the NAS, in gigabytes")
            Text("GB").foregroundStyle(MVTheme.body)
            SettingsButton(title: "Save") {
              if let l = PhoneStore.bytes(editLimitGB), l > 0, let r = PhoneStore.bytes(editReserveGB) {
                model.send(["do": "set_backup", "id": f.id, "limit": String(l), "reserve": String(r)])
              }
              editing = nil
            }
            .disabled((PhoneStore.bytes(editLimitGB) ?? 0) <= 0 || PhoneStore.bytes(editReserveGB) == nil)
            SettingsButton(title: "Cancel") { editing = nil }
          }
          .font(MVTheme.font(12))
        }
      }
      HStack(spacing: MVTheme.space(8)) {
        TextField(newPath.isEmpty ? "Label" : PhoneStore.shareLabel(forPath: newPath), text: $newLabel)
          .textFieldStyle(.roundedBorder).frame(width: 120)
          .accessibilityLabel("Name of the new shared folder")
        TextField("Folder on the NAS, like /volume1/Video", text: $newPath).textFieldStyle(.roundedBorder)
          .accessibilityLabel("Path of the new shared folder on the NAS")
        SettingsButton(title: "Add") {
          let label = newLabel.isEmpty ? PhoneStore.shareLabel(forPath: newPath) : newLabel
          var request: [String: Any] = ["do": "add_folder", "label": label, "path": newPath]
          if newIsBackup, let l = PhoneStore.bytes(newLimitGB), let r = PhoneStore.bytes(newReserveGB) {
            request["backup"] = "1"
            request["limit"] = String(l)
            request["reserve"] = String(r)
          }
          model.send(request)
          newLabel = ""
          newPath = ""
          newIsBackup = false
        }
        .disabled(!s.busy.isEmpty || !newPath.hasPrefix("/") || PhoneStore.shareLabel(forPath: newPath).isEmpty
                  || (newIsBackup && ((PhoneStore.bytes(newLimitGB) ?? 0) <= 0 || PhoneStore.bytes(newReserveGB) == nil)))
        .accessibilityLabel("Add the shared folder")
      }
      .font(MVTheme.font(12))
      .padding(.horizontal, MVTheme.space(12)).padding(.top, MVTheme.space(12))
      HStack(spacing: MVTheme.space(8)) {
        Toggle("Phones back up to this folder", isOn: $newIsBackup)
          .toggleStyle(.checkbox)
          .accessibilityLabel("Phones back up their photos to this folder")
        if newIsBackup {
          Text("Limit").foregroundStyle(MVTheme.body)
          TextField("GB", text: $newLimitGB).textFieldStyle(.roundedBorder).frame(width: 70)
            .accessibilityLabel("Storage limit in gigabytes")
          Text("GB, keep free").foregroundStyle(MVTheme.body)
          TextField("GB", text: $newReserveGB).textFieldStyle(.roundedBorder).frame(width: 60)
            .accessibilityLabel("Free space to keep on the NAS, in gigabytes")
          Text("GB").foregroundStyle(MVTheme.body)
        }
      }
      .font(MVTheme.font(12))
      .padding(.horizontal, MVTheme.space(12)).padding(.bottom, MVTheme.space(12))
      .onChange(of: newIsBackup) { _, on in
        // §10b.1 B5: a suggestion the owner changes; the service refuses past what is really free.
        if on && newLimitGB.isEmpty { newLimitGB = "1000" }
      }
    }
  }

  // MARK: another computer

  private var otherPublisher: some View {
    SettingsRow(title: "Published from another computer",
                detail: "Key \(s.otherFingerprint). Ask it to hand over, and phones follow by themselves. If it's gone, start over and scan each phone again.",
                labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        SettingsButton(title: "Ask to hand over") { model.send(["do": "takeover_ask"]) }
          .disabled(!s.busy.isEmpty)
        SettingsButton(title: "Start over…") { confirmRestart = true }
          .disabled(!s.busy.isEmpty)
      }
    }
    .confirmationDialog("Start over from this Mac?", isPresented: $confirmRestart) {
      Button("Start over", role: .destructive) { model.send(["do": "takeover_restart"]) }
    } message: {
      Text("Every phone will have to scan a new code.")
    }
  }

  private var asking: some View {
    SettingsRow(title: "\(s.askingName) wants to publish this library",
                detail: "Its key is \(s.askingFingerprint). Check it matches what that computer shows.") {
      SettingsButton(title: "Hand over…") { confirmHandOver = true }
        .disabled(!s.busy.isEmpty)
    }
    .confirmationDialog("Hand this library to \(s.askingName)?", isPresented: $confirmHandOver) {
      Button("Hand over") { model.send(["do": "takeover_allow", "key": s.askingKey]) }
    } message: {
      Text("This Mac will stop publishing it. Phones follow by themselves.")
    }
  }

  // MARK: pieces

  private func secure(_ title: String, detail: String, text: Binding<String>) -> some View {
    SettingsRow(title: title, detail: detail) {
      SecureField("", text: text).textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
    }
  }

  // "Relay (your own server)": the address and the server's key go to the NAS,
  // which keeps the key; neither this Mac nor a phone's code ever holds it.
  @ViewBuilder private var relayRow: some View {
    SettingsRow(title: "Relay (your own server)", detail: PhoneStore.relayDetail(s.relay, available: s.nasHasRelay),
                labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        if !s.relay.isEmpty {
          SettingsButton(title: "Remove") { model.send(["do": "set_relay", "address": "", "key": ""]) }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel("Stop using the relay")
        }
        SettingsButton(title: s.relay.isEmpty ? "Set up…" : "Change…") {
          relayEditing.toggle()
          relayAddress = s.relay
          relayKey = ""
        }
        .disabled(!s.busy.isEmpty || !s.nasHasRelay)
        .help(s.nasHasRelay ? "" : "Update the NAS service to use this")
        .accessibilityLabel(s.relay.isEmpty ? "Set up your own relay" : "Change the relay")
      }
    }
    if relayEditing {
      field("Relay address", hint: "relay.example.com:7879", text: $relayAddress)
      secure("Relay key", detail: "The key your driftwire-relay was started with. Your NAS keeps it; this Mac doesn't.", text: $relayKey)
        .onSubmit(saveRelay)
      HStack(spacing: MVTheme.space(8)) {
        Spacer()
        SettingsButton(title: "Cancel") {
          relayEditing = false
          relayKey = ""
        }
        SettingsButton(title: "Save relay", action: saveRelay)
          .disabled(!relayReady)
      }
    }
  }

  private var relayReady: Bool {
    s.busy.isEmpty && !relayAddress.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
      relayKey.count >= PhoneStore.relayKeyMinimum
  }

  private func saveRelay() {
    guard relayReady else { return }
    model.send(["do": "set_relay", "address": relayAddress.trimmingCharacters(in: .whitespacesAndNewlines), "key": relayKey])
    relayEditing = false
    relayKey = ""
  }

  private func forgetSecrets() {
    relayKey = ""
    relayEditing = false
    passphrase = ""
    passphraseAgain = ""
    unlockWith = ""
    changing = false
    unlockWanted = false
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }

  private func field(_ title: String, hint: String, text: Binding<String>, onCommit: @escaping () -> Void = {}) -> some View {
    SettingsRow(title: title, detail: "") {
      TextField(hint, text: text, onCommit: onCommit)
        .textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
    }
  }
}

private struct PhoneQRImage: View {
  let text: String
  @State private var image: CGImage?

  var body: some View {
    ZStack {
      RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(Color.white)
      if let image {
        Image(decorative: image, scale: 1)
          .interpolation(.none)
          .resizable()
          .padding(12)  // the quiet zone a scanner needs
      }
    }
    .task(id: text) {
      let t = text
      image = await Task.detached(priority: .userInitiated) { PhoneQR.image(for: t, side: 432) }.value
    }
  }
}
