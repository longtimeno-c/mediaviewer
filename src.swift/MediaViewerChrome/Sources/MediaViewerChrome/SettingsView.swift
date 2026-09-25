// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Settings screen (plan/16 "Settings"), laid out like the Windows one
// (IslandHost.Settings.cs): grouped General preferences and a separate keyboard
// shortcuts tab, with a persistent Done footer. Native owns every value -- this view
// reads the flags and the live command table through the bridge and posts
// changes back, so the keys, `?` and this list cannot drift.
import Combine
import Foundation
import SwiftUI
import MVChromeBridge

struct KeyRow: Identifiable, Equatable {
  let row: Int32
  let name: String
  let keys: String
  var id: Int32 { row }
}

@MainActor
final class SettingsStore: ObservableObject {
  static let shared = SettingsStore()

  static let filmstripFolder: Int32 = 1 << 0
  static let filmstripImage: Int32 = 1 << 1
  static let wrap: Int32 = 1 << 2
  static let stickyZoom: Int32 = 1 << 3
  static let backgroundShift: Int32 = 4
  static let backgroundMask: Int32 = 3 << 4

  @Published private(set) var visible = false
  @Published private(set) var flags: Int32 = 0
  @Published private(set) var rows: [KeyRow] = []
  @Published private(set) var captureRow: Int32 = -1
  /// PR 9: packed sort order (bits 0-2 key, bit 3 descending), as sort_order.h.
  @Published private(set) var sort: Int32 = 0

  private var timer: Timer?
  private var generation: UInt64 = .max

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private func poll() {
    let v = mv_chrome_settings_visible()
    if v != visible { visible = v }
    guard v else { return }
    // The menu can change the sort too, and that does not move the keys generation.
    let sortNow = mv_chrome_sort_order()
    if sortNow != sort { sort = sortNow }
    let g = mv_chrome_keys_generation()
    if g == generation { return }
    generation = g
    flags = mv_chrome_view_flags()
    captureRow = mv_chrome_key_capture_row()
    rows = Self.readTable()
  }

  /// "Ctrl+Shift+O" -> "⇧⌘O": the table's labels in Mac glyphs. Command and
  /// Control both count as Ctrl in the table, so ⌘ is what the bar shows.
  nonisolated static func macLabel(_ keys: String) -> String {
    keys.replacingOccurrences(of: "Ctrl+", with: "⌘")
      .replacingOccurrences(of: "Alt+", with: "⌥")
      .replacingOccurrences(of: "Shift+", with: "⇧")
  }

  /// The shortcut currently bound to the command called `name`, for menu labels.
  static func keyLabel(_ name: String) -> String? {
    guard let row = readTable().first(where: { $0.name == name }), !row.keys.isEmpty else { return nil }
    return macLabel(row.keys)
  }

  private static func readTable() -> [KeyRow] {
    let need = Int(mv_chrome_command_table(nil, 0))
    guard need > 0 else { return [] }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = mv_chrome_command_table(&buf, Int32(buf.count))
    let text = String(cString: buf)
    var seen = Set<Int32>()
    var out: [KeyRow] = []
    for line in text.split(separator: "\n") {
      let f = line.split(separator: "\t", omittingEmptySubsequences: false)
      guard f.count >= 6, let row = Int32(f[5]), !seen.contains(row) else { continue }
      seen.insert(row)
      out.append(KeyRow(row: row, name: String(f[2]), keys: String(f[3])))
    }
    return out
  }

  func setFlag(_ bit: Int32, _ on: Bool) {
    mv_chrome_set_view_flags(on ? flags | bit : flags & ~bit)
    generation = .max
    poll()
  }

  var background: Int {
    get { Int((flags & Self.backgroundMask) >> Self.backgroundShift) }
    set {
      mv_chrome_set_view_flags((flags & ~Self.backgroundMask) | (Int32(newValue & 3) << Self.backgroundShift))
      generation = .max
      poll()
    }
  }

  var sortKey: Int {
    get { Int(sort & 7) }
    set { mv_chrome_set_sort_order((sort & 8) | Int32(newValue & 7)); sort = mv_chrome_sort_order() }
  }
  var sortDescending: Bool {
    get { sort & 8 != 0 }
    set { mv_chrome_set_sort_order(newValue ? sort | 8 : sort & ~8); sort = mv_chrome_sort_order() }
  }

  func beginCapture(_ row: Int32) { mv_chrome_key_capture_begin(row); generation = .max; poll() }
  func cancelCapture() { mv_chrome_key_capture_cancel(); generation = .max; poll() }
  func resetKeys() { mv_chrome_keys_reset(); generation = .max; poll() }
  func close() { mv_chrome_menu(18) }
}

// A separate label column keeps every switch on the same trailing edge,
// including rows whose descriptions wrap at smaller window sizes.
private struct SettingsRow<Content: View>: View {
  let title: String
  let detail: String
  @ViewBuilder let content: () -> Content

  var body: some View {
    HStack(spacing: 24) {
      VStack(alignment: .leading, spacing: 4) {
        Text(title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Text(detail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      content()
        .labelsHidden()
        .accessibilityLabel(title)
    }
    .padding(14)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.04)))
  }
}

private struct SettingsToggle: View {
  let title: String
  let detail: String
  let bit: Int32
  @ObservedObject var store: SettingsStore
  var body: some View {
    SettingsRow(title: title, detail: detail) {
      Toggle(title, isOn: Binding(
        get: { store.flags & bit != 0 },
        set: { store.setFlag(bit, $0) }))
        .toggleStyle(.switch)
        .fixedSize()
    }
  }
}

private struct SettingsButton: View {
  let title: String
  let action: () -> Void
  var body: some View {
    Button(title, action: action).buttonStyle(SettingsButtonStyle())
  }
}

private struct SettingsButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(MVTheme.font())
      .foregroundStyle(MVTheme.title)
      .padding(.horizontal, 12).padding(.vertical, 7)
      .background(RoundedRectangle(cornerRadius: 6)
        .fill(Color.white.opacity(configuration.isPressed ? 0.16 : 0.08)))
      .contentShape(Rectangle())
  }
}

struct SettingsView: View {
  @ObservedObject private var store = SettingsStore.shared
  @State private var filter = ""
  @State private var keyboard = false

  private var shown: [KeyRow] {
    let q = filter.trimmingCharacters(in: .whitespaces).lowercased()
    if q.isEmpty { return store.rows }
    return store.rows.filter {
      $0.name.lowercased().contains(q) || $0.keys.lowercased().contains(q)
        || SettingsStore.macLabel($0.keys).lowercased().contains(q)
    }
  }

  private var hint: String {
    if !keyboard { return "Changes are saved automatically." }
    return store.captureRow >= 0
      ? "Press the new shortcut, or Esc to cancel."
      : "Choose a shortcut to change it. Conflicts swap shortcuts."
  }

  private func section(_ title: String) -> some View {
    Text(title).font(MVTheme.font(16)).fontWeight(.semibold)
      .foregroundStyle(MVTheme.title).padding(.top, 12)
  }

  private var preferences: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 8) {
        section("Filmstrip")
        SettingsToggle(title: "When opening a folder", detail: "Show thumbnails below the viewer.",
                       bit: SettingsStore.filmstripFolder, store: store)
        SettingsToggle(title: "When opening an image", detail: "Show nearby images from the same folder.",
                       bit: SettingsStore.filmstripImage, store: store)
        section("Browsing")
        SettingsToggle(title: "Wrap at the end", detail: "Continue from the last item to the first.",
                       bit: SettingsStore.wrap, store: store)
        SettingsToggle(title: "Keep pan and zoom", detail: "Keep your view position when moving to the next item.",
                       bit: SettingsStore.stickyZoom, store: store)
        SettingsRow(title: "Sort folder by", detail: "Applies to the gallery and filmstrip.") {
          Picker("Sort folder by", selection: Binding(get: { store.sortKey }, set: { store.sortKey = $0 })) {
            Text("Name").tag(0)
            Text("Date modified").tag(1)
            Text("Size").tag(2)
            Text("Type").tag(3)
            Text("Date taken (EXIF)").tag(4)
          }
          .pickerStyle(.menu).frame(width: 180)
        }
        SettingsRow(title: "Descending order", detail: "Reverse the selected sort order.") {
          Toggle("Descending order", isOn: Binding(get: { store.sortDescending }, set: { store.sortDescending = $0 }))
            .toggleStyle(.switch).fixedSize()
        }
        section("Appearance")
        SettingsRow(title: "Canvas background", detail: "The area behind your photos and videos.") {
          Picker("Canvas background", selection: Binding(get: { store.background }, set: { store.background = $0 })) {
            Text("Dark").tag(0)
            Text("Grey").tag(1)
            Text("White").tag(2)
            Text("Checkerboard").tag(3)
          }
          .pickerStyle(.menu).frame(width: 180)
        }
        AddonsSection().padding(.top, 20)
      }
      .padding(.horizontal, 24).padding(.bottom, 24)
      .frame(maxWidth: 800)
      .frame(maxWidth: .infinity)
    }
  }

  private var shortcuts: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("Keyboard shortcuts").font(MVTheme.font(18)).foregroundStyle(MVTheme.title)
        Spacer()
        SettingsButton(title: "Reset to default") { store.resetKeys() }
      }
      TextField("Search commands or keys", text: $filter)
        .textFieldStyle(.roundedBorder)
        .font(MVTheme.font())
        .onExitCommand {
          if store.captureRow >= 0 { store.cancelCapture() }
          else if !filter.isEmpty { filter = "" }
          else { store.close() }
        }
      ScrollView {
        LazyVStack(spacing: 0) {
          ForEach(shown) { row in
            HStack(spacing: 16) {
              Text(row.name).font(MVTheme.font()).foregroundStyle(MVTheme.title)
                .frame(maxWidth: .infinity, alignment: .leading)
              Button {
                store.beginCapture(row.row)
              } label: {
                Text(store.captureRow == row.row ? "Press shortcut…"
                     : (row.keys.isEmpty ? "Unbound" : SettingsStore.macLabel(row.keys)))
                  .frame(width: 160)
              }
              .buttonStyle(SettingsButtonStyle())
              .accessibilityLabel("Change shortcut for " + row.name)
            }
            .padding(.vertical, 6)
            Rectangle().fill(MVTheme.hairline.opacity(0.5)).frame(height: 1)
          }
          if shown.isEmpty {
            Text("No matching shortcuts").font(MVTheme.font()).foregroundStyle(MVTheme.body)
              .padding(.top, 16)
          }
        }
      }
    }
    .padding(24)
    .frame(maxWidth: 900)
    .frame(maxWidth: .infinity)
  }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 24) {
        Text("Settings").font(MVTheme.font(24)).foregroundStyle(MVTheme.title)
        Spacer(minLength: 0)
        Picker("Settings category", selection: $keyboard) {
          Text("General").tag(false)
          Text("Keyboard shortcuts").tag(true)
        }
        .pickerStyle(.segmented).labelsHidden().frame(maxWidth: 320)
      }
      .padding(.horizontal, 24).padding(.vertical, 18)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      Group {
        if keyboard { shortcuts } else { preferences }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      HStack(spacing: 12) {
        Text(hint).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        Spacer()
        if store.captureRow >= 0 {
          SettingsButton(title: "Cancel change") { store.cancelCapture() }
        }
        SettingsButton(title: "Done") { store.close() }
      }
      .padding(.horizontal, 24).padding(.vertical, 12)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(MVTheme.canvas)
    .preferredColorScheme(.dark)
    .onChange(of: keyboard) { _, _ in store.cancelCapture() }
  }
}
