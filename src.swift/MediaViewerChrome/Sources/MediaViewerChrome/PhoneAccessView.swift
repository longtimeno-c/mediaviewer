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
    var id: String { key }
  }
  struct Folder: Identifiable, Equatable {
    let id: String
    let label: String
    let path: String
    var isCatalogue: Bool { label == "Catalogue" }
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
      return Phone(name: n, key: k)
    }
    s.folders = (o["folders"] as? [[String: Any]] ?? []).compactMap {
      guard let i = $0["id"] as? String, let l = $0["label"] as? String, let p = $0["path"] as? String else { return nil }
      return Folder(id: i, label: l, path: p)
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
    return s
  }

  func send(_ request: [String: String]) {
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
    default: return "That didn't work."
    }
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
  @State private var confirmRestart = false
  @State private var confirmHandOver = false

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
    .onDisappear { model.disappeared() }
    .onChange(of: settings.visible) { _, visible in if visible { model.appeared() } else { model.disappeared() } }
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
        SettingsButton(title: "Claim") { model.send(["do": "claim", "local": local, "public": publicName]) }
          .accessibilityLabel("Claim the service")
          .disabled(!s.busy.isEmpty || local.isEmpty)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local)
      publicField
    }
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
      if s.qr.isEmpty {
        SettingsRow(title: "Add a phone",
                    detail: s.added.last.map { "Added \($0)." } ?? "One scan pairs a phone with this Mac and your NAS.") {
          SettingsButton(title: "Show code") { model.send(["do": "pair"]) }
            .disabled(!s.busy.isEmpty)
        }
      } else {
        codeBlock
      }
      ForEach(s.phones) { phone in
        SettingsRow(title: phone.name, detail: "Key \(phone.key.prefix(8))", labelsContent: false) {
          SettingsButton(title: "Remove") { model.send(["do": "revoke", "key": phone.key]) }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel("Remove \(phone.name)")
        }
      }
      if !s.reachable {
        Text("Couldn't reach the NAS. These are the phones it knew about last.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local, onCommit: saveAddresses)
      publicField
      folders
    }
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
        SettingsRow(title: f.label, detail: f.path, labelsContent: false) {
          if !f.isCatalogue {
            SettingsButton(title: "Stop sharing") { model.send(["do": "remove_folder", "id": f.id]) }
              .disabled(!s.busy.isEmpty)
              .accessibilityLabel("Stop sharing \(f.label)")
          }
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
          model.send(["do": "add_folder", "label": label, "path": newPath])
          newLabel = ""
          newPath = ""
        }
        .disabled(!s.busy.isEmpty || !newPath.hasPrefix("/") || PhoneStore.shareLabel(forPath: newPath).isEmpty)
        .accessibilityLabel("Add the shared folder")
      }
      .font(MVTheme.font(12))
      .padding(MVTheme.space(12))
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
