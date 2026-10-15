// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Settings screen (docs/design/16 "Settings"), laid out like the Windows one
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
  // Bits 4-6 were the canvas background, removed by issue #408 (settings.h).
  /// Set = folders list the kind (settings.h kSettingShow*). Off by default.
  static let showAudio: Int32 = 1 << 12
  static let showDocuments: Int32 = 1 << 13
  /// #345: set = the AI badge is hidden (settings.h kSettingHideAiBadge), so the
  /// badge is on by default and in every word saved before the bit existed.
  static let hideAiBadge: Int32 = 1 << 14

  @Published private(set) var visible = false
  @Published private(set) var flags: Int32 = 0
  @Published private(set) var rows: [KeyRow] = []
  @Published private(set) var captureRow: Int32 = -1
  /// PR 9: packed sort order (bits 0-2 key, bit 3 descending), as sort_order.h.
  @Published private(set) var sort: Int32 = 0
  /// -1 without an updater (unsigned build), 0 stable, 1 preview.
  @Published private(set) var updateChannel: Int32 = -1

  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  private func poll() {
    let v = mv_chrome_settings_visible()
    if v != visible { visible = v }
    guard v else { return }
    // The menu can change the sort too, and that does not move the keys generation.
    let sortNow = mv_chrome_sort_order()
    if sortNow != sort { sort = sortNow }
    let channelNow = mv_chrome_update_channel()
    if channelNow != updateChannel { updateChannel = channelNow }
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
  /// The bar asks for about 20 labels each time it renders, so the table is
  /// parsed once per keys generation, not once per label (issue #179).
  static func keyLabel(_ name: String) -> String? {
    let g = mv_chrome_keys_generation()
    if g != labelsGeneration {
      var fresh: [String: String] = [:]
      // The first row of a name wins, as first(where:) did.
      for row in readTable() where fresh[row.name] == nil {
        fresh[row.name] = row.keys.isEmpty ? "" : macLabel(row.keys)
      }
      labels = fresh
      labelsGeneration = g
    }
    guard let label = labels[name], !label.isEmpty else { return nil }
    return label
  }
  private static var labels: [String: String] = [:]
  private static var labelsGeneration: UInt64 = .max

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


  var sortKey: Int {
    get { Int(sort & 7) }
    set { mv_chrome_set_sort_order((sort & 8) | Int32(newValue & 7)); sort = mv_chrome_sort_order() }
  }
  var sortDescending: Bool {
    get { sort & 8 != 0 }
    set { mv_chrome_set_sort_order(newValue ? sort | 8 : sort & ~8); sort = mv_chrome_sort_order() }
  }

  func setUpdateChannel(_ channel: Int32) {
    mv_chrome_set_update_channel(channel)
    updateChannel = mv_chrome_update_channel()
  }

  func beginCapture(_ row: Int32) { mv_chrome_key_capture_begin(row); generation = .max; poll() }
  func cancelCapture() { mv_chrome_key_capture_cancel(); generation = .max; poll() }
  func resetKeys() { mv_chrome_keys_reset(); generation = .max; poll() }
  /// A key layout was applied (KeyLayoutRow): re-read the table now.
  func keysChanged() { generation = .max; poll() }
  func close() { mv_chrome_menu(kMenuSettings) }
}

// A separate label column keeps every switch on the same trailing edge,
// including rows whose descriptions wrap at smaller window sizes.
private struct SettingsRow<Content: View>: View {
  let title: String
  let detail: String
  @ViewBuilder let content: () -> Content

  var body: some View {
    HStack(spacing: MVTheme.space(24)) {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Text(title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Text(detail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      content()
        .labelsHidden()
        .accessibilityLabel(title)
    }
    .padding(MVTheme.space(14))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8)).stroke(MVTheme.hairline, lineWidth: 1))
  }
}

private struct SettingsToggle: View {
  let title: String
  let detail: String
  let bit: Int32
  /// The bit means "off" (a setting that is on by default).
  var inverted = false
  @ObservedObject var store: SettingsStore
  var body: some View {
    SettingsRow(title: title, detail: detail) {
      Toggle(title, isOn: Binding(
        get: { (store.flags & bit != 0) != inverted },
        set: { store.setFlag(bit, $0 != inverted) }))
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
      .padding(.horizontal, MVTheme.space(12)).padding(.vertical, MVTheme.space(7))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .fill(Color.primary.opacity(configuration.isPressed ? 0.16 : 0.08)))
      .contentShape(Rectangle())
  }
}

struct SettingsView: View {
  @ObservedObject private var store = SettingsStore.shared
  @ObservedObject private var theme = ThemeStore.shared
  @ObservedObject private var addons = OpenAddonStore.shared
  @ObservedObject private var photos = PhotosBackupStore.shared
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
      .foregroundStyle(MVTheme.title).padding(.top, MVTheme.space(12))
  }

  private var preferences: some View {
    ScrollViewReader { scroll in
    ScrollView {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
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
        // #345 (docs/design/06 "The AI badge"): on by default (owner, 2026-10-11).
        section("Viewer")
        SettingsToggle(title: "Show a badge on AI-generated images",
                       detail: "When a file says it was made by an AI tool, mark it in the corner. Files that say nothing get no mark.",
                       bit: SettingsStore.hideAiBadge, inverted: true, store: store)
        section("File types")
        SettingsToggle(title: "Show audio files",
                       detail: "List MP3, M4A and M4P files in folders. A file you open directly still opens.",
                       bit: SettingsStore.showAudio, store: store)
        SettingsToggle(title: "Show documents",
                       detail: "List PDF and Word (DOCX) files in folders. A file you open directly still opens.",
                       bit: SettingsStore.showDocuments, store: store)
        section("Appearance")
        // docs/design/25: the chrome's colours. Default, then the built-in
        // palettes, then every theme an installed add-on contributes, chosen
        // in a sheet. The photo is never recoloured.
        SettingsRow(title: "Theme",
                    detail: theme.note.isEmpty
                      ? "Colours and a quiet ground for the window, its bars and panes, and the area around your photos and videos. Your photos are never recoloured."
                      : theme.note) {
          // Issue #394: a sheet of cards in each theme's colours (ThemePicker.swift).
          ThemePickerButton()
        }
        .id(ThemeStore.rowAnchor)
        if store.updateChannel >= 0 {
          section("Updates")
          SettingsRow(title: "Update channel",
                      detail: "Preview gets signed test builds before they become stable. Switching back to Stable keeps this version until a newer stable one is released.") {
            Picker("Update channel", selection: Binding(get: { store.updateChannel },
                                                        set: { store.setUpdateChannel($0) })) {
              Text("Stable").tag(Int32(0))
              Text("Preview").tag(Int32(1))
            }
            .pickerStyle(.menu).frame(width: 180)
          }
        }
        // docs/design/26: the Photos library's backup, once the library was added
        // (Local search -> Add Photos Library); absent otherwise.
        PhotosBackupSection()
          .id(PhotosBackupStore.anchor)
        // Import and (Milestone H, docs/design/17) Local search, as items of one
        // Add-ons section; Local search is absent on an Intel Mac.
        AddonsSection().padding(.top, MVTheme.space(20))
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.bottom, MVTheme.space(24))
      .frame(maxWidth: 800)
      .frame(maxWidth: .infinity)
    }
    // docs/design/25: a package handed to the app opens Settings; the question
    // about it is at the foot of this page, so bring it into view.
    .onChange(of: addons.offer) { _, offer in
      guard offer != nil else { return }
      keyboard = false
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(OpenAddonSheet.anchor, anchor: .center) }
      }
    }
    // docs/design/25 PR 57: the script rig shows From others (a stopped add-on's line).
    .onChange(of: addons.revealRequests) { _, _ in
      keyboard = false
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(OpenAddonStore.sectionAnchor, anchor: .top) }
      }
    }
    // docs/design/26: the welcome card's iCloud Photos row opens Settings here.
    .onChange(of: photos.revealRequests) { _, _ in
      guard photos.takeReveal() else { return }
      keyboard = false
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(PhotosBackupStore.anchor, anchor: .top) }
      }
    }
    .onAppear {
      if addons.offer != nil {
        scroll.scrollTo(OpenAddonSheet.anchor, anchor: .center)
      } else if photos.takeReveal() {
        scroll.scrollTo(PhotosBackupStore.anchor, anchor: .top)
      } else if theme.takeChosenHere() {
        // Choosing a theme rebuilds the chrome; stay at the row that did it.
        scroll.scrollTo(ThemeStore.rowAnchor, anchor: .center)
      }
    }
    }
  }

  private var shortcuts: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(12)) {
      HStack {
        Text("Keyboard shortcuts").font(MVTheme.font(18)).foregroundStyle(MVTheme.title)
        Spacer()
        SettingsButton(title: "Reset to default") { store.resetKeys() }
      }
      KeyLayoutRow()
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
            HStack(spacing: MVTheme.space(16)) {
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
            .padding(.vertical, MVTheme.space(6))
            Rectangle().fill(MVTheme.hairline.opacity(0.5)).frame(height: 1)
          }
          if shown.isEmpty {
            Text("No matching shortcuts").font(MVTheme.font()).foregroundStyle(MVTheme.body)
              .padding(.top, MVTheme.space(16))
          }
        }
      }
    }
    .padding(MVTheme.space(24))
    .frame(maxWidth: 900)
    .frame(maxWidth: .infinity)
  }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: MVTheme.space(24)) {
        Text("Settings").font(MVTheme.font(24)).foregroundStyle(MVTheme.title)
        Spacer(minLength: 0)
        Picker("Settings category", selection: $keyboard) {
          Text("General").tag(false)
          Text("Keyboard shortcuts").tag(true)
        }
        .pickerStyle(.segmented).labelsHidden().frame(maxWidth: 320)
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.vertical, MVTheme.space(18))
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      Group {
        if keyboard && addons.offer == nil { shortcuts } else { preferences }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      HStack(spacing: MVTheme.space(12)) {
        Text(hint).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        Spacer()
        if store.captureRow >= 0 {
          SettingsButton(title: "Cancel change") { store.cancelCapture() }
        }
        SettingsButton(title: "Done") { store.close() }
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.vertical, MVTheme.space(12))
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(ThemeField())
    .onChange(of: keyboard) { _, _ in store.cancelCapture() }
  }
}
