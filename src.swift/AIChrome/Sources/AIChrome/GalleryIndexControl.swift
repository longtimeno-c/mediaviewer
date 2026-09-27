// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery search bar's index control (plan/17 "Gallery search bar",
// 2026-09-27): a compact control at the right end of the base gallery's field
// for the folder the viewer last opened. Not covered: "Index…" with this
// folder / and subfolders. Covered: "Indexing 120 of 800" with a ring, or
// "Indexed", and a menu to pause, choose what videos are indexed for, rescan
// or remove the folder's root. The base embeds it (MVAIChrome
// -galleryAccessory) as it embeds the Settings management view.
//
// Threads: folder_coverage, status, index_folder, root_set_enabled,
// root_rescan and root_set_media are [no-block]; roots_json and root_remove
// are [worker-thread] and run detached. It polls at 2 Hz only while the
// gallery is on screen (the host says so), and nothing here logs a path.
import AppKit
import CAiApi
import SwiftUI

@MainActor
final class GalleryIndexModel: ObservableObject {
  let table: AITable

  @Published private(set) var folder = ""
  /// folder_coverage: 0 not covered, 1 covered and indexing, 2 complete.
  @Published private(set) var coverage: UInt32 = 0
  /// The remembered root that covers `folder` (itself, or a recursive
  /// ancestor); nil until roots_json has been read, or when none does.
  @Published private(set) var root: RootRow?
  /// The Sound piece is loaded: "Videos: Sound / Both" can be chosen.
  @Published private(set) var audioReady = false
  /// Indexing waits on battery: the menu offers "Index anyway".
  @Published private(set) var onBattery = false
  @Published var confirmingRemove = false

  private var roots: [RootRow] = []
  private var visible = false
  private var timer: Timer?
  private var readingRoots = false
  private var rootsAgain = false

  init(table: AITable) { self.table = table }

  /// The Contents search covers the subfolders when the folder's root does.
  var recursive: Bool { root?.recursive ?? false }

  func folderChanged(_ dir: String) {
    guard dir != folder else { return }
    folder = dir
    root = covering(dir)
    refresh()
    if visible { reloadRoots() }
  }

  /// The grid was shown or hidden (MvAddonsGalleryVisible).
  func setVisible(_ on: Bool) {
    visible = on
    timer?.invalidate()
    timer = nil
    guard on else { return }
    refresh()
    reloadRoots()
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.tick() }
    }
  }

  func stop() {
    timer?.invalidate()
    timer = nil
    visible = false
  }

  /// MV_ADDON_EVENT_AI_STATUS / _ROOTS: the next tick would see it anyway;
  /// while shown, show it now.
  func statusChanged() { if visible { refresh() } }
  func rootsChanged() { if visible { reloadRoots() } }

  private func tick() {
    refresh()
    // The counts move only while it indexes (or waits paused mid-way).
    if coverage == 1 || root?.enabled == false { reloadRoots() }
  }

  /// [no-block] reads, on the main actor.
  func refresh() {
    var state: UInt32 = 0
    if folder.isEmpty || table.call({ table.a.folder_coverage?(table.ctx, folder, &state) }) != MV_OK {
      state = 0
    }
    if state != coverage { coverage = state }
    let status = table.status()
    let audio = (status?.flags ?? 0) & MV_AI_STATUS_AUDIO_READY != 0
    if audio != audioReady { audioReady = audio }
    let battery = status.map { $0.state == MV_AI_STATE_YIELDING.rawValue && $0.yield_reason == MV_AI_YIELD_BATTERY.rawValue } ?? false
    if battery != onBattery { onBattery = battery }
  }

  /// roots_json is [worker-thread]: one read at a time, a request made while
  /// one runs is folded into one more read after it.
  func reloadRoots() {
    if readingRoots {
      rootsAgain = true
      return
    }
    readingRoots = true
    let t = table
    Task.detached {
      let json = t.json { t.a.roots_json?(t.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG }
      let rows: [RootRow] = (parseJSON(json) as? [[String: Any]] ?? []).map {
        RootRow(id: UInt64(clamping: int64($0["id"])), path: $0["path"] as? String ?? "",
                recursive: $0["recursive"] as? Bool ?? false, enabled: $0["enabled"] as? Bool ?? true,
                assets: int64($0["assets"]), done: int64($0["done"]), bytes: int64($0["bytes"]),
                media: UInt32(clamping: int64($0["media"])))
      }
      await MainActor.run {
        self.readingRoots = false
        self.roots = rows
        let r = self.covering(self.folder)
        if r != self.root { self.root = r }
        if self.rootsAgain {
          self.rootsAgain = false
          self.reloadRoots()
        }
      }
    }
  }

  /// The folder's own root, else the deepest recursive root above it.
  private func covering(_ dir: String) -> RootRow? {
    guard !dir.isEmpty else { return nil }
    let d = Self.trimmed(dir)
    var best: RootRow?
    for r in roots {
      let p = Self.trimmed(r.path)
      guard !p.isEmpty else { continue }
      let hit = p == d || (r.recursive && d.hasPrefix(p == "/" ? p : p + "/"))
      if hit, p.count > (best.map { Self.trimmed($0.path).count } ?? -1) { best = r }
    }
    return best
  }

  private static func trimmed(_ p: String) -> String {
    p.count > 1 && p.hasSuffix("/") ? String(p.dropLast()) : p
  }

  // MARK: actions

  func index(recursive: Bool) {
    guard !folder.isEmpty else { return }
    var id: UInt64 = 0
    table.call { table.a.index_folder?(table.ctx, folder, recursive ? 1 : 0, &id) }
    refresh()
    reloadRoots()
  }

  func setPaused(_ paused: Bool) {
    guard let root else { return }
    table.call { table.a.root_set_enabled?(table.ctx, root.id, paused ? 0 : 1) }
    reloadRoots()
  }

  func setMedia(_ media: UInt32) {
    guard let root, table.has(\mv_ai_api.root_set_media) else { return }
    table.call { table.a.root_set_media?(table.ctx, root.id, media) }
    reloadRoots()
    refresh()
  }

  func indexAnyway() {
    table.indexAnyway()
    refresh()
  }

  func rescan() {
    guard let root else { return }
    table.call { table.a.root_rescan?(table.ctx, root.id) }
    reloadRoots()
    refresh()
  }

  func remove() {
    confirmingRemove = false
    guard let root else { return }
    let id = root.id
    roots.removeAll { $0.id == id }
    self.root = covering(folder)
    let t = table
    Task.detached {
      // Deletes rows and waits for the indexer to let go: never on main.
      t.call { t.a.root_remove?(t.ctx, id) }
      await MainActor.run {
        self.refresh()
        self.reloadRoots()
      }
    }
  }

  // MARK: what the control shows

  var label: String {
    switch coverage {
    case 0: return "Index…"
    case 1:
      guard let root, root.assets > 0 else { return root?.enabled == false ? "Paused" : "Indexing" }
      let counts = "\(countText(UInt64(max(0, root.done)))) of \(countText(UInt64(root.assets)))"
      if !root.enabled { return "Paused · \(counts)" }
      return onBattery ? "Paused on battery · \(counts)" : "Indexing \(counts)"
    default: return "Indexed"
    }
  }

  var progress: Double {
    guard let root, root.assets > 0 else { return 0 }
    return min(1, Double(max(0, root.done)) / Double(root.assets))
  }

  var rootName: String {
    guard let root else { return "this folder" }
    let leaf = (root.path as NSString).lastPathComponent
    return "“\(leaf.isEmpty ? root.path : leaf)”"
  }
}

/// The control. Uses the base chrome's colour roles and face (AITheme), so it
/// reads as part of the bar in light, dark and Increase Contrast.
struct GalleryIndexControl: View {
  @ObservedObject var model: GalleryIndexModel
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    Group {
      if model.folder.isEmpty {
        EmptyView()
      } else if model.coverage == 0 {
        Menu {
          Button("Index this folder") { model.index(recursive: false) }
          Button("Index this folder and subfolders") { model.index(recursive: true) }
        } label: {
          Label("Index…", systemImage: "square.stack.3d.up")
            .font(AITheme.font(13))
        }
        .help("Add this folder to Local search. It indexes in the background while you keep browsing.")
      } else {
        Menu {
          covered
        } label: {
          HStack(spacing: 6) {
            if model.coverage == 1 {
              AIProgressRing(progress: model.progress,
                             spinning: model.root?.enabled != false && !model.onBattery && !reduceMotion)
                .frame(width: 12, height: 12)
            } else {
              Image(systemName: "checkmark.circle")
            }
            Text(model.label)
              .font(AITheme.font(13))
              .contentTransition(.numericText())
          }
        }
        .help(model.coverage == 1 ? "Local search is indexing this folder in the background: keep browsing."
                                  : "This folder is in Local search")
      }
    }
    .menuStyle(.borderlessButton)
    .fixedSize()
    .foregroundStyle(AITheme.title)
    .accessibilityLabel(model.coverage == 0 ? "Index this folder for Local search" : model.label)
    .alert("Remove \(model.rootName) from the search index?", isPresented: $model.confirmingRemove) {
      Button("Remove", role: .destructive) { model.remove() }
      Button("Cancel", role: .cancel) {}
    } message: {
      Text((model.root?.recursive ?? false ? "It and its subfolders are forgotten and their search data deleted. "
                                           : "It is forgotten and its search data deleted. ") +
           "Your files are not touched.")
    }
  }

  @ViewBuilder private var covered: some View {
    if let root = model.root {
      if model.coverage == 1 && root.enabled && model.onBattery {
        Button("Index anyway, on battery") { model.indexAnyway() }
      }
      if model.coverage == 1 || !root.enabled {
        Button(root.enabled ? "Pause indexing this folder" : "Resume indexing this folder") {
          model.setPaused(root.enabled)
        }
      }
      Menu("Videos: \(Self.mediaName(root.media))") {
        ForEach(ManagementView.mediaChoices) { choice in
          Button {
            model.setMedia(choice.value)
          } label: {
            if root.media == choice.value { Label(choice.label, systemImage: "checkmark") } else { Text(choice.label) }
          }
          .disabled(choice.value & MV_AI_MEDIA_SOUND != 0 && !model.audioReady)
        }
        if !model.audioReady {
          Divider()
          Text("Sound needs the Sound piece: Settings → Local search")
        }
      }
      Button("Rescan") { model.rescan() }
      Divider()
      Button("Remove from index…") { model.confirmingRemove = true }
    } else {
      Text("Reading the index…")
    }
  }

  private static func mediaName(_ media: UInt32) -> String {
    switch media {
    case MV_AI_MEDIA_PICTURES: return "Pictures"
    case MV_AI_MEDIA_SOUND: return "Sound"
    case MV_AI_MEDIA_BOTH: return "Both"
    default: return "Default"
    }
  }
}
