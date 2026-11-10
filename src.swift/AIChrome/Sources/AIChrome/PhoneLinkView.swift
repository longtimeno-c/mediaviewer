// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Local search → Phone access, the setup half (docs/plans/mobile.md §5 rule 5,
// §7.2–§7.5): the NAS service, the code a phone scans, the phones and named
// folders, handing the library to another desktop. Every request runs on the
// pack's own thread; this only asks and watches. Keys and addresses shown here
// never reach a log (rule 6).
import AppKit
import CAiApi
import CoreImage
import SwiftUI

/// The QR for a phone: black modules, enlarged without smoothing, on white.
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
final class PhoneLinkModel: ObservableObject {
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
  struct State: Equatable {
    var driftwire = true
    var busy = ""
    var error = ""
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

  let table: AITable
  var getDriftwire: () -> Void = {}
  @Published private(set) var state = State()
  private var asked = false

  init(table: AITable) { self.table = table }

  var available: Bool { table.has(\mv_ai_api.phone_link) && table.has(\mv_ai_api.phone_link_json) }

  func poll() {
    guard available else { return }
    if !asked {
      asked = true
      send(["do": "refresh"])
    }
    guard let o = parseJSON(table.json { table.a.phone_link_json?(table.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG })
            as? [String: Any] else { return }
    var s = State()
    s.driftwire = o["driftwire"] as? Bool ?? true
    s.busy = o["busy"] as? String ?? ""
    s.error = o["error"] as? String ?? ""
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
    s.qrExpires = int64(o["qr_expires"])
    s.added = o["added"] as? [String] ?? []
    s.otherPublisher = o["other_publisher"] as? Bool ?? false
    s.mismatch = o["mismatch"] as? Bool ?? false
    s.otherFingerprint = o["other_fingerprint"] as? String ?? ""
    s.askingName = o["asking_name"] as? String ?? ""
    s.askingKey = o["asking_key"] as? String ?? ""
    s.askingFingerprint = o["asking_fingerprint"] as? String ?? ""
    if s != state { state = s }
  }

  func send(_ request: [String: String]) {
    guard available, let data = try? JSONSerialization.data(withJSONObject: request),
          let text = String(data: data, encoding: .utf8) else { return }
    _ = table.call { table.a.phone_link?(table.ctx, text) ?? MV_ERR_INVALID_ARG }
    poll()
  }

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
    case "not_published": return "Publish the catalogue first."
    case "no_key_store": return "This copy of MediaViewer can't keep a signing key."
    case "revoke_waits": return "Removed here. The NAS will be told when it can be reached."
    case "no_driftwire": return "Driftwire isn't installed."
    default: return "That didn't work."
    }
  }

  /// The work under way, in a few words, for the progress line; nil for the
  /// quick ones and the two-second poll under a code.
  var busyText: String? {
    switch state.busy {
    case "claim": return "Claiming the service"
    case "pair": return "Asking the NAS for a code"
    case "refresh": return "Checking the NAS"
    case "revoke": return "Removing the phone"
    case "add_folder", "remove_folder": return "Changing the shared folders"
    case "set_addresses": return "Checking the address"
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
}

struct PhoneLinkSection: View {
  @ObservedObject var model: PhoneLinkModel
  @State private var local = ""
  @State private var publicName = ""
  @State private var newLabel = ""
  @State private var newPath = ""
  @State private var confirmRestart = false
  @State private var confirmHandOver = false

  private var s: PhoneLinkModel.State { model.state }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      if !s.driftwire {
        row("Driftwire", detail: "Phone access reaches your NAS and your phone through it.") {
          Button("Get Driftwire…") { model.getDriftwire() }
        }
      } else {
        if let line = model.busyText { SweepLine(text: line) }
        if s.mismatch {
          row("The share doesn't match this Mac", detail: "Its library.json names another library or an older epoch, perhaps after a restore. This Mac goes on publishing its own; phones will refuse what doesn't match until it is fixed.") {
            EmptyView()
          }
        }
        if s.otherPublisher { otherPublisher }
        if !s.askingName.isEmpty { asking }
        if !s.claimed { claim } else { claimed }
      }
      if let e = model.errorText {
        Text(e).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .padding(.horizontal, 12).padding(.bottom, 12)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .onChange(of: s.local) { _, v in if local.isEmpty { local = v } }
    .onChange(of: s.publicName) { _, v in if publicName.isEmpty { publicName = v } }
    .onAppear { model.poll() }
  }

  // MARK: claiming

  private var claim: some View {
    VStack(alignment: .leading, spacing: 0) {
      row("Service on your NAS", detail: "Claim it from the NAS's own network, once. Your Mac writes a one-time token to the share to prove it can.") {
        Button("Claim") { model.send(["do": "claim", "local": local, "public": publicName]) }
          .disabled(!s.busy.isEmpty || local.isEmpty)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local)
      publicField
    }
  }

  private var publicField: some View {
    VStack(alignment: .leading, spacing: 4) {
      field("Address from outside", hint: "photos.example.com:7878", text: $publicName, onCommit: saveAddresses)
      if let w = model.warningText {
        Text(w).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .padding(.horizontal, 12).padding(.bottom, 8)
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
    VStack(alignment: .leading, spacing: 0) {
      if s.qr.isEmpty {
        row("Add a phone", detail: addDetail) {
          Button("Show code") { model.send(["do": "pair"]) }
            .disabled(!s.busy.isEmpty)
        }
      } else {
        codeBlock
      }
      ForEach(s.phones) { phone in
        row(phone.name, detail: "Key \(phone.key.prefix(8))") {
          Button("Remove") { model.send(["do": "revoke", "key": phone.key]) }
            .disabled(!s.busy.isEmpty)
        }
      }
      if !s.reachable {
        Text("Couldn't reach the NAS. These are the phones it knew about last.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .padding(.horizontal, 12).padding(.bottom, 8)
      }
      field("Address on your network", hint: "nas.local:7878", text: $local, onCommit: saveAddresses)
      publicField
      folders
    }
  }

  private var addDetail: String {
    if let last = s.added.last { return "Added \(last)." }
    return "One scan pairs a phone with this Mac and your NAS."
  }

  private var codeBlock: some View {
    HStack(alignment: .top, spacing: 16) {
      QRImage(text: s.qr)
        .frame(width: 240, height: 240)
        .accessibilityLabel("Code for your phone to scan")
      VStack(alignment: .leading, spacing: 6) {
        Text("Scan with MediaViewer on your phone").font(AITheme.font(14)).foregroundStyle(AITheme.title)
        Text("Your phone will show \(s.qrLibrary) and this code. Check they match before you confirm.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
        Text(s.qrFingerprint).font(AITheme.font(13).monospacedDigit()).foregroundStyle(AITheme.title)
          .textSelection(.enabled)
        TimelineView(.periodic(from: .now, by: 1)) { context in
          Text(countdown(context.date)).font(AITheme.font(12).monospacedDigit()).foregroundStyle(AITheme.body)
            .onChange(of: context.date) { _, _ in model.poll() }
        }
        Button("Cancel") { model.send(["do": "pair_stop"]) }
          .buttonStyle(.link).font(AITheme.font(12))
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .padding(12)
  }

  private func countdown(_ now: Date) -> String {
    let left = max(0, Int(s.qrExpires) - Int(now.timeIntervalSince1970))
    return String(format: "Good for %d:%02d", left / 60, left % 60)
  }

  private var folders: some View {
    VStack(alignment: .leading, spacing: 0) {
      ForEach(s.folders) { f in
        row(f.label, detail: f.path) {
          if !f.isCatalogue {
            Button("Stop sharing") { model.send(["do": "remove_folder", "id": f.id]) }
              .disabled(!s.busy.isEmpty)
          }
        }
      }
      HStack(spacing: 8) {
        TextField(newPath.isEmpty ? "Label" : PhoneLinkModel.shareLabel(forPath: newPath), text: $newLabel)
          .textFieldStyle(.roundedBorder).frame(width: 120)
          .accessibilityLabel("Name of the new shared folder")
        TextField("Folder on the NAS, like /volume1/Video", text: $newPath).textFieldStyle(.roundedBorder)
          .accessibilityLabel("Path of the new shared folder on the NAS")
        Button("Add") {
          let label = newLabel.isEmpty ? PhoneLinkModel.shareLabel(forPath: newPath) : newLabel
          model.send(["do": "add_folder", "label": label, "path": newPath])
          newLabel = ""
          newPath = ""
        }
        .disabled(!s.busy.isEmpty || !newPath.hasPrefix("/") || PhoneLinkModel.shareLabel(forPath: newPath).isEmpty)
      }
      .font(AITheme.font(12))
      .padding(12)
    }
  }

  // MARK: another desktop

  private var otherPublisher: some View {
    row("Published from another computer", detail: "Key \(s.otherFingerprint). Ask it to hand over, and phones follow by themselves. If it's gone, start over and scan each phone again.") {
      HStack(spacing: 8) {
        Button("Ask to hand over") { model.send(["do": "takeover_ask"]) }
          .disabled(!s.busy.isEmpty)
        Button("Start over…") { confirmRestart = true }
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
    row("\(s.askingName) wants to publish this library", detail: "Its key is \(s.askingFingerprint). Check it matches what that computer shows.") {
      Button("Hand over…") { confirmHandOver = true }
        .disabled(!s.busy.isEmpty)
    }
    .confirmationDialog("Hand this library to \(s.askingName)?", isPresented: $confirmHandOver) {
      Button("Hand over") { model.send(["do": "takeover_allow", "key": s.askingKey]) }
    } message: {
      Text("This Mac will stop publishing it. Phones follow by themselves.")
    }
  }

  // MARK: pieces

  private func field(_ title: String, hint: String, text: Binding<String>, onCommit: @escaping () -> Void = {}) -> some View {
    HStack(spacing: 20) {
      Text(title).font(AITheme.font(14)).foregroundStyle(AITheme.title)
        .frame(maxWidth: .infinity, alignment: .leading)
      TextField(hint, text: text, onCommit: onCommit)
        .textFieldStyle(.roundedBorder).frame(width: 240).font(AITheme.font(12))
        .accessibilityLabel(title)
    }
    .padding(12)
  }

  private func row<Content: View>(_ title: String, detail: String,
                                  @ViewBuilder _ content: () -> Content) -> some View {
    HStack(spacing: 20) {
      VStack(alignment: .leading, spacing: 3) {
        Text(title).font(AITheme.font(14)).foregroundStyle(AITheme.title)
        Text(detail).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      content().labelsHidden()
    }
    .padding(12)
  }
}

private struct QRImage: View {
  let text: String
  @State private var image: CGImage?

  var body: some View {
    ZStack {
      RoundedRectangle(cornerRadius: AITheme.radius(8)).fill(Color.white)
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

/// The progress language's line with no fraction (docs/design/20 "Craft
/// rules"): a 3 pt rounded track, a calm 1.8 s accent sweep, one line under it.
/// The sweep runs with Reduce Motion too, as the rules say.
private struct SweepLine: View {
  let text: String

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      TimelineView(.animation) { context in
        GeometryReader { geo in
          let phase = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1.8) / 1.8
          let width = geo.size.width * 0.3
          ZStack(alignment: .leading) {
            Capsule().fill(AITheme.hairline)
            Capsule().fill(AITheme.accent)
              .frame(width: width)
              .offset(x: (geo.size.width + width) * phase - width)
          }
          .clipShape(Capsule())
        }
      }
      .frame(height: 3)
      Text(text).font(AITheme.font(12)).foregroundStyle(AITheme.body)
    }
    .padding(12)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(text)
  }
}
