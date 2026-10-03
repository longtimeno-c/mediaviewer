// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Find duplicates (PR 54), the Mac twin of DuplicatesWindow.cs: pick a folder,
// the engine walks it and every folder under it and groups files with
// identical bytes (size, then BLAKE3; never the name). Each file can be
// opened in the viewer or shown in the Finder, and the copies the person
// picks (one, or several with ⌘ / ⇧) are moved to the Trash in one go. A pick
// that would take every copy of a file is refused here, and the engine still
// refuses to trash the last copy in a group and re-reads the copy it keeps
// before it moves anything.
// Keyboard-complete: arrows move through the files (⇧-arrows pick several),
// Return opens one in the viewer, ⌘⌫ moves the picked copies to the Trash,
// ⌘R shows one in the Finder, Esc closes.
import AppKit
import CImportApi
import SwiftUI

struct DuplicateFile: Identifiable, Equatable {
  let path: String
  let relative: String
  let mtime: Int
  let state: String   // kept | queued | trashed | refused
  let reason: String
  var id: String { path }
}

struct DuplicateGroup: Identifiable, Equatable {
  let index: Int
  let size: Int64
  let files: [DuplicateFile]
  var id: Int { index }
  /// Files still on disk; the engine never lets this reach zero.
  var alive: Int { files.filter { $0.state != "trashed" }.count }
}

@MainActor
final class DuplicatesModel: ObservableObject {
  let table: ImportTable
  weak var chrome: MVImportChrome?

  @Published var showing = false
  @Published var folder = ""
  @Published var running = false
  @Published var progressLine = ""
  @Published var fraction: Double?
  @Published var headline = ""
  @Published var groups: [DuplicateGroup] = []
  @Published var unreadable: [String] = []
  @Published var canTrash = false
  @Published var reportPath = ""
  /// The picked files, by path (⌘-click adds or removes one, ⇧-click a run).
  @Published var selection: Set<String> = []
  private(set) var job: UInt64 = 0

  init(table: ImportTable) { self.table = table }

  var api: mv_import_api { table.api.pointee }

  func choose() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.prompt = "Find Duplicates"
    panel.message = "Every file in this folder and the folders inside it is compared by content."
    guard panel.runModal() == .OK, let url = panel.url else { return }
    start(url.path)
  }

  func start(_ dir: String) {
    guard let id = table.id({ out in dir.withCString { api.find_duplicates!(table.ctx, $0, out) } }) else {
      return
    }
    job = id
    folder = dir
    running = true
    groups = []
    unreadable = []
    selection = []
    headline = ""
    progressLine = "Looking through \(dir)…"
    fraction = nil
    showing = true
    chrome?.track(id, label: "duplicates")
  }

  func cancel() { _ = api.cancel!(table.ctx, job) }

  func progressed(_ id: UInt64, _ p: mv_import_progress) {
    guard id == job, running else { return }
    if p.bytes_total == 0 && p.units_done == 0 {
      progressLine = "Looking through \(p.units_total) files…"
      fraction = nil
      return
    }
    let rate = p.bytes_per_second > 0 ? " · \(Int(p.bytes_per_second / 1_000_000)) MB/s" : ""
    progressLine = "Comparing \(p.units_done) of \(p.units_total) files that share a size\(rate)"
    fraction = p.units_total == 0 ? nil : Double(p.units_done) / Double(p.units_total)
  }

  /// MV_ADDON_EVENT_DUPLICATES_DONE and every MV_ADDON_EVENT_DUPLICATE_TRASHED.
  func refresh(_ id: UInt64) {
    guard id == job else { return }
    running = false
    guard let s = parse(table.json { api.summary_json!(table.ctx, id, $0, $1, $2) }) as? [String: Any] else {
      return
    }
    let root = (s["folder"] as? String) ?? folder
    let prefix = root.hasSuffix("/") ? root : root + "/"
    groups = (s["groups"] as? [[String: Any]] ?? []).enumerated().map { i, g in
      let files = (g["files"] as? [[String: Any]] ?? []).map { f -> DuplicateFile in
        let path = f["path"] as? String ?? ""
        return DuplicateFile(path: path,
                             relative: path.hasPrefix(prefix) ? String(path.dropFirst(prefix.count)) : path,
                             mtime: f["mtime"] as? Int ?? 0,
                             state: f["state"] as? String ?? "kept",
                             reason: f["reason"] as? String ?? "")
      }
      return DuplicateGroup(index: i, size: (g["size"] as? NSNumber)?.int64Value ?? 0, files: files)
    }
    unreadable = s["unreadable"] as? [String] ?? []
    canTrash = s["can_trash"] as? Bool ?? false
    let wasted = (s["wasted_bytes"] as? NSNumber)?.int64Value ?? 0
    let dupes = s["duplicate_files"] as? Int ?? 0
    let files = s["files"] as? Int ?? 0
    if s["walk_failed"] as? Bool ?? false {
      headline = "This folder could not be read."
    } else if groups.isEmpty {
      headline = "No duplicates among \(files) files."
    } else {
      headline = "\(dupes) duplicate \(dupes == 1 ? "file" : "files") in \(groups.count) " +
        "\(groups.count == 1 ? "group" : "groups") · " +
        ByteCountFormatter.string(fromByteCount: wasted, countStyle: .file) + " could be freed"
    }
    if s["cancelled"] as? Bool ?? false { headline += " (stopped early: not every file was compared)" }
    if let path = table.path({ api.report_path!(table.ctx, id, $0, $1) }) { reportPath = path }
  }

  func file(_ path: String?) -> (DuplicateGroup, DuplicateFile)? {
    guard let path else { return nil }
    for g in groups { if let f = g.files.first(where: { $0.path == path }) { return (g, f) } }
    return nil
  }

  /// Offered while another copy in the group is still on disk.
  func canTrash(_ f: DuplicateFile, in g: DuplicateGroup) -> Bool {
    canTrash && f.state != "trashed" && f.state != "queued" && g.alive > 1
  }

  /// The picked file Return and ⌘R act on: the first picked, in list order.
  var focused: String? {
    for g in groups { if let f = g.files.first(where: { selection.contains($0.path) }) { return f.path } }
    return nil
  }

  /// The picked copies that can go to the Trash, and whether the pick would
  /// take every copy of some file: then nothing is sent, so which copy
  /// survives is never down to the order the engine ran the requests in.
  func batch() -> (files: [DuplicateFile], takesEveryCopy: Bool) {
    var files: [DuplicateFile] = []
    var every = false
    for g in groups {
      let picked = g.files.filter { selection.contains($0.path) && canTrash($0, in: g) }
      if picked.isEmpty { continue }
      let onDisk = g.files.filter { $0.state != "trashed" && $0.state != "queued" }.count
      if onDisk - picked.count < 1 { every = true }
      files += picked
    }
    return (files, every)
  }

  /// Bytes the picked copies would free.
  func batchBytes() -> Int64 {
    var bytes: Int64 = 0
    for g in groups {
      bytes += g.size * Int64(g.files.filter { selection.contains($0.path) && canTrash($0, in: g) }.count)
    }
    return bytes
  }

  /// One copy (a row's own button), or the whole pick.
  func trash(_ path: String) {
    guard let (g, f) = file(path), canTrash(f, in: g) else { return }
    if path.withCString({ api.trash_duplicate!(table.ctx, job, $0) }) == MV_OK { refresh(job) }
  }

  /// No-block: every call only queues; the engine checks and moves the copies
  /// one at a time on its own thread, and a refusal shows on that file's row.
  func trashSelection() {
    let (files, every) = batch()
    guard !running, !files.isEmpty, !every else { return }
    var queued = false
    for f in files {
      guard f.path.withCString({ api.trash_duplicate!(table.ctx, job, $0) }) == MV_OK else { continue }
      selection.remove(f.path)
      queued = true
    }
    if queued { refresh(job) }
  }

  func open(_ path: String) { chrome?.openInViewer(path) }

  func reveal(_ path: String) {
    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
  }

  func showReport() {
    guard !reportPath.isEmpty else { return }
    NSWorkspace.shared.open(URL(fileURLWithPath: reportPath))
  }
}

struct DuplicatesView: View {
  @ObservedObject var model: DuplicatesModel
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Find Duplicates").font(.title2)
      Text(model.folder).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
      if model.running {
        if let f = model.fraction { ProgressView(value: f) } else { ProgressView().progressViewStyle(.linear) }
        Text(model.progressLine).font(.caption).foregroundStyle(.secondary)
      } else {
        Text(model.headline)
        if !model.canTrash && !model.groups.isEmpty {
          Text("Moving to the Trash needs a newer MediaViewer.").font(.caption).foregroundStyle(.secondary)
        }
      }
      List(selection: $model.selection) {
        ForEach(model.groups) { g in
          Section(header: Text("\(g.files.count) identical files · " +
                               ByteCountFormatter.string(fromByteCount: g.size, countStyle: .file) + " each")) {
            ForEach(g.files) { f in row(f, in: g).tag(f.path) }
          }
        }
        if !model.unreadable.isEmpty {
          Section(header: Text("Could not be read")) {
            ForEach(model.unreadable, id: \.self) { Text($0).font(.caption) }
          }
        }
      }
      .onKeyPress(.return) {
        guard let p = model.focused else { return .ignored }
        model.open(p)
        return .handled
      }
      .onKeyPress(characters: ["r"]) { press in
        guard press.modifiers.contains(.command), let p = model.focused else { return .ignored }
        model.reveal(p)
        return .handled
      }
      .onDeleteCommand { model.trashSelection() }
      selectionBar
      HStack {
        Text("Originals stay put. Move to Trash can be undone from the Trash; the last copy of a file is always kept.")
          .font(.caption).foregroundStyle(.secondary)
        Spacer()
        if model.running {
          Button("Stop") { model.cancel() }
        } else {
          Button("Show report") { model.showReport() }.disabled(model.reportPath.isEmpty)
          Button("Scan another folder…") { model.choose() }
        }
        Button("Close") { model.showing = false }.keyboardShortcut(.cancelAction)
      }
    }
    .padding()
    .frame(minWidth: 820, minHeight: 560)
  }

  /// How much is picked, and the batch move.
  @ViewBuilder private var selectionBar: some View {
    if !model.groups.isEmpty {
      let pick = model.batch()
      HStack {
        Text(selectionLine(every: pick.takesEveryCopy)).font(.caption).foregroundStyle(.secondary)
        Spacer()
        if model.selection.count > 1 {
          Button("Deselect All") { model.selection = [] }
        }
        Button(pick.files.count > 1 ? "Move \(pick.files.count) to Trash" : "Move to Trash") { model.trashSelection() }
          .disabled(model.running || pick.files.isEmpty || pick.takesEveryCopy)
          .help(pick.takesEveryCopy ? "Every copy of a file is picked. Leave one unpicked to keep it."
                                    : "⌘⌫. Each copy goes only while another identical copy stays.")
      }
    }
  }

  private func selectionLine(every: Bool) -> String {
    let n = model.selection.count
    if every { return "\(n) picked, including every copy of a file. Leave one copy of each file unpicked." }
    if n > 1 {
      let bytes = ByteCountFormatter.string(fromByteCount: model.batchBytes(), countStyle: .file)
      return "\(n) picked · \(bytes) to free"
    }
    return "⌘-click or ⇧-click to pick several copies, then move them to the Trash together."
  }

  private func row(_ f: DuplicateFile, in g: DuplicateGroup) -> some View {
    HStack {
      VStack(alignment: .leading, spacing: 2) {
        Text(f.relative).strikethrough(f.state == "trashed").lineLimit(1).truncationMode(.middle)
        Text(detail(f)).font(.caption).foregroundStyle(.secondary)
      }
      Spacer()
      Button("Open") { model.open(f.path) }.disabled(f.state == "trashed")
      Button("Show in Finder") { model.reveal(f.path) }.disabled(f.state == "trashed")
      Button("Move to Trash") { model.trash(f.path) }
        .disabled(!model.canTrash(f, in: g))
        .help(g.alive > 1 ? "Moves this copy to the Trash; another copy stays."
                          : "The last copy is always kept.")
    }
    .contextMenu {
      Button("Open in Viewer") { model.open(f.path) }
      Button("Show in Finder") { model.reveal(f.path) }
      Button("Move to Trash") { model.trash(f.path) }.disabled(!model.canTrash(f, in: g))
    }
  }

  private func detail(_ f: DuplicateFile) -> String {
    let when = Date(timeIntervalSince1970: TimeInterval(f.mtime)).formatted(date: .abbreviated, time: .shortened)
    switch f.state {
    case "trashed": return "Moved to the Trash"
    case "queued": return "Checking the other copy, then moving to the Trash…"
    case "refused":
      switch f.reason {
      case "last_copy": return "Kept: no other copy with the same bytes is left"
      case "changed": return "Kept: this file changed since the scan. Scan again."
      case "no_bin": return "Kept: this drive has no Trash, and nothing is deleted outright"
      default: return "Kept: it could not be moved to the Trash"
      }
    default: return "Modified \(when)"
    }
  }
}
