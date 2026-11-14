// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/26 "Backup": Settings -> Photos Library. Every original of the Photos
// library, copied and verified into a folder of the user's choosing (a NAS,
// a drive) in the folder layout they pick (YYYY/YYYY-MM-DD unless changed; issue
// #414), by the host's engine (shell/photos_backup.h over PhotoKit). This view
// only asks and watches: the destination and layout pickers, Back Up Now /
// Cancel, the run's progress, and the last run. Shown once the
// library was added in Settings -> Local search.
//
// Check library (docs/plans/check-library.md): with Import installed, the
// backup skips a photo whose bytes are already anywhere in the folder (or in
// every library Import knows), and Check library reports what is there, what
// would be copied, what has gone since it was backed up, and the duplicates
// the folder already holds. Report only: it copies and deletes nothing.
import AppKit
import MVChromeBridge
import SwiftUI

@MainActor
final class PhotosBackupStore: ObservableObject {
  static let shared = PhotosBackupStore()

  @Published private(set) var available = false
  @Published private(set) var destination = ""
  /// backup::layout, as mv_chrome_photos_backup_layout numbers it.
  @Published private(set) var layout: Int32 = 0
  @Published private(set) var run = mv_chrome_photos_backup()
  @Published private(set) var last: mv_chrome_photos_backup?
  @Published private(set) var startFailed = false
  /// backup::library_scope: 0 this folder, 1 every library Import knows.
  @Published private(set) var scope: Int32 = 0
  /// Import is loaded and has the Check library calls.
  @Published private(set) var index = false
  /// The run on screen is a check, not a backup.
  @Published private(set) var checking = false
  @Published private(set) var report: CheckReport?
  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), and
    // only while Settings, where the backup shows, is open; its timer runs
    // while a backup there is in progress.
    ChromePulse.shared.add { [weak self] in
      guard let self, mv_chrome_settings_visible() else { return false }
      self.refresh()
      return self.running
    }
  }

  var running: Bool { run.state == 1 || run.state == 2 || run.state == 6 }

  /// The welcome card's iCloud Photos row opens Settings at this section. A
  /// count, so a second click while Settings is open scrolls again; `pending`
  /// carries a request made before the page first appeared.
  static let anchor = "photos-library-section"
  @Published private(set) var revealRequests = 0
  private var revealPending = false
  func reveal() {
    revealPending = true
    revealRequests += 1
  }
  func takeReveal() -> Bool {
    defer { revealPending = false }
    return revealPending
  }

  private func refresh() {
    let now = mv_chrome_photos_library_available()
    if now != available { available = now }
    guard now else { return }
    var buf = [CChar](repeating: 0, count: 4096)
    let n = buf.withUnsafeMutableBufferPointer { mv_chrome_photos_backup_destination($0.baseAddress, Int32($0.count)) }
    let dest = n > 0 ? String(cString: buf) : ""
    if dest != destination { destination = dest }
    let folders = mv_chrome_photos_backup_layout()
    if folders != layout { layout = folders }
    let s = mv_chrome_photos_backup_scope()
    if s != scope { scope = s }
    let i = mv_chrome_photos_library_index()
    if i != index { index = i }
    var p = mv_chrome_photos_backup()
    mv_chrome_photos_backup_progress(&p)
    if !sameBackup(p, run) {
      let ended = running && !(p.state == 1 || p.state == 2 || p.state == 6)
      run = p
      if ended && checking { loadReport() }
    }
    var l = mv_chrome_photos_backup()
    if mv_chrome_photos_backup_last(&l) {
      if last == nil || !sameBackup(last!, l) { last = l }
    } else if last != nil {
      last = nil
    }
  }

  func chooseDestination() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.canCreateDirectories = true
    panel.allowsMultipleSelection = false
    panel.prompt = "Choose"
    panel.message = "Choose the folder your Photos library is backed up into (a NAS or a drive works)."
    if !destination.isEmpty { panel.directoryURL = URL(fileURLWithPath: destination) }
    guard panel.runModal() == .OK, let url = panel.url else { return }
    destination = url.path
    start()
  }

  func start() {
    guard !destination.isEmpty else { chooseDestination(); return }
    checking = false
    report = nil
    startFailed = !destination.withCString { mv_chrome_photos_backup_start($0) }
    refresh()
  }

  func cancel() { mv_chrome_photos_backup_cancel() }

  func check() {
    guard !destination.isEmpty else { return }
    let ok = destination.withCString { mv_chrome_photos_check_start($0) }
    startFailed = !ok
    if ok {
      checking = true
      report = nil
    }
    refresh()
  }

  func setScope(_ s: Int32) {
    mv_chrome_set_photos_backup_scope(s)
    scope = mv_chrome_photos_backup_scope()
  }

  /// A backup that starts clears the check's view (start does).
  func startBackup() { start() }

  private func loadReport() {
    let need = mv_chrome_photos_check_json(nil, 0)
    guard need > 0 else { return }
    var buf = [CChar](repeating: 0, count: Int(need) + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_photos_check_json($0.baseAddress, Int32($0.count)) }
    report = CheckReport(json: Data(String(cString: buf).utf8))
  }

  func reveal(_ path: String) {
    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
  }

  /// Copy again / Leave deleted for some of the report's removed files. The
  /// host writes the manifest off the main thread; the list shows the choice
  /// at once, and the next check or backup reads it back.
  @Published private(set) var decideFailed = false
  /// A choice is being written; the list's buttons wait.
  @Published private(set) var deciding = false
  /// The choices as they were before the write on its way, to put back if it fails.
  private var undo: [Int: String] = [:]
  func decide(_ ids: [Int], copyAgain: Bool) {
    guard !deciding, var r = report else { return }
    let picked = r.deletedFiles.filter { ids.contains($0.id) }
    guard !picked.isEmpty else { return }
    let rows: [[String: Any]] = picked.map { ["asset": $0.asset, "kind": $0.kind, "copy_again": copyAgain] }
    guard let data = try? JSONSerialization.data(withJSONObject: rows),
          let text = String(data: data, encoding: .utf8) else { return }
    let ok = text.withCString { mv_chrome_photos_decide_deleted($0) }
    decideFailed = !ok
    guard ok else { return }
    undo = [:]
    for i in r.deletedFiles.indices where ids.contains(r.deletedFiles[i].id) {
      undo[r.deletedFiles[i].id] = r.deletedFiles[i].choice
      r.deletedFiles[i].choice = copyAgain ? "again" : "left"
    }
    report = r
    deciding = true
    watchDecision()
  }

  /// Until the host has written it: then the choice stands, or goes back with
  /// a line saying it was not saved.
  private func watchDecision() {
    Task { @MainActor [weak self] in
      while let self, mv_chrome_photos_decide_state() == 1 {
        try? await Task.sleep(nanoseconds: 100_000_000)
      }
      guard let self else { return }
      self.deciding = false
      if mv_chrome_photos_decide_state() == 2, var r = self.report {
        for i in r.deletedFiles.indices {
          if let before = self.undo[r.deletedFiles[i].id] { r.deletedFiles[i].choice = before }
        }
        self.report = r
        self.decideFailed = true
      }
      self.undo = [:]
    }
  }

  func setLayout(_ l: Int32) {
    mv_chrome_set_photos_backup_layout(l)
    layout = mv_chrome_photos_backup_layout()
  }

  func showReport() {
    guard !destination.isEmpty else { return }
    let report = URL(fileURLWithPath: destination)
      .appendingPathComponent(".mediaviewer-photos-backup", isDirectory: true)
      .appendingPathComponent("last-run.txt")
    NSWorkspace.shared.open(report)
  }

  func revealDestination() {
    guard !destination.isEmpty else { return }
    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: destination)])
  }
}

/// Check library's answer (mv_chrome_photos_check_json).
struct CheckReport {
  struct Found: Identifiable {
    let id: Int
    let name: String
    let whereAt: [String]
  }
  struct Gone: Identifiable {
    let id: Int
    let place: String
    let movedTo: String
  }
  struct Group: Identifiable {
    let id: Int
    let size: UInt64
    let paths: [String]
  }
  /// Backed up, then removed from the folder (owner, 2026-11-14): held until
  /// someone chooses. `choice`: "" (held), "left" (Leave deleted, remembered by
  /// hash), "again" (Copy again: the next backup copies it).
  struct Deleted: Identifiable {
    let id: Int
    let asset: String
    let kind: Int
    let name: String
    let place: String
    let bytes: UInt64
    var choice: String
  }
  var originals: UInt64 = 0
  var inBackup: UInt64 = 0
  var toCopy: UInt64 = 0
  var toCopyBytes: UInt64 = 0
  var unconfirmed: UInt64 = 0
  var libraryFiles: UInt64 = 0
  var found: [Found] = []
  var gone: [Gone] = []
  var duplicates: [Group] = []
  var deletedFiles: [Deleted] = []

  init?(json: Data) {
    guard let o = try? JSONSerialization.jsonObject(with: json) as? [String: Any] else { return nil }
    func u(_ k: String) -> UInt64 { (o[k] as? NSNumber)?.uint64Value ?? 0 }
    originals = u("originals")
    inBackup = u("in_backup")
    toCopy = u("to_copy")
    toCopyBytes = u("to_copy_bytes")
    unconfirmed = u("unconfirmed")
    libraryFiles = u("library_files")
    for (i, f) in ((o["in_library"] as? [[String: Any]]) ?? []).enumerated() {
      found.append(Found(id: i, name: f["name"] as? String ?? "", whereAt: f["where"] as? [String] ?? []))
    }
    for (i, g) in ((o["gone"] as? [[String: Any]]) ?? []).enumerated() {
      gone.append(Gone(id: i, place: g["rel"] as? String ?? "", movedTo: g["moved_to"] as? String ?? ""))
    }
    for (i, d) in ((o["deleted"] as? [[String: Any]]) ?? []).enumerated() {
      deletedFiles.append(Deleted(id: i, asset: d["asset"] as? String ?? "", kind: (d["kind"] as? NSNumber)?.intValue ?? 0,
                                  name: d["name"] as? String ?? "", place: d["place"] as? String ?? "",
                                  bytes: (d["bytes"] as? NSNumber)?.uint64Value ?? 0,
                                  choice: (d["left"] as? Bool ?? false) ? "left" : ""))
    }
    for (i, g) in ((o["duplicates"] as? [[String: Any]]) ?? []).enumerated() {
      duplicates.append(Group(id: i, size: (g["size"] as? NSNumber)?.uint64Value ?? 0, paths: g["paths"] as? [String] ?? []))
    }
  }

  var moved: Int { gone.filter { !$0.movedTo.isEmpty }.count }
  var held: Int { deletedFiles.filter { $0.choice.isEmpty }.count }
}

/// Field-by-field, since a C struct with fixed-size char arrays gets no
/// synthesized Equatable (and the CI runner's Swift predates `@retroactive`).
private func sameBackup(_ a: mv_chrome_photos_backup, _ b: mv_chrome_photos_backup) -> Bool {
  a.state == b.state && a.total == b.total && a.done == b.done && a.skipped == b.skipped
    && a.failed == b.failed && a.fetched == b.fetched && a.bytes == b.bytes
    && a.found == b.found && a.bytes_total == b.bytes_total && a.held == b.held
    && a.left_deleted == b.left_deleted
    && a.started_unix == b.started_unix && a.finished_unix == b.finished_unix
    && withUnsafeBytes(of: a.current) { Array($0) } == withUnsafeBytes(of: b.current) { Array($0) }
    && withUnsafeBytes(of: a.error) { Array($0) } == withUnsafeBytes(of: b.error) { Array($0) }
}

private func cString<T>(_ tuple: T) -> String {
  withUnsafeBytes(of: tuple) { raw in
    let bytes = raw.prefix { $0 != 0 }
    return String(decoding: bytes, as: UTF8.self)
  }
}

private func bytesText(_ n: UInt64) -> String {
  ByteCountFormatter.string(fromByteCount: Int64(clamping: n), countStyle: .file)
}

private func countText(_ n: UInt64) -> String {
  let f = NumberFormatter()
  f.numberStyle = .decimal
  return f.string(from: NSNumber(value: n)) ?? "\(n)"
}

private func dateText(_ unix: Int64) -> String {
  let d = Date(timeIntervalSince1970: TimeInterval(unix))
  let f = DateFormatter()
  f.dateStyle = .medium
  f.timeStyle = .short
  return f.string(from: d)
}

/// Where Back Up Now and Check library look, in backup::library_scope's numbering.
private let backupScopes: [(value: Int32, label: String)] = [
  (0, "This folder"),
  (1, "Every library Import knows"),
]

/// A long list stays readable: the first rows, then how many more (the
/// report file has every one).
private let reportRows = 200

/// The layouts, in backup::layout's numbering, with what each writes.
private let backupLayouts: [(value: Int32, label: String)] = [
  (1, "Year — 2024/IMG_0001.HEIC"),
  (5, "Year and month — 2024/05/IMG_0001.HEIC"),
  (0, "Year and day — 2024/2024-05-17/IMG_0001.HEIC"),
  (2, "Year, month and day — 2024/05/17/IMG_0001.HEIC"),
  (3, "Day — 2024-05-17/IMG_0001.HEIC"),
  (4, "Flat — IMG_0001.HEIC"),
]

struct PhotosBackupSection: View {
  @ObservedObject private var store = PhotosBackupStore.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  /// docs/design/20 "Craft rules": a change of state fades in its slot; Reduce Motion cuts.
  private var quick: Animation? { reduceMotion ? nil : .easeOut(duration: 0.15) }

  private var layoutBinding: Binding<Int32> {
    Binding(get: { store.layout }, set: { store.setLayout($0) })
  }

  private var scopeBinding: Binding<Int32> {
    Binding(get: { store.scope }, set: { store.setScope($0) })
  }

  private var destinationLabel: String {
    store.destination.isEmpty ? "No folder chosen yet" : (store.destination as NSString).abbreviatingWithTildeInPath
  }

  private var progressFraction: Double {
    let p = store.run
    guard p.total > 0 else { return 0 }
    return min(1, Double(p.done + p.skipped + p.failed) / Double(p.total))
  }

  /// The run's numbers for the shared progress line ("Backing up · 1,234 of
  /// 5,678 · about 20 min left"). Short and fixed: the line's numbers never
  /// truncate, so the file being copied (which changed many times a second)
  /// is not among them; what was copied and what was already there is the
  /// summary's to say when the run ends.
  private var progressDetail: String {
    let p = store.run
    var parts = ["\(countText(p.done + p.skipped + p.failed)) of \(countText(p.total))"]
    if p.failed > 0 { parts.append("\(countText(p.failed)) failed") }
    return parts.joined(separator: " · ")
  }

  private var runVerb: String {
    switch store.run.state {
    case 1: return "Listing your library"
    case 6: return "Checking the folder"
    default: return store.checking ? "Comparing" : "Backing up"
    }
  }

  private var runFraction: Double? {
    let p = store.run
    if p.state == 1 { return nil }
    if p.state == 6 { return p.bytes_total > 0 ? min(1, Double(p.bytes) / Double(p.bytes_total)) : nil }
    return progressFraction
  }

  private var runOK: Bool { store.run.state == 3 && store.run.failed == 0 }

  private func summary(_ p: mv_chrome_photos_backup, prefix: String) -> String {
    var parts = [prefix]
    switch p.state {
    case 3: break
    case 4: parts[0] += " (cancelled)"
    case 5: parts[0] += " (stopped on an error)"
    default: break
    }
    parts.append("\(countText(p.done)) copied")
    if p.found > 0 { parts.append("\(countText(p.found)) found in the library") }
    if p.held > 0 { parts.append("\(countText(p.held)) removed from the folder, not copied back") }
    parts.append("\(countText(p.skipped)) already there")
    if p.failed > 0 { parts.append("\(countText(p.failed)) failed") }
    return parts.joined(separator: " · ")
  }

  var body: some View {
    if store.available {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        Text("Photos Library").font(MVTheme.font(16)).fontWeight(.semibold)
          .foregroundStyle(MVTheme.title).padding(.top, MVTheme.space(12))
        VStack(alignment: .leading, spacing: MVTheme.space(10)) {
          HStack(alignment: .top, spacing: MVTheme.space(24)) {
            VStack(alignment: .leading, spacing: MVTheme.space(4)) {
              Text("Back up originals to a folder").font(MVTheme.font()).foregroundStyle(MVTheme.title)
              Text("Copies every original in your Photos library — iCloud Photos and hidden items included, a Live Photo's video and a RAW+JPEG pair's RAW too — into folders by date, and verifies each copy. "
                   + "Originals that are only in iCloud are downloaded for this. Run it again any time: only what is new or missing is copied. Your library is never changed. "
                   + "Changing the folders applies to files copied from then on; what is already backed up stays where it is.")
                .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
                .fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            // One button whose words change on itself, not two swapped in.
            Button {
              if store.running { store.cancel() } else { store.startBackup() }
            } label: {
              Text(store.running ? "Cancel" : store.destination.isEmpty ? "Choose folder…" : "Back up now")
                .contentTransition(.opacity)
            }
            .keyboardShortcut(store.running ? nil : .defaultAction)
            .animation(quick, value: store.running)
          }
          HStack(spacing: MVTheme.space(8)) {
            Image(systemName: "externaldrive").foregroundStyle(MVTheme.body)
            Text(destinationLabel).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
              .lineLimit(1).truncationMode(.middle)
              .help(store.destination)
            if !store.destination.isEmpty {
              Button("Reveal") { store.revealDestination() }.controlSize(.small)
            }
            Button(store.destination.isEmpty ? "Choose…" : "Change…") { store.chooseDestination() }
              .controlSize(.small).disabled(store.running)
            Spacer(minLength: 0)
          }
          HStack(spacing: MVTheme.space(8)) {
            Image(systemName: "folder").foregroundStyle(MVTheme.body)
            Text("Folders").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            Picker("Folders", selection: layoutBinding) {
              ForEach(backupLayouts, id: \.value) { Text($0.label).tag($0.value) }
            }
            .labelsHidden().controlSize(.small).fixedSize().disabled(store.running)
            Spacer(minLength: 0)
          }
          HStack(spacing: MVTheme.space(8)) {
            Image(systemName: "square.on.square").foregroundStyle(MVTheme.body)
            Text("Skip photos already in").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            Picker("Skip photos already in", selection: scopeBinding) {
              ForEach(backupScopes, id: \.value) { Text($0.label).tag($0.value) }
            }
            .labelsHidden().controlSize(.small).fixedSize().disabled(store.running || !store.index)
            Button("Check library") { store.check() }
              .controlSize(.small)
              .disabled(store.running || !store.index || store.destination.isEmpty)
              .help("Compares your Photos library with the folder, by content: what is already there, what would be copied, what was deleted since, and duplicates. Changes nothing.")
            Spacer(minLength: 0)
          }
          if !store.index {
            Text("Install Import (Settings → Add-ons) to find photos that are already in the folder, however they got there, and skip them.")
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
              .fixedSize(horizontal: false, vertical: true)
          }
          if store.running {
            // The shared progress line (ProgressLine.swift): the track, then one
            // line with the time left once there is a rate. Cancel is the
            // button above, so the line has none of its own (say it once).
            ProgressLine(verb: runVerb,
                         fraction: runFraction,
                         detail: store.run.state == 1 || store.run.state == 6 ? nil : progressDetail,
                         key: "photos-backup")
              .transition(.opacity)
          } else if store.checking, let r = store.report {
            CheckReportView(report: r, reveal: store.reveal,
                            decide: { ids, again in store.decide(ids, copyAgain: again) },
                            decideFailed: store.decideFailed, deciding: store.deciding)
              .transition(.opacity)
          } else if store.run.state >= 3 && !store.checking {
            // The outcome takes the line's slot, as ProgressLine's own does: the
            // track's room kept with the bar faded out, then the result.
            VStack(alignment: .leading, spacing: MVTheme.space(6)) {
              ProgressTrack(fraction: 1, faded: true)
              HStack(spacing: MVTheme.space(8)) {
                Image(systemName: runOK ? "checkmark.circle.fill" : "exclamationmark.circle.fill")
                  .font(.system(size: 12))
                  .foregroundStyle(runOK ? MVTheme.accent : MVTheme.warm)
                  .accessibilityHidden(true)
                Text(store.run.state == 5 ? cString(store.run.error) : summary(store.run, prefix: "This run"))
                  .font(MVTheme.font(12).monospacedDigit()).foregroundStyle(MVTheme.title)
                  .lineLimit(2)
                Spacer(minLength: 0)
                if store.run.held > 0 && store.index {
                  QuietButton(title: "Review…") { store.check() }
                }
                QuietButton(title: "Show report") { store.showReport() }
              }
              .frame(minHeight: 22)
            }
            .transition(.opacity)
          } else if let last = store.last {
            Text(summary(last, prefix: "Last backup \(dateText(last.finished_unix))"))
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          }
          if store.startFailed {
            HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(6)) {
              Image(systemName: "exclamationmark.circle.fill").font(.system(size: 12)).foregroundStyle(MVTheme.warm)
                .accessibilityHidden(true)
              Text("The backup could not start. Check that the folder can be written.")
                .font(MVTheme.font(12)).foregroundStyle(MVTheme.title)
            }
            .transition(.opacity.combined(with: .offset(y: -4)))
          }
        }
        .animation(quick, value: store.run.state)
        .animation(quick, value: store.startFailed)
        .padding(MVTheme.space(14))
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
        .settingsCardOutline(MVTheme.radius(8))  // fill only; the hairline under Increase Contrast, as SettingsRow
      }
    }
  }
}

/// Check library's report: counts first, each list behind its own disclosure.
private struct CheckReportView: View {
  let report: CheckReport
  let reveal: (String) -> Void
  let decide: ([Int], Bool) -> Void
  let decideFailed: Bool
  let deciding: Bool
  /// Open while anything waits for a choice.
  @State private var expandedDeleted = true

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      ProgressTrack(fraction: 1, faded: true)
      line("checkmark.circle", "\(countText(UInt64(report.found.count) + report.inBackup)) of \(countText(report.originals)) originals are already in the folder"
           + (report.inBackup > 0 ? " (\(countText(report.inBackup)) backed up before)" : ""))
      line("arrow.down.circle", report.toCopy == 0 ? "Nothing would be copied"
           : "\(countText(report.toCopy)) would be copied" + (report.toCopyBytes > 0 ? ", \(bytesText(report.toCopyBytes))" : ""))
      if report.unconfirmed > 0 {
        line("icloud", "\(countText(report.unconfirmed)) only in iCloud have a size the folder has; they are checked when they are backed up")
      }
      if !report.found.isEmpty {
        DisclosureGroup("Found under another name or folder (\(countText(UInt64(report.found.count))))") {
          ForEach(report.found.prefix(reportRows)) { f in
            row(f.name, f.whereAt.first ?? "")
          }
          more(report.found.count)
        }
        .font(MVTheme.font(12))
      }
      if !report.deletedFiles.isEmpty {
        // Owner, 2026-11-14: never brought back by itself; nothing happens to
        // these until someone chooses here.
        DisclosureGroup("Removed from the folder since they were backed up (\(countText(UInt64(report.deletedFiles.count))))",
                        isExpanded: $expandedDeleted) {
          Text("Back Up Now does not copy these again until you choose. Your Photos library still has every one.")
            .font(MVTheme.font(12)).foregroundStyle(MVTheme.body).fixedSize(horizontal: false, vertical: true)
          if report.held > 1 {
            HStack(spacing: MVTheme.space(8)) {
              Button("Leave all deleted") { decide(report.deletedFiles.filter { $0.choice.isEmpty }.map(\.id), false) }
                .controlSize(.small).disabled(deciding)
              Button("Copy all again") { decide(report.deletedFiles.filter { $0.choice.isEmpty }.map(\.id), true) }
                .controlSize(.small).disabled(deciding)
            }
          }
          ForEach(report.deletedFiles.prefix(reportRows)) { d in
            HStack(spacing: MVTheme.space(8)) {
              VStack(alignment: .leading, spacing: 1) {
                Text(d.name).font(MVTheme.font(12)).foregroundStyle(MVTheme.title).lineLimit(1).truncationMode(.middle)
                Text(d.choice == "left" ? "Left deleted" : d.choice == "again" ? "Copied at the next backup" : "Not copied: choose")
                  .font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
              }
              Spacer(minLength: 0)
              if d.choice != "again" {
                Button("Copy again") { decide([d.id], true) }.controlSize(.small).disabled(deciding)
              }
              if d.choice.isEmpty {
                Button("Leave deleted") { decide([d.id], false) }.controlSize(.small).disabled(deciding)
              }
            }
          }
          more(report.deletedFiles.count)
          if decideFailed {
            Text("That choice could not be saved to the folder. Check that it is reachable, then try again.")
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.warm)
          }
        }
        .font(MVTheme.font(12))
      }
      if report.moved > 0 {
        DisclosureGroup("Moved inside the folder since they were backed up (\(report.moved))") {
          ForEach(report.gone.filter { !$0.movedTo.isEmpty }.prefix(reportRows)) { g in
            row(g.place, g.movedTo)
          }
          more(report.moved)
        }
        .font(MVTheme.font(12))
      }
      if !report.duplicates.isEmpty {
        DisclosureGroup("Duplicates already in the folder (\(countText(UInt64(report.duplicates.count))) sets)") {
          ForEach(report.duplicates.prefix(reportRows)) { g in
            VStack(alignment: .leading, spacing: 2) {
              Text("\(g.paths.count) identical files, \(bytesText(g.size)) each")
                .font(MVTheme.font(12)).foregroundStyle(MVTheme.title)
              ForEach(g.paths, id: \.self) { p in pathButton(p) }
            }
          }
          more(report.duplicates.count)
        }
        .font(MVTheme.font(12))
      }
      Text("Nothing was copied, moved or deleted. Find duplicates (in Import) can move a copy to the Trash.")
        .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  private func line(_ icon: String, _ text: String) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(8)) {
      Image(systemName: icon).font(.system(size: 12)).foregroundStyle(MVTheme.body).accessibilityHidden(true)
      Text(text).font(MVTheme.font(12).monospacedDigit()).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  private func row(_ left: String, _ right: String) -> some View {
    HStack(spacing: MVTheme.space(8)) {
      Text(left).font(MVTheme.font(12)).foregroundStyle(MVTheme.title).lineLimit(1).truncationMode(.middle)
      Text("→").foregroundStyle(MVTheme.body).accessibilityHidden(true)
      if right.hasPrefix("/") { pathButton(right) } else {
        Text(right).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }
      Spacer(minLength: 0)
    }
  }

  private func pathButton(_ path: String) -> some View {
    Button((path as NSString).abbreviatingWithTildeInPath) { reveal(path) }
      .buttonStyle(.link).font(MVTheme.font(12)).lineLimit(1).truncationMode(.middle)
      .help("Show in Finder")
  }

  @ViewBuilder private func more(_ total: Int) -> some View {
    if total > reportRows {
      Text("and \(countText(UInt64(total - reportRows))) more").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
    }
  }
}
