// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Import window's model over the mv.import.1 table (mediaviewer_import.h),
// the Mac twin of ImportWindow.cs. Calls marked [no-block] in the header run
// on the main actor; the ones that read files or import.db run detached.
import AppKit
import CImportApi
import Foundation

struct ImportTile: Identifiable, Equatable {
  let index: Int
  let name: String
  let badge: String
  let state: String
  let path: String
  let tip: String
  var selected: Bool
  var id: Int { index }
  var dimmed: Bool { state == "duplicate" || state == "imported" || state == "filtered" }
}

struct ImportDay: Identifiable, Equatable {
  let day: String
  var header: String
  var tiles: [ImportTile]
  var id: String { day }
}

struct FolderCount: Identifiable, Equatable {
  let folder: String
  let units: Int
  var id: String { folder }
}

struct ImportSource: Identifiable, Equatable {
  let root: String
  let label: String
  let detail: String
  let volumeID: String
  var id: String { root }
}

/// A thin, checked view of the C table.
final class ImportTable: @unchecked Sendable {
  let api: UnsafePointer<mv_import_api>

  init(_ api: UnsafePointer<mv_import_api>) { self.api = api }

  var ctx: UnsafeMutableRawPointer? { api.pointee.ctx }

  /// The buffer rule: retry with `needed`.
  func json(_ call: (UnsafeMutablePointer<CChar>?, UInt32, UnsafeMutablePointer<UInt32>?) -> mv_status) -> String? {
    var cap: UInt32 = 64 * 1024
    for _ in 0..<4 {
      var buf = [CChar](repeating: 0, count: Int(cap))
      var needed: UInt32 = 0
      let s = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, cap, &needed) }
      if s == MV_OK { return String(cString: buf) }
      if s != MV_ERR_INVALID_ARG || needed <= cap { return nil }
      cap = needed + 1024
    }
    return nil
  }

  func path(_ call: (UnsafeMutablePointer<CChar>?, UInt32) -> mv_status) -> String? {
    var buf = [CChar](repeating: 0, count: 32 * 1024)
    let s = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, UInt32($0.count)) }
    return s == MV_OK ? String(cString: buf) : nil
  }

  func id(_ call: (UnsafeMutablePointer<UInt64>) -> mv_status) -> UInt64? {
    var out: UInt64 = 0
    return call(&out) == MV_OK ? out : nil
  }
}

func parse(_ text: String?) -> Any? {
  guard let text else { return nil }
  return try? JSONSerialization.jsonObject(with: Data(text.utf8))
}

@MainActor
final class ImportModel: ObservableObject {
  let table: ImportTable
  weak var chrome: MVImportChrome?

  @Published var sources: [ImportSource] = []
  @Published var selectedSource: String = ""
  @Published var title = ""
  @Published var days: [ImportDay] = []
  @Published var folders: [FolderCount] = []
  @Published var bottom = ""
  @Published var importCount = 0
  @Published var preset: [String: Any] = [:]
  @Published var presetNames: [String] = []
  @Published var copying = false
  @Published var progressLine = ""
  @Published var progress: [Double] = []
  @Published var paused = false
  @Published var summary: [String] = []
  @Published var summaryTitle = ""
  @Published var canRetry = false
  @Published var reportPath = ""
  @Published var banner = ""
  @Published var bannerJob: UInt64 = 0
  @Published var thumbs: [Int: NSImage] = [:]
  @Published var historyRows: [String] = []

  var marks: [String] = []
  private var presets: [[String: Any]] = []
  private var volumeID = ""
  private var scan: UInt64 = 0
  private(set) var plan: UInt64 = 0
  private(set) var job: UInt64 = 0
  private var destination = ""
  private var requested = Set<Int>()

  init(table: ImportTable) { self.table = table }

  var api: mv_import_api { table.api.pointee }

  // MARK: sources

  func refreshSources(select: String?) {
    let t = table
    Task.detached {
      let sources = t.json { t.api.pointee.sources_json!(t.ctx, $0, $1, $2) }
      let presets = t.json { t.api.pointee.presets_json!(t.ctx, $0, $1, $2) }
      await MainActor.run { self.applySources(sources, presets, select) }
    }
  }

  private func applySources(_ json: String?, _ presetsJSON: String?, _ select: String?) {
    if let p = parse(presetsJSON) as? [String: Any] {
      presets = p["presets"] as? [[String: Any]] ?? []
      presetNames = presets.compactMap { $0["name"] as? String }
      if preset.isEmpty {
        let last = p["last"] as? String ?? ""
        preset = presets.first(where: { $0["name"] as? String == last }) ?? presets.first ?? ["name": "Default"]
        if (preset["destination"] as? String ?? "").isEmpty {
          let lastDest = p["last_destination"] as? String ?? ""
          preset["destination"] = lastDest.isEmpty ? (p["default_destination"] as? String ?? "") : lastDest
        }
      }
    }
    var list: [ImportSource] = []
    for s in parse(json) as? [[String: Any]] ?? [] {
      let total = s["total_bytes"] as? Int64 ?? Int64(s["total_bytes"] as? Int ?? 0)
      let fresh = s["new_count"] as? Int ?? -1
      let kind = s["kind"] as? String ?? ""
      let root = s["root"] as? String ?? ""
      var detail = total > 0 ? ByteCountFormatter.string(fromByteCount: total, countStyle: .file) + " · " : ""
      detail += fresh >= 0 ? "\(fresh) new" : (kind == "network" ? "network (slower)" : "")
      let label = (s["label"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? root
      list.append(ImportSource(root: root, label: label, detail: detail, volumeID: s["volume_id"] as? String ?? ""))
    }
    let current = select ?? (selectedSource.isEmpty ? nil : selectedSource)
    if let current, !list.contains(where: { $0.root == current }) {
      list.insert(ImportSource(root: current, label: current, detail: "", volumeID: ""), at: 0)
    }
    sources = list
    if let pick = list.first(where: { $0.root == current }) ?? list.first, pick.root != selectedSource || scan == 0 {
      load(pick)
    }
  }

  func load(_ source: ImportSource) {
    selectedSource = source.root
    volumeID = source.volumeID
    title = source.root + " · reading…"
    days = []
    thumbs = [:]
    requested = []
    plan = 0
    scan = table.id { api.scan!(table.ctx, source.root, $0) } ?? 0
  }

  func nextSource(_ step: Int) {
    guard !sources.isEmpty else { return }
    let i = sources.firstIndex(where: { $0.root == selectedSource }) ?? 0
    load(sources[(i + step + sources.count) % sources.count])
  }

  func addFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    guard panel.runModal() == .OK, let url = panel.url else { return }
    let t = table
    let path = url.path
    Task.detached {
      _ = t.api.pointee.add_folder_source!(t.ctx, path)
      await MainActor.run { self.refreshSources(select: path) }
    }
  }

  // MARK: events

  func scanDone(_ id: UInt64, _ status: UInt32) {
    guard id == scan else { return }
    if status != MV_OK.rawValue { title = selectedSource + " · could not be read"; return }
    replan()
  }

  func replan() {
    guard scan != 0, let data = try? JSONSerialization.data(withJSONObject: preset),
          let text = String(data: data, encoding: .utf8) else { return }
    let marked: String? = (preset["selection"] as? String) == "marked"
      ? String(data: (try? JSONSerialization.data(withJSONObject: marks)) ?? Data("[]".utf8), encoding: .utf8)
      : nil
    plan = table.id { out in
      text.withCString { p in
        if let marked { return marked.withCString { m in api.plan!(table.ctx, scan, p, m, out) } }
        return api.plan!(table.ctx, scan, p, nil, out)
      }
    } ?? 0
  }

  func planReady(_ id: UInt64, _ status: UInt32) {
    guard id == plan else { return }
    guard status == MV_OK.rawValue,
          let root = parse(table.json { api.plan_json!(table.ctx, id, $0, $1, $2) }) as? [String: Any],
          let totals = root["totals"] as? [String: Any] else {
      bottom = "Could not plan this import."
      return
    }
    let source = root["source"] as? [String: Any] ?? [:]
    let label = (source["label"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? selectedSource
    let n = { (k: String) in totals[k] as? Int ?? 0 }
    title = "\(label) · \(n("new")) new of \(n("units")) · " +
      ByteCountFormatter.string(fromByteCount: Int64(n("bytes")), countStyle: .file)
    destination = root["destination"] as? String ?? ""

    var byDay: [String: ImportDay] = [:]
    var order: [String] = []
    for u in root["units"] as? [[String: Any]] ?? [] {
      let day = u["day"] as? String ?? ""
      let kind = u["kind"] as? String ?? ""
      let type = u["type"] as? String ?? ""
      let state = u["state"] as? String ?? "new"
      let matched = u["matched"] as? String ?? ""
      let tile = ImportTile(
        index: u["i"] as? Int ?? 0,
        name: (u["sources"] as? [String])?.first ?? "",
        badge: kind == "raw_jpeg" ? "RAW+JPEG" : kind == "live_photo" ? "LIVE" : type == "video" ? "▶" : "",
        state: state,
        path: u["path"] as? String ?? "",
        tip: state == "duplicate" ? "Already in library: " + matched : state == "imported" ? "Imported from this card before" : "",
        selected: u["selected"] as? Bool ?? false)
      if byDay[day] == nil { byDay[day] = ImportDay(day: day, header: day, tiles: []); order.append(day) }
      byDay[day]!.tiles.append(tile)
    }
    for d in root["days"] as? [[String: Any]] ?? [] {
      guard let day = d["day"] as? String, var g = byDay[day] else { continue }
      let units = d["units"] as? Int ?? 0, fresh = d["new"] as? Int ?? 0, sel = d["selected"] as? Int ?? 0
      let check = sel == 0 ? "☐" : sel == units ? "☑" : "◧"
      g.header = "\(check) \(day) · \(units)" + (fresh == units ? " (all new)" : fresh == 0 ? " (already imported)" : " (\(fresh) new)")
      byDay[day] = g
    }
    days = order.compactMap { byDay[$0] }
    folders = (root["folders"] as? [[String: Any]] ?? []).map {
      let f = $0["folder"] as? String ?? ""
      return FolderCount(folder: f.isEmpty ? "(destination)" : f, units: $0["units"] as? Int ?? 0)
    }
    let eta = totals["eta_seconds"] as? Int ?? -1
    let rate = totals["bytes_per_second"] as? Double ?? 0
    bottom = "\(n("selected_files")) files · " +
      ByteCountFormatter.string(fromByteCount: Int64(n("selected_bytes")), countStyle: .file) +
      " · \(n("duplicates")) duplicates skipped" +
      (eta >= 0 ? " · ≈ \(max(1, eta / 60)) min at \(Int(rate / 1_000_000)) MB/s" : " · time shown after the first import from this device")
    importCount = n("selected_units")
  }

  func thumbnail(for tile: ImportTile) {
    guard !requested.contains(tile.index), plan != 0 else { return }
    requested.insert(tile.index)
    let t = table, p = plan, i = UInt32(tile.index)
    Task.detached(priority: .utility) {
      let path = t.path { t.api.pointee.thumbnail!(t.ctx, p, i, $0, $1) }
      let image = path.flatMap { NSImage(contentsOfFile: $0) }
      await MainActor.run { if let image { self.thumbs[Int(i)] = image } }
    }
  }

  func toggle(_ tile: ImportTile) {
    guard !copying else { return }
    _ = api.select!(table.ctx, plan, Int32(tile.index), nil, tile.selected ? 0 : 1)
  }

  func toggleDay(_ day: String) {
    guard !copying, let g = days.first(where: { $0.day == day }) else { return }
    let any = g.tiles.contains { $0.selected }
    _ = day.withCString { api.select!(table.ctx, plan, -1, $0, any ? 0 : 1) }
  }

  // MARK: presets

  func string(_ key: String, _ fallback: String = "") -> String { preset[key] as? String ?? fallback }
  func bool(_ key: String, _ fallback: Bool) -> Bool { preset[key] as? Bool ?? fallback }

  func set(_ key: String, _ value: Any) {
    preset[key] = value
    replan()
  }

  func choosePreset(_ name: String) {
    guard let p = presets.first(where: { $0["name"] as? String == name }) else { return }
    preset = p
    replan()
  }

  func chooseFolder(_ key: String) {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.canCreateDirectories = true
    guard panel.runModal() == .OK, let url = panel.url else { return }
    set(key, url.path)
  }

  func savePreset(named name: String) {
    preset["name"] = name.isEmpty ? "Default" : name
    guard let data = try? JSONSerialization.data(withJSONObject: preset),
          let text = String(data: data, encoding: .utf8) else { return }
    let t = table
    Task.detached {
      _ = t.api.pointee.save_preset!(t.ctx, text)
      await MainActor.run { self.refreshSources(select: self.selectedSource) }
    }
  }

  func bindCard(use: Bool, auto: Bool) {
    guard !volumeID.isEmpty else { return }
    let t = table, vol = volumeID, name = use ? string("name", "Default") : ""
    Task.detached { _ = t.api.pointee.bind_card!(t.ctx, vol, name, (use && auto) ? 1 : 0) }
  }

  var hasCard: Bool { !volumeID.isEmpty }

  // MARK: jobs

  func start() {
    guard !copying, plan != 0, let id = table.id({ api.start!(table.ctx, plan, $0) }) else { return }
    job = id
    copying = true
    summary = []
    chrome?.track(id, label: title.components(separatedBy: " · ").first ?? "Import")
  }

  func pauseResume() {
    guard copying else { return }
    var p = mv_import_progress()
    guard api.progress!(table.ctx, job, &p) == MV_OK else { return }
    _ = api.pause!(table.ctx, job, p.state == MV_IMPORT_JOB_PAUSED.rawValue ? 0 : 1)
  }

  func cancel() { _ = api.cancel!(table.ctx, job) }

  func progressed(_ id: UInt64, _ p: mv_import_progress) {
    guard id == job, copying else { return }
    var verified = p.bytes_verified
    let total = Double(max(1, p.bytes_total))
    progress = withUnsafeBytes(of: &verified) { raw in
      let v = raw.bindMemory(to: UInt64.self)
      return (0..<Int(min(2, p.destination_count))).map { Double(v[$0]) / total }
    }
    paused = p.state == MV_IMPORT_JOB_PAUSED.rawValue
    var name = p.current_name_utf8
    let current = withUnsafeBytes(of: &name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
    let rate = p.bytes_per_second > 0 ? " · \(Int(p.bytes_per_second / 1_000_000)) MB/s" : ""
    let eta = p.eta_seconds >= 0 ? " · ≈ \(max(1, p.eta_seconds / 60)) min" : ""
    progressLine = "\(paused ? "Paused" : "Copying") \(current) · \(p.units_done) of \(p.units_total) verified\(rate)\(eta)"
  }

  func finished(_ id: UInt64) {
    guard id == job else { return }
    copying = false
    guard let s = parse(table.json { api.summary_json!(table.ctx, id, $0, $1, $2) }) as? [String: Any] else { return }
    if s["kind"] as? String == "verify" {
      let problems = s["problems"] as? [[String: Any]] ?? []
      summaryTitle = "Verified \(s["ok"] as? Int ?? 0) files: " + (problems.isEmpty ? "all intact." : "\(problems.count) problems.")
      summary = problems.map { "\($0["problem"] as? String ?? ""): \($0["path"] as? String ?? "")" }
      canRetry = false
      reportPath = ""
      return
    }
    let copied = s["copied"] as? [String: Any] ?? [:]
    let skipped = s["skipped"] as? [[String: Any]] ?? []
    let failed = s["failed"] as? [[String: Any]] ?? []
    summaryTitle = "\(copied["files"] as? Int ?? 0) copied · \(skipped.count) skipped · \(failed.count) failed" +
      ((s["ejected"] as? Bool ?? false) ? " · card ejected" : "")
    summary = failed.map { "Failed: \($0["name"] as? String ?? "") — \($0["reason"] as? String ?? "")" } +
      skipped.prefix(500).map { "Skipped: \($0["name"] as? String ?? "") — matches \($0["matched"] as? String ?? "")" }
    canRetry = !failed.isEmpty
    reportPath = s["report"] as? String ?? ""
    if let root = sources.first(where: { $0.root == selectedSource }) { load(root) }  // what is new now
  }

  func retryFailed() {
    guard let id = table.id({ api.retry_failed!(table.ctx, job, $0) }) else { return }
    job = id
    copying = true
    chrome?.track(id, label: "retry")
  }

  func eject() {
    let t = table, root = selectedSource
    Task.detached {
      let ok = t.api.pointee.eject!(t.ctx, root) == MV_OK
      await MainActor.run { self.bottom = ok ? "Ejected. The card can be removed." : "The card is in use and was not ejected." }
    }
  }

  func openDestination() { chrome?.openInViewer(destination) }
  func open(_ tile: ImportTile) { chrome?.openInViewer(tile.path) }

  func showReport() {
    guard !reportPath.isEmpty else { return }
    NSWorkspace.shared.open(URL(fileURLWithPath: reportPath))
  }

  func checkUnfinished() {
    let t = table
    Task.detached {
      let json = t.json { t.api.pointee.unfinished_json!(t.ctx, $0, $1, $2) }
      await MainActor.run {
        guard let first = (parse(json) as? [[String: Any]])?.first,
              let job = first["job"] as? Int else { return }
        let label = (first["label"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? first["source"] as? String ?? ""
        self.banner = "An import from \(label) was interrupted: \(first["pending"] as? Int ?? 0) files not copied yet. Verified files are kept."
        self.bannerJob = UInt64(job)
      }
    }
  }

  func resumeUnfinished() {
    guard bannerJob != 0, api.resume!(table.ctx, bannerJob) == MV_OK else { return }
    job = bannerJob
    copying = true
    chrome?.track(bannerJob, label: "resume")
    banner = ""
  }

  func verifyFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    guard panel.runModal() == .OK, let url = panel.url,
          let id = table.id({ out in url.path.withCString { api.verify_folder!(table.ctx, $0, out) } }) else { return }
    job = id
    copying = true
    chrome?.track(id, label: "verify")
  }

  /// Reads import.db off the main thread (rule 1: history_json waits on the
  /// database a running job is writing), then publishes `historyRows`.
  func loadHistory() {
    let t = table
    Task.detached {
      let text = t.json { t.api.pointee.history_json!(t.ctx, $0, $1, $2) }
      await MainActor.run { self.historyRows = ImportModel.historyLines(text) }
    }
  }

  nonisolated static func historyLines(_ text: String?) -> [String] {
    return (parse(text) as? [[String: Any]] ?? []).map { j in
      let when = Date(timeIntervalSince1970: TimeInterval(j["created"] as? Int ?? 0))
      let files = ((j["summary"] as? [String: Any])?["copied"] as? [String: Any])?["files"] as? Int
      return "\(when.formatted(date: .abbreviated, time: .shortened)) · \(j["kind"] as? String ?? "") · \(j["label"] as? String ?? "") \(j["source"] as? String ?? "") · \(j["state"] as? String ?? "")" +
        (files.map { " · \($0) files" } ?? "")
    }
  }
}
