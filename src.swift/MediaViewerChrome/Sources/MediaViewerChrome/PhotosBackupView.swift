// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/26 "Backup": Settings -> Photos Library. Every original of the Photos
// library, copied and verified into a folder of the user's choosing (a NAS,
// a drive) in the folder layout they pick (YYYY/YYYY-MM-DD unless changed; issue
// #414), by the host's engine (shell/photos_backup.h over PhotoKit). This view
// only asks and watches: the destination and layout pickers, Back Up Now /
// Cancel, the run's progress, and the last run. Shown once the
// library was added in Settings -> Local search.
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
    let folders = mv_chrome_photos_backup_layout()
    if folders != layout { layout = folders }
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
              if store.running { store.cancel() } else { store.start() }
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
          if store.running {
            // The shared progress line (ProgressLine.swift): the track, then one
            // line with the time left once there is a rate. Cancel is the
            // button above, so the line has none of its own (say it once).
            ProgressLine(verb: store.run.state == 1 ? "Listing your library" : "Backing up",
                         fraction: store.run.state == 1 ? nil : progressFraction,
                         detail: store.run.state == 1 ? nil : progressDetail,
                         key: "photos-backup")
              .transition(.opacity)
          } else if store.run.state >= 3 {
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
