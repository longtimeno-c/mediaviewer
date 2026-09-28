// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Local search → Import and export (plan/17 "Sharing an index",
// 2026-09-28): the index of some folders written to one .mvindex file, and a
// file merged into this index with each of its folders pointed at where those
// files are on this Mac (a NAS mounted elsewhere, a copied card). People and
// thumbnails travel only when ticked; People is off by default because the
// file then identifies the people in it.
//
// export_index / import_index / transfer_json / transfer_cancel are
// [no-block]; inspect_export reads the file and runs detached. Progress is
// read on the status poll while a transfer runs.
import AppKit
import CAiApi
import SwiftUI
import UniformTypeIdentifiers

/// One of the file's folders, and where it is here.
struct ImportRoot: Identifiable, Equatable {
  let id: Int64
  let name: String
  let path: String        // where it was on the machine that exported it
  let assets: Int64
  var target: String      // where it is here ("" : not chosen)
  var include: Bool
}

/// What inspect_export said about a file.
struct ImportPlan: Equatable {
  let url: URL
  let from: String
  let model: String
  let pictureUsable: Bool
  let adoptQuality: Int
  let whyNot: String
  let faces: Int64        // -1: the file has no People
  let people: Int64
  let peopleReady: Bool
  let peopleMatch: Bool
  let thumbs: Int64
  var roots: [ImportRoot]
}

@MainActor
final class IndexTransferModel: ObservableObject {
  let table: AITable

  @Published var exporting = false        // the export options are open
  @Published var exportRoots: Set<UInt64> = []
  @Published var exportThumbs = true
  @Published var exportPeople = false
  @Published var plan: ImportPlan?
  @Published var importThumbs = true
  @Published var importPeople = false
  @Published private(set) var running = false
  @Published private(set) var runningImport = false
  @Published private(set) var fraction = 0.0
  @Published private(set) var note = ""
  /// The roots / People changed: the management view reloads them.
  var changed: (() -> Void)?

  private var job: UInt64 = 0

  init(table: AITable) { self.table = table }

  /// A pack built before these entries were appended has no Import and export.
  var available: Bool {
    table.has(\mv_ai_api.export_index) && table.has(\mv_ai_api.transfer_json)
  }

  static let fileType = UTType(filenameExtension: "mvindex") ?? .data

  // MARK: export

  func beginExport(roots: [RootRow]) {
    plan = nil
    exportRoots = Set(roots.map(\.id))
    exportPeople = false
    exporting = true
  }

  func export(peopleOn: Bool) {
    guard !exportRoots.isEmpty else { return }
    let panel = NSSavePanel()
    panel.allowedContentTypes = [Self.fileType]
    panel.nameFieldStringValue = "MediaViewer index.mvindex"
    panel.prompt = "Export"
    panel.message = "The index describes your files by name; your photos and videos are not in it."
    guard panel.runModal() == .OK, let url = panel.url else { return }
    var flags: UInt32 = 0
    if exportThumbs { flags |= MV_AI_TRANSFER_THUMBS }
    if exportPeople && peopleOn { flags |= MV_AI_TRANSFER_PEOPLE }
    let ids = Array(exportRoots)
    var id: UInt64 = 0
    let s = table.call {
      ids.withUnsafeBufferPointer { table.a.export_index?(table.ctx, url.path, $0.baseAddress, UInt32($0.count), flags, &id) }
    }
    guard s == MV_OK else {
      note = s == MV_ERR_BUSY ? "Another import or export is running." : "The index could not be exported."
      return
    }
    exporting = false
    started(id, import: false)
  }

  // MARK: import

  func chooseImport() {
    let panel = NSOpenPanel()
    panel.allowedContentTypes = [Self.fileType]
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.prompt = "Open"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    exporting = false
    note = "Reading the file…"
    let t = table
    Task.detached {
      let json = t.json { t.a.inspect_export?(t.ctx, url.path, $0, $1, $2) ?? MV_ERR_INVALID_ARG }
      let obj = parseJSON(json) as? [String: Any]
      await MainActor.run {
        guard let obj else {
          self.note = "That file is not a MediaViewer index, or it comes from a newer version."
          return
        }
        self.note = ""
        self.plan = Self.plan(url, obj)
        self.importThumbs = (self.plan?.thumbs ?? 0) > 0
        self.importPeople = false
      }
    }
  }

  private static func plan(_ url: URL, _ o: [String: Any]) -> ImportPlan {
    let people = o["people"] as? [String: Any]
    let roots = (o["roots"] as? [[String: Any]] ?? []).map { r -> ImportRoot in
      let path = r["path"] as? String ?? ""
      // The same place here (a NAS mounted at the same path) answers itself.
      let here = r["exists"] as? Bool ?? false
      return ImportRoot(id: int64(r["id"]), name: r["name"] as? String ?? "", path: path,
                        assets: int64(r["assets"]), target: here ? path : "", include: true)
    }
    return ImportPlan(url: url, from: o["from"] as? String ?? "", model: o["model"] as? String ?? "",
                      pictureUsable: o["picture_usable"] as? Bool ?? false,
                      adoptQuality: Int(int64(o["adopt_quality"])), whyNot: o["why_not"] as? String ?? "",
                      faces: people == nil ? -1 : int64(people?["faces"]), people: int64(people?["people"]),
                      peopleReady: people?["ready"] as? Bool ?? false, peopleMatch: people?["match"] as? Bool ?? false,
                      thumbs: int64(o["thumbs"]), roots: roots)
  }

  func chooseTarget(_ root: ImportRoot) {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.prompt = "Choose"
    panel.message = "Where are the files of “\(root.name)” on this Mac?"
    guard panel.runModal() == .OK, let url = panel.url,
          let i = plan?.roots.firstIndex(where: { $0.id == root.id }) else { return }
    plan?.roots[i].target = url.path
    plan?.roots[i].include = true
  }

  var canImport: Bool {
    guard let plan else { return false }
    let chosen = plan.roots.filter(\.include)
    return !chosen.isEmpty && chosen.allSatisfy { !$0.target.isEmpty }
  }

  func runImport() {
    guard let plan, canImport else { return }
    let map = plan.roots.filter(\.include).map { ["id": $0.id, "path": $0.target] as [String: Any] }
    guard let data = try? JSONSerialization.data(withJSONObject: map),
          let json = String(data: data, encoding: .utf8) else { return }
    var flags: UInt32 = 0
    if importThumbs && plan.thumbs > 0 { flags |= MV_AI_TRANSFER_THUMBS }
    if importPeople && plan.faces > 0 { flags |= MV_AI_TRANSFER_PEOPLE }
    var id: UInt64 = 0
    let s = table.call { table.a.import_index?(table.ctx, plan.url.path, json, flags, &id) }
    guard s == MV_OK else {
      note = s == MV_ERR_BUSY ? "Another import or export is running." : "The index could not be imported."
      return
    }
    self.plan = nil
    started(id, import: true)
  }

  // MARK: progress

  private func started(_ id: UInt64, import: Bool) {
    job = id
    running = true
    runningImport = `import`
    fraction = 0
    note = ""
  }

  func cancel() { table.call { table.a.transfer_cancel?(table.ctx) } }

  /// The status poll, while a transfer runs.
  func poll() {
    guard running, available else { return }
    guard let obj = parseJSON(table.json { table.a.transfer_json?(table.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG })
            as? [String: Any], UInt64(clamping: int64(obj["id"])) == job else { return }
    fraction = (obj["fraction"] as? NSNumber)?.doubleValue ?? fraction
    guard obj["done"] as? Bool ?? false else { return }
    running = false
    let status = Int32(int64(obj["status"]))
    let outcome = obj["outcome"] as? [String: Any] ?? [:]
    note = runningImport ? Self.importNote(status, outcome) : Self.exportNote(status, outcome)
    if runningImport { changed?() }
  }

  private static func exportNote(_ status: Int32, _ o: [String: Any]) -> String {
    if status == MV_ERR_CANCELLED.rawValue { return "The export was cancelled." }
    guard status == MV_OK.rawValue else { return "The index could not be exported." }
    var s = "Exported \(countText(UInt64(max(0, int64(o["assets"]))))) files (\(bytesText(UInt64(max(0, int64(o["bytes"])))))."
    if o["people_included"] as? Bool ?? false { s += " People included." }
    let missing = int64(o["thumbs_missing"])
    if missing > 0 { s += " \(countText(UInt64(missing))) thumbnails were not made yet and were left out." }
    return s
  }

  private static func importNote(_ status: Int32, _ o: [String: Any]) -> String {
    if status == MV_ERR_CANCELLED.rawValue { return "The import was cancelled. Nothing it had not finished was kept." }
    guard status == MV_OK.rawValue else { return "The index could not be imported." }
    let added = int64(o["added"]) + int64(o["replaced"])
    var s = "Imported \(countText(UInt64(max(0, added)))) files."
    let kept = int64(o["kept"])
    if kept > 0 { s += " \(countText(UInt64(kept))) already indexed here were kept." }
    if !(o["picture_usable"] as? Bool ?? true) {
      s += " Their pictures were indexed with another model, so they will be indexed again here."
    }
    if int64(o["adopted_quality"]) != 0 { s += " Search now uses the model the file was made with." }
    if int64(o["faces"]) > 0 { s += " People: \(countText(UInt64(int64(o["faces"])))) faces." }
    switch o["people_why"] as? String ?? "" {
    case "no_piece": s += " Install People to bring in its faces."
    case "model": s += " Its faces came from another People model and were left out."
    default: break
    }
    return s + " Folders are checked now; anything changed here is indexed again."
  }
}

struct IndexTransferSection: View {
  @ObservedObject var model: IndexTransferModel
  let roots: [RootRow]
  let peopleOn: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      row("Export the index",
          detail: "Save what is indexed to one file, to import on another computer or after moving a library, so nothing is indexed twice. Your photos and videos are not in it.") {
        Button("Export…") { model.beginExport(roots: roots) }
          .disabled(model.running || roots.isEmpty)
      }
      if model.exporting { exportOptions }
      Rectangle().fill(AITheme.hairline).frame(height: 1)
      row("Import an index",
          detail: "Add an exported index to this one. Folders already indexed here keep their own results; files that differ are indexed again.") {
        Button("Import…") { model.chooseImport() }
          .disabled(model.running)
      }
      if let plan = model.plan { importOptions(plan) }
      if model.running {
        HStack(spacing: 10) {
          ProgressView(value: model.fraction).progressViewStyle(.linear)
          Text(model.runningImport ? "Importing…" : "Exporting…")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          Button("Cancel") { model.cancel() }
        }
        .padding(12)
      }
      if !model.note.isEmpty {
        Text(model.note).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
          .padding(.horizontal, 12).padding(.bottom, 12)
      }
    }
  }

  private var exportOptions: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Folders").font(AITheme.font(12)).foregroundStyle(AITheme.body)
      ForEach(roots) { root in
        Toggle(isOn: Binding(get: { model.exportRoots.contains(root.id) },
                             set: { on in
                               if on { model.exportRoots.insert(root.id) } else { model.exportRoots.remove(root.id) }
                             })) {
          Text(root.path).lineLimit(1).truncationMode(.middle)
        }
        .toggleStyle(.checkbox)
      }
      Toggle("Include thumbnails (a larger file; the other computer shows the gallery at once)",
             isOn: $model.exportThumbs)
        .toggleStyle(.checkbox)
      Toggle("Include People (faces and names)", isOn: $model.exportPeople)
        .toggleStyle(.checkbox)
        .disabled(!peopleOn)
      if model.exportPeople && peopleOn {
        Label("The file will identify the people in your photos. Share it only with people you trust.",
              systemImage: "exclamationmark.triangle")
          .font(AITheme.font(12)).foregroundStyle(AITheme.title)
      } else if !peopleOn {
        Text("People is off, so there are no faces to include.")
          .font(AITheme.font(11)).foregroundStyle(AITheme.body)
      }
      HStack {
        Spacer()
        Button("Cancel") { model.exporting = false }.keyboardShortcut(.cancelAction)
        Button("Export…") { model.export(peopleOn: peopleOn) }
          .keyboardShortcut(.defaultAction)
          .disabled(model.exportRoots.isEmpty)
      }
    }
    .font(AITheme.font(12))
    .padding(12)
    .background(Color.accentColor.opacity(0.06))
  }

  private func importOptions(_ plan: ImportPlan) -> some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(summary(plan)).font(AITheme.font(12)).foregroundStyle(AITheme.title)
        .fixedSize(horizontal: false, vertical: true)
      if plan.whyNot == "loading" {
        Text("Local search is still loading its models. The import waits for them, then uses this file's pictures if they were made with a model this Mac has.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
      } else if !plan.pictureUsable {
        Label(plan.whyNot == "no_models"
                ? "Local search could not load its models, so the pictures in this file cannot be used here."
                : "This file was indexed with another search model than this Mac uses, so its pictures will be indexed again here. Clear this Mac's index first to use the file's model instead.",
              systemImage: "exclamationmark.triangle")
          .font(AITheme.font(12)).foregroundStyle(AITheme.title)
          .fixedSize(horizontal: false, vertical: true)
      } else if plan.adoptQuality != 0 {
        Text("This index is empty, so search will use the file's model\(plan.model.isEmpty ? "" : " (\(plan.model))").")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      }
      Text("Where are these folders on this Mac?").font(AITheme.font(12)).foregroundStyle(AITheme.body)
      ForEach(plan.roots) { root in
        HStack(spacing: 8) {
          Toggle("", isOn: Binding(get: { model.plan?.roots.first(where: { $0.id == root.id })?.include ?? false },
                                   set: { on in
                                     if let i = model.plan?.roots.firstIndex(where: { $0.id == root.id }) {
                                       model.plan?.roots[i].include = on
                                     }
                                   }))
            .toggleStyle(.checkbox).labelsHidden()
          VStack(alignment: .leading, spacing: 2) {
            Text("\(root.name) · \(countText(UInt64(max(0, root.assets)))) files")
              .foregroundStyle(AITheme.title)
            Text(root.target.isEmpty ? "Was \(root.path)" : root.target)
              .foregroundStyle(root.target.isEmpty ? AITheme.body : AITheme.title)
              .lineLimit(1).truncationMode(.middle)
          }
          .frame(maxWidth: .infinity, alignment: .leading)
          Button(root.target.isEmpty ? "Choose…" : "Change…") { model.chooseTarget(root) }
        }
      }
      if plan.thumbs > 0 {
        Toggle("Import thumbnails", isOn: $model.importThumbs).toggleStyle(.checkbox)
      }
      if plan.faces > 0 {
        Toggle(plan.peopleReady ? "Import People (\(countText(UInt64(plan.faces))) faces)"
                                : "Import People (\(countText(UInt64(plan.faces))) faces; turns People on)",
               isOn: $model.importPeople)
          .toggleStyle(.checkbox)
        if plan.peopleReady && !plan.peopleMatch {
          Text("Its faces come from another People model and would be left out.")
            .font(AITheme.font(11)).foregroundStyle(AITheme.body)
        }
      }
      HStack {
        Spacer()
        Button("Cancel") { model.plan = nil }.keyboardShortcut(.cancelAction)
        Button("Import") { model.runImport() }
          .keyboardShortcut(.defaultAction)
          .disabled(!model.canImport)
      }
    }
    .font(AITheme.font(12))
    .padding(12)
    .background(Color.accentColor.opacity(0.06))
  }

  private func summary(_ plan: ImportPlan) -> String {
    let files = plan.roots.reduce(Int64(0)) { $0 + $1.assets }
    var s = "\(countText(UInt64(max(0, files)))) files in \(plan.roots.count == 1 ? "1 folder" : "\(plan.roots.count) folders")"
    if !plan.from.isEmpty { s += ", exported on \(plan.from)" }
    if !plan.model.isEmpty { s += ", indexed with \(plan.model)" }
    return s + "."
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
