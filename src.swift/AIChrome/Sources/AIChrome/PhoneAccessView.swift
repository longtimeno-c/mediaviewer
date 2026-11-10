// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Local search → Phone access (docs/plans/mobile.md §6): the NAS folder the
// catalogue is published to, and Publish now. The work is the pack's, on its own
// thread; this only asks and watches.
import AppKit
import CAiApi
import SwiftUI

@MainActor
final class PhoneAccessModel: ObservableObject {
  let table: AITable

  @Published private(set) var running = false
  @Published private(set) var phase = ""
  @Published private(set) var fraction = 0.0
  @Published private(set) var error = ""
  @Published private(set) var destination = ""
  @Published private(set) var published: Int64 = 0
  @Published private(set) var assets: Int64 = 0
  let link: PhoneLinkModel

  init(table: AITable) {
    self.table = table
    link = PhoneLinkModel(table: table)
  }

  /// A pack built before these entries were appended has no Phone access.
  var available: Bool {
    table.has(\mv_ai_api.phone_publish) && table.has(\mv_ai_api.phone_json)
  }

  func poll() {
    guard available,
          let obj = parseJSON(table.json { table.a.phone_json?(table.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG })
            as? [String: Any] else { return }
    let isRunning = obj["running"] as? Bool ?? false
    if isRunning != running { running = isRunning }
    let newPhase = obj["phase"] as? String ?? ""
    if newPhase != phase { phase = newPhase }
    let f = (obj["fraction"] as? NSNumber)?.doubleValue ?? 0
    if f != fraction { fraction = f }
    let e = obj["error"] as? String ?? ""
    if e != error { error = e }
    let d = obj["destination"] as? String ?? ""
    if d != destination { destination = d }
    let p = int64(obj["published"])
    if p != published { published = p }
    let a = int64(obj["assets"])
    if a != assets { assets = a }
    if !destination.isEmpty { link.poll() }
  }

  func choose() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.prompt = "Choose"
    panel.message = "Choose a folder on the NAS that holds your library."
    guard panel.runModal() == .OK, let url = panel.url else { return }
    publish(url.path)
  }

  func publishNow() {
    guard !destination.isEmpty else {
      choose()
      return
    }
    publish(destination)
  }

  func cancel() { _ = table.call { table.a.phone_cancel?(table.ctx) ?? MV_ERR_INVALID_ARG } }

  private func publish(_ path: String) {
    _ = table.call { table.a.phone_publish?(table.ctx, path) ?? MV_ERR_INVALID_ARG }
    poll()
  }

  var status: String {
    if running { return phase == "sending" ? "Sending to the NAS" : "Building the catalogue" }
    switch error {
    case "unreachable": return "Couldn't reach that folder. It will need to be mounted."
    case "other_library": return "That folder holds another library's catalogue."
    case "no_index": return "Nothing is indexed yet."
    case "no_folders": return "None of your search folders are on that NAS."
    case "no_key_store": return "This copy of MediaViewer can't keep a signing key, so it can't publish."
    case "failed": return "The catalogue could not be built."
    case "cancelled": return "Stopped."
    default: break
    }
    guard published > 0 else { return "Not published yet." }
    let when = Date(timeIntervalSince1970: TimeInterval(published))
    let ago = RelativeDateTimeFormatter().localizedString(for: when, relativeTo: Date())
    // No key here: the one value to compare with a phone is the check code
    // beside the QR (pairing_check), and a second one would teach people to
    // skip the comparison.
    return "Published \(ago) · \(countText(UInt64(max(0, assets)))) files"
  }
}

struct PhoneAccessSection: View {
  @ObservedObject var model: PhoneAccessModel

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      row("Folder on your NAS", detail: model.destination.isEmpty ? "Where your phone will read the catalogue from." : model.destination) {
        Button(model.destination.isEmpty ? "Choose…" : "Change…") { model.choose() }
          .disabled(model.running)
      }
      row("Catalogue", detail: model.status) {
        if model.running {
          Button("Cancel") { model.cancel() }
        } else {
          Button("Publish Now") { model.publishNow() }
        }
      }
      if model.running {
        ProgressView(value: min(1, max(0, model.fraction)))
          .padding(.horizontal, 12).padding(.bottom, 12)
      }
      if !model.destination.isEmpty && model.link.available { PhoneLinkSection(model: model.link) }
    }
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
