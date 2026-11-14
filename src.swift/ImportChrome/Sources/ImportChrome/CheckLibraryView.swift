// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Check library (docs/plans/check-library.md), the Mac twin of
// CheckLibraryWindow.cs: pick a folder to check and a library folder, and the
// engine says which of the folder's files are already in the library (by
// content: size, then BLAKE3; never the name), wherever they are and whatever
// they are called, which are not, and which duplicates the library already
// holds. The library's hashes are kept in import.db, so a second check reads
// no unchanged file. Report only: nothing is copied, moved or deleted.
// Keyboard: Return starts the check, Esc closes.
import AppKit
import CImportApi
import SwiftUI

@MainActor
final class CheckLibraryModel: ObservableObject {
  let table: ImportTable
  weak var chrome: MVImportChrome?

  @Published var showing = false
  @Published var source = ""
  @Published var library = ""
  /// 0 the library folder and its subfolders, 1 every library Import knows.
  @Published var scope = 0
  @Published var running = false
  @Published var progressLine = ""
  @Published var fraction: Double?
  @Published var headline = ""
  @Published var found: [(path: String, whereAt: [String])] = []
  @Published var notFound: [String] = []
  @Published var duplicates: [(size: Int64, paths: [String])] = []
  @Published var reportPath = ""
  private(set) var job: UInt64 = 0

  init(table: ImportTable) { self.table = table }

  var api: mv_import_api { table.api.pointee }

  func open(library defaultLibrary: String) {
    if library.isEmpty { library = defaultLibrary }
    showing = true
  }

  func chooseSource() { if let p = pick("Choose the folder to check against the library.") { source = p } }
  func chooseLibrary() { if let p = pick("Choose the library folder. Its subfolders are included.") { library = p } }

  private func pick(_ message: String) -> String? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.prompt = "Choose"
    panel.message = message
    guard panel.runModal() == .OK, let url = panel.url else { return nil }
    return url.path
  }

  var canStart: Bool { !running && !source.isEmpty && !library.isEmpty && source != library }

  func start() {
    guard canStart else { return }
    let request: [String: Any] = [
      "destination": library, "source_folder": source, "scope": scope == 1 ? "library" : "destination",
    ]
    guard let data = try? JSONSerialization.data(withJSONObject: request),
          let text = String(data: data, encoding: .utf8),
          let id = table.id({ out in text.withCString { api.check_library!(table.ctx, $0, out) } }) else { return }
    job = id
    running = true
    found = []
    notFound = []
    duplicates = []
    headline = ""
    reportPath = ""
    progressLine = "Looking through the library…"
    fraction = nil
    chrome?.track(id, label: "check")
  }

  func cancel() { table.call { api.cancel!(table.ctx, job) } }

  func progressed(_ id: UInt64, _ p: mv_import_progress) {
    guard id == job, running else { return }
    switch p.destination_count {  // the check's phase (mediaviewer_import.h)
    case 0:
      progressLine = "Looking through the library · \(p.units_done) files"
      fraction = nil
    default:
      progressLine = p.destination_count == 1 ? "Reading library files whose size matches" : "Reading the folder's files"
      fraction = p.bytes_total == 0 ? nil : min(1, Double(p.bytes_read) / Double(p.bytes_total))
    }
  }

  /// MV_ADDON_EVENT_LIBRARY_CHECKED.
  func refresh(_ id: UInt64) {
    guard id == job else { return }
    running = false
    guard let s = parse(table.json { api.summary_json!(table.ctx, id, $0, $1, $2) }) as? [String: Any] else { return }
    found = (s["found"] as? [[String: Any]] ?? []).map { ($0["path"] as? String ?? "", $0["where"] as? [String] ?? []) }
    notFound = s["not_found"] as? [String] ?? []
    duplicates = (s["duplicates"] as? [[String: Any]] ?? []).map {
      (($0["size"] as? NSNumber)?.int64Value ?? 0, $0["paths"] as? [String] ?? [])
    }
    if !(s["reachable"] as? Bool ?? false) {
      headline = "The library folder could not be reached. Nothing was checked."
    } else {
      let bytes = (s["not_found_bytes"] as? NSNumber)?.int64Value ?? 0
      headline = "\(found.count) already in the library · \(notFound.count) not (" +
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) + ")"
      if s["cancelled"] as? Bool ?? false { headline += " · stopped early" }
    }
    if let path = table.path({ api.report_path!(table.ctx, id, $0, $1) }) { reportPath = path }
  }

  func openInViewer(_ path: String) { chrome?.openInViewer(path) }
  func reveal(_ path: String) { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)]) }
  func showReport() {
    guard !reportPath.isEmpty else { return }
    NSWorkspace.shared.open(URL(fileURLWithPath: reportPath))
  }
}

struct CheckLibraryView: View {
  @ObservedObject var model: CheckLibraryModel
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Check a Folder Against the Library").font(.title2)
      Text("Finds the folder's files that are already in the library by their content, even renamed or in other folders. Nothing is copied, moved or deleted.")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 6) {
        GridRow {
          Text("Folder to check")
          Text(model.source.isEmpty ? "None chosen" : model.source).lineLimit(1).truncationMode(.middle)
          Button("Choose…") { model.chooseSource() }.disabled(model.running)
        }
        GridRow {
          Text("Library")
          Text(model.library.isEmpty ? "None chosen" : model.library).lineLimit(1).truncationMode(.middle)
          Button("Choose…") { model.chooseLibrary() }.disabled(model.running)
        }
        GridRow {
          Text("Look in")
          Picker("Look in", selection: $model.scope) {
            Text("This library folder").tag(0)
            Text("Every library Import knows").tag(1)
          }
          .labelsHidden().fixedSize().disabled(model.running)
          Color.clear.frame(width: 1, height: 1)
        }
      }
      if model.running {
        if let f = model.fraction { ProgressView(value: f) } else { ProgressView().progressViewStyle(.linear) }
        Text(model.progressLine).font(.caption).foregroundStyle(.secondary)
      } else if !model.headline.isEmpty {
        Text(model.headline)
      }
      List {
        if !model.found.isEmpty {
          Section(header: Text("Already in the library (\(model.found.count))")) {
            ForEach(model.found, id: \.path) { f in
              VStack(alignment: .leading, spacing: 2) {
                pathRow(f.path)
                ForEach(f.whereAt, id: \.self) { w in pathRow(w, prefix: "= ") }
              }
            }
          }
        }
        if !model.notFound.isEmpty {
          Section(header: Text("Not in the library (\(model.notFound.count))")) {
            ForEach(model.notFound, id: \.self) { pathRow($0) }
          }
        }
        if !model.duplicates.isEmpty {
          Section(header: Text("Duplicates already in the library (\(model.duplicates.count) sets)")) {
            ForEach(Array(model.duplicates.enumerated()), id: \.offset) { _, g in
              VStack(alignment: .leading, spacing: 2) {
                Text("\(g.paths.count) identical files · " + ByteCountFormatter.string(fromByteCount: g.size, countStyle: .file) + " each")
                  .font(.caption).foregroundStyle(.secondary)
                ForEach(g.paths, id: \.self) { pathRow($0) }
              }
            }
          }
        }
      }
      .frame(minHeight: 220)
      HStack {
        if !model.reportPath.isEmpty { Button("Show Report") { model.showReport() } }
        Spacer()
        if model.running {
          Button("Stop") { model.cancel() }
        }
        Button("Close") { model.showing = false; dismiss() }.keyboardShortcut(.cancelAction)
        Button("Check") { model.start() }.keyboardShortcut(.defaultAction).disabled(!model.canStart)
      }
    }
    .padding(16)
    .frame(minWidth: 620, minHeight: 460)
  }

  private func pathRow(_ path: String, prefix: String = "") -> some View {
    HStack(spacing: 6) {
      Text(prefix + (path as NSString).abbreviatingWithTildeInPath).lineLimit(1).truncationMode(.middle)
      Spacer(minLength: 0)
      Button("Open") { model.openInViewer(path) }.buttonStyle(.link)
      Button("Show in Finder") { model.reveal(path) }.buttonStyle(.link)
    }
    .font(.callout)
  }
}

/// Observes the check model itself, so its sheet updates without
/// re-rendering the Import window on every progress tick.
struct CheckLibrarySheetHost: View {
  @ObservedObject var model: CheckLibraryModel

  var body: some View {
    Color.clear.sheet(isPresented: $model.showing) { CheckLibraryView(model: model) }
  }
}
