// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The chrome slots an add-on fills (docs/design/25 §7, PR 59): command-bar and
// path-bar buttons, status text, gallery tile badges and context-menu entries.
// Each is drawn from SlotStore's tables like the chrome's own items, so it
// takes the theme, the keyboard and VoiceOver's names; choosing one posts to
// the add-on's worker and returns. The Windows twin is IslandHost.Slots.cs.
import AppKit
import MVChromeBridge
import SwiftUI

/// What an item targets, as paths (docs/design/16: the marks if any, else the
/// current item).
@MainActor
enum SlotTargets {
  static func folder() -> String {
    let need = Int(mv_chrome_current_folder(nil, 0))
    guard need > 0 else { return "" }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_current_folder($0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }

  static func path(at index: Int) -> String? {
    guard index >= 0 else { return nil }
    let need = Int(mv_chrome_item_path(Int32(index), nil, 0))
    guard need > 0 else { return nil }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_item_path(Int32(index), $0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }

  static func paths(for item: SlotItem, tile: Int? = nil) -> [String] {
    let store = FolderStore.shared
    switch item.target {
    case "folder":
      let f = folder()
      return f.isEmpty ? [] : [f]
    case "file":
      return path(at: tile ?? store.currentIndex).map { [$0] } ?? []
    case "files":
      // A tile that is one of the marks takes the marks; otherwise that tile
      // (or the current item).
      let index = tile ?? store.currentIndex
      if store.markedCount > 0, index < 0 || store.markedNames.contains(store.names[safe: index] ?? "") {
        return store.names.indices.filter { store.markedNames.contains(store.names[$0]) }.compactMap { path(at: $0) }
      }
      return path(at: index).map { [$0] } ?? []
    default:
      return []
    }
  }
}

extension Array {
  subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil }
}

/// The add-ons' command-bar buttons and status text, after the chrome's own.
struct SlotBarItems: View {
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    HStack(spacing: 2) {
      ForEach(slots.items(in: "bar")) { item in
        Button {
          slots.invoke(item, paths: SlotTargets.paths(for: item))
        } label: {
          Label(item.label, systemImage: slotSymbol(item.symbol))
            .labelStyle(.iconOnly)
        }
        .buttonStyle(FlatButtonStyle())
        .help(item.label)
        .accessibilityLabel(item.label)
      }
      ForEach(slots.status, id: \.id) { status in
        Text(status.text)
          .font(MVTheme.font(13))
          .foregroundStyle(MVTheme.body)
          .lineLimit(1)
          .padding(.horizontal, 6)
          .help(status.label)
          .accessibilityLabel("\(status.label): \(status.text)")
      }
    }
  }
}

/// The add-ons' path-bar icons, after the search icon.
struct SlotPathItems: View {
  @ObservedObject private var slots = SlotStore.shared
  @State private var hover: String?

  var body: some View {
    ForEach(slots.items(in: "path")) { item in
      Button {
        slots.invoke(item, paths: SlotTargets.paths(for: item))
      } label: {
        Image(systemName: slotSymbol(item.symbol))
          .font(.system(size: 12, weight: .semibold))
          .frame(width: 26, height: 26)
          .background(RoundedRectangle(cornerRadius: 4).fill(Color.primary.opacity(hover == item.id ? 0.06 : 0)))
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .foregroundStyle(MVTheme.title)
      .onHover { hover = $0 ? item.id : nil }
      .help(item.label)
      .accessibilityLabel(item.label)
    }
  }
}

/// Whether any add-on contributes tile badges or context-menu entries. Its own
/// small store, set only when contributions change, so a gallery with no such
/// add-on builds plain cells (no badge view observing SlotStore, no context menu).
@MainActor
final class SlotTileFlags: ObservableObject {
  static let shared = SlotTileFlags()
  @Published private(set) var badges = false
  @Published private(set) var context = false
  func set(badges: Bool, context: Bool) {
    if badges != self.badges { self.badges = badges }
    if context != self.context { self.context = context }
  }
}

struct SlotBadgesOverlay: ViewModifier {
  let on: Bool
  let name: String
  func body(content: Content) -> some View {
    if on {
      content.overlay(alignment: .bottomLeading) { SlotTileBadges(name: name) }
    } else {
      content
    }
  }
}

struct SlotContextMenu: ViewModifier {
  let on: Bool
  let tile: Int
  func body(content: Content) -> some View {
    if on {
      content.contextMenu { SlotContextEntries(tile: tile) }
    } else {
      content
    }
  }
}

/// A gallery tile's badges: a table lookup by name (SlotStore.badgesByName),
/// never an add-on call (PR 59 verify: counted).
struct SlotTileBadges: View {
  let name: String
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    let badges = slots.badges(forName: name)
    if !badges.isEmpty {
      HStack(spacing: 2) {
        ForEach(badges, id: \.key) { badge in
          Image(systemName: slotSymbol(badge.symbol))
            .font(.system(size: 10, weight: .semibold))
            .foregroundStyle(.white)
            .padding(4)
            .background(Circle().fill(MVTheme.accent.opacity(0.9)))
            .help(badge.label)
        }
      }
      .padding(4)
      .accessibilityElement(children: .ignore)
      .accessibilityLabel(badges.map(\.label).joined(separator: ", "))
    }
  }
}

/// The add-ons' context-menu entries for a gallery tile (Control-click, or
/// VoiceOver's Show Menu).
struct SlotContextEntries: View {
  let tile: Int
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    ForEach(slots.items(in: "context")) { item in
      Button(item.label) { slots.invoke(item, paths: SlotTargets.paths(for: item, tile: tile)) }
    }
  }
}

/// Under a Local search result list's title: what the other local-scope
/// providers (Places) matched for the same query, and one click to add them
/// to the list. Absent until one answers with rows.
struct SlotLocalMore: View {
  let title: String
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    let rows = slots.localResults.rows.filter { $0.addon != "ai" }
    if !rows.isEmpty {
      let names = Set(rows.map(\.provider)).count == 1 ? (rows.first?.why ?? "") : ""
      Button {
        addToList(rows)
      } label: {
        Text(names.isEmpty ? "+ \(rows.count) more from add-ons" : "+ \(rows.count) in \(names)")
      }
      .buttonStyle(FlatButtonStyle(compact: true))
      .help("Add what other Local search providers matched to these results")
      .accessibilityLabel("Add \(rows.count) more results from add-ons")
    } else if slots.localResults.searching && slots.localResults.total > 0 {
      Text("Still reading tags — \(slots.localResults.read) of \(slots.localResults.total)")
        .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
    }
  }

  private func addToList(_ rows: [SlotSearchRow]) {
    var paths: [String] = []
    var seen = Set<String>()
    for i in 0..<Int(mv_chrome_item_count()) {
      if let p = SlotTargets.path(at: i), seen.insert(p).inserted { paths.append(p) }
    }
    for r in rows where seen.insert(r.path).inserted { paths.append(r.path) }
    let c = paths.map { strdup($0) }
    defer { c.forEach { free($0) } }
    let ptrs: [UnsafePointer<CChar>?] = c.map { UnsafePointer($0) }
    _ = title.withCString { t in
      ptrs.withUnsafeBufferPointer { mv_chrome_open_list(t, $0.baseAddress, nil, Int32(paths.count), 0, true) }
    }
    // They are in the list now: the offer goes.
    slots.search(scope: 2, query: "", folder: "")
  }
}

/// Settings -> Add-ons: MediaViewer's own add-ons that fill screens and slots
/// (Places), each with what it is and Remove. Removing takes every item it
/// added away at once, with no restart. Absent when none is installed.
struct SlotAddonsSection: View {
  struct Row: Identifiable {
    let id: String
    let name: String
    let version: String
    let description: String
    let state: String
    let loaded: Bool
  }

  @ObservedObject private var settings = SettingsStore.shared
  @State private var rows: [Row] = []
  @State private var confirming: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      ForEach(rows) { row in
        VStack(alignment: .leading, spacing: 6) {
          Text(row.name).font(MVTheme.font()).foregroundStyle(MVTheme.title)
          if !row.description.isEmpty {
            Text(row.description).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
              .fixedSize(horizontal: false, vertical: true)
          }
          Text(stateText(row)).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
          if confirming == row.id {
            HStack {
              Button("Remove, keep its data") { remove(row.id, keep: true) }
              Button("Remove everything") { remove(row.id, keep: false) }
              Button("Cancel") { confirming = nil }
            }
          } else {
            Button("Remove…") { confirming = row.id }.accessibilityLabel("Remove \(row.name)")
          }
        }
      }
    }
    .onAppear { refresh() }
    .onChange(of: settings.visible) { _, visible in if visible { refresh() } }
  }

  private func stateText(_ row: Row) -> String {
    switch row.state {
    case "ok": return row.loaded ? "Installed, version \(row.version)." : "Installed, version \(row.version); starting…"
    case "needs_update": return "Version \(row.version) needs a newer MediaViewer."
    default: return "The installed copy did not pass verification and is not loaded."
    }
  }

  private func remove(_ id: String, keep: Bool) {
    confirming = nil
    _ = id.withCString { mv_chrome_slot_addon_remove($0, keep) }
    rows.removeAll { $0.id == id }
  }

  private func refresh() {
    Task.detached(priority: .utility) {
      // Reads the store (its hashes are cached per process): off the main actor.
      let need = Int(mv_chrome_slot_addons(nil, 0))
      var buf = [CChar](repeating: 0, count: need + 1)
      _ = buf.withUnsafeMutableBufferPointer { mv_chrome_slot_addons($0.baseAddress, Int32($0.count)) }
      let list = (try? JSONSerialization.jsonObject(with: Data(String(cString: buf).utf8))) as? [[String: Any]] ?? []
      let parsed = list.compactMap { o -> Row? in
        guard let id = o["id"] as? String else { return nil }
        return Row(id: id, name: o["name"] as? String ?? id, version: o["version"] as? String ?? "",
                   description: o["description"] as? String ?? "", state: o["state"] as? String ?? "",
                   loaded: (o["loaded"] as? Bool) ?? false)
      }
      await MainActor.run { rows = parsed }
    }
  }
}
