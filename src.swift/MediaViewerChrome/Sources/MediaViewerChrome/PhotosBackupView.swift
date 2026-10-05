// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/26 "Backup": Settings -> Photos Library. Every original of the Photos
// library, copied and verified into a folder of the user's choosing (a NAS,
// a drive) under YYYY/YYYY-MM-DD, by the host's engine (shell/photos_backup.h
// over PhotoKit). This view only asks and watches: the destination picker,
// Back Up Now / Cancel, the run's progress, and the last run. Shown once the
// library was added in Settings -> Local search.
import AppKit
import MVChromeBridge
import SwiftUI

@MainActor
final class PhotosBackupStore: ObservableObject {
  static let shared = PhotosBackupStore()

  @Published private(set) var available = false
  @Published private(set) var destination = ""
  @Published private(set) var run = mv_chrome_photos_backup()
  @Published private(set) var last: mv_chrome_photos_backup?
  @Published private(set) var startFailed = false
  private var timer: Timer?

  private init() {
    refresh()
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.refresh() }
    }
  }

  var running: Bool { run.state == 1 || run.state == 2 }

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
    var p = mv_chrome_photos_backup()
    mv_chrome_photos_backup_progress(&p)
    if !sameBackup(p, run) { run = p }
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
    startFailed = !destination.withCString { mv_chrome_photos_backup_start($0) }
    refresh()
  }

  func cancel() { mv_chrome_photos_backup_cancel() }

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

/// Field-by-field, since a C struct with fixed-size char arrays gets no
/// synthesized Equatable (and the CI runner's Swift predates `@retroactive`).
private func sameBackup(_ a: mv_chrome_photos_backup, _ b: mv_chrome_photos_backup) -> Bool {
  a.state == b.state && a.total == b.total && a.done == b.done && a.skipped == b.skipped
    && a.failed == b.failed && a.fetched == b.fetched && a.bytes == b.bytes
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

struct PhotosBackupSection: View {
  @ObservedObject private var store = PhotosBackupStore.shared

  private var destinationLabel: String {
    store.destination.isEmpty ? "No folder chosen yet" : (store.destination as NSString).abbreviatingWithTildeInPath
  }

  private var progressFraction: Double {
    let p = store.run
    guard p.total > 0 else { return 0 }
    return min(1, Double(p.done + p.skipped + p.failed) / Double(p.total))
  }

  private var progressLine: String {
    let p = store.run
    if p.state == 1 { return "Listing your library…" }
    var parts = ["\(countText(p.done + p.skipped + p.failed)) of \(countText(p.total))"]
    if p.done > 0 { parts.append("\(countText(p.done)) copied (\(bytesText(p.bytes)))") }
    if p.skipped > 0 { parts.append("\(countText(p.skipped)) already there") }
    if p.failed > 0 { parts.append("\(countText(p.failed)) failed") }
    let current = cString(p.current)
    if !current.isEmpty { parts.append(current) }
    return parts.joined(separator: " · ")
  }

  private func summary(_ p: mv_chrome_photos_backup, prefix: String) -> String {
    var parts = [prefix]
    switch p.state {
    case 3: break
    case 4: parts[0] += " (cancelled)"
    case 5: parts[0] += " (stopped on an error)"
    default: break
    }
    parts.append("\(countText(p.done)) copied")
    parts.append("\(countText(p.skipped)) already there")
    if p.failed > 0 { parts.append("\(countText(p.failed)) failed") }
    return parts.joined(separator: " · ")
  }

  var body: some View {
    if store.available {
      VStack(alignment: .leading, spacing: 8) {
        Text("Photos Library").font(MVTheme.font(16)).fontWeight(.semibold)
          .foregroundStyle(MVTheme.title).padding(.top, 12)
        VStack(alignment: .leading, spacing: 10) {
          HStack(alignment: .top, spacing: 24) {
            VStack(alignment: .leading, spacing: 4) {
              Text("Back up originals to a folder").font(MVTheme.font()).foregroundStyle(MVTheme.title)
              Text("Copies every original in your Photos library — iCloud Photos and hidden items included, a Live Photo's video and a RAW+JPEG pair's RAW too — into year and day folders, and verifies each copy. "
                   + "Originals that are only in iCloud are downloaded for this. Run it again any time: only what is new or missing is copied. Your library is never changed.")
                .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
                .fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if store.running {
              Button("Cancel") { store.cancel() }
            } else {
              Button(store.destination.isEmpty ? "Choose Folder…" : "Back Up Now") { store.start() }
                .keyboardShortcut(.defaultAction)
            }
          }
          HStack(spacing: 8) {
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
          if store.running {
            ProgressView(value: progressFraction).progressViewStyle(.linear)
            Text(progressLine).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
              .lineLimit(1).truncationMode(.middle)
          } else if store.run.state >= 3 {
            HStack(spacing: 8) {
              Image(systemName: store.run.state == 3 && store.run.failed == 0 ? "checkmark.circle" : "exclamationmark.triangle")
                .foregroundStyle(store.run.state == 3 && store.run.failed == 0 ? Color.green : Color.orange)
              Text(store.run.state == 5 ? cString(store.run.error) : summary(store.run, prefix: "This run"))
                .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
              Button("Show Report") { store.showReport() }.controlSize(.small)
            }
          } else if let last = store.last {
            Text(summary(last, prefix: "Last backup \(dateText(last.finished_unix))"))
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          }
          if store.startFailed {
            Text("The backup could not start. Check that the folder can be written.")
              .font(MVTheme.font(12)).foregroundStyle(.orange)
          }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(MVTheme.surface))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(MVTheme.hairline, lineWidth: 1))
      }
    }
  }
}
