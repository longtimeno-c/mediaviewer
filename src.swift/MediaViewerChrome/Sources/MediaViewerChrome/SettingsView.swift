// SPDX-License-Identifier: GPL-2.0-or-later
// The Settings screen (plan/16 "Settings"), laid out like the Windows one
// (IslandHost.Settings.cs): view preferences on the left, the remappable key
// table on the right, a footer with Close. Native owns every value -- this view
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

  func beginCapture(_ row: Int32) { mv_chrome_key_capture_begin(row); generation = .max; poll() }
  func cancelCapture() { mv_chrome_key_capture_cancel(); generation = .max; poll() }
  func resetKeys() { mv_chrome_keys_reset(); generation = .max; poll() }
  func close() { mv_chrome_menu(18) }
}

private struct SettingsToggle: View {
  let title: String
  let bit: Int32
  @ObservedObject var store: SettingsStore
  var body: some View {
    Toggle(isOn: Binding(
      get: { store.flags & bit != 0 },
      set: { store.setFlag(bit, $0) })
    ) {
      Text(title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
    }
    .toggleStyle(.switch)
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
      .padding(.horizontal, 12).padding(.vertical, 5)
      .background(RoundedRectangle(cornerRadius: 4)
        .fill(Color.white.opacity(configuration.isPressed ? 0.16 : 0.08)))
      .contentShape(Rectangle())
  }
}

struct SettingsView: View {
  @ObservedObject private var store = SettingsStore.shared
  @State private var filter = ""

  private var shown: [KeyRow] {
    let q = filter.trimmingCharacters(in: .whitespaces).lowercased()
    if q.isEmpty { return store.rows }
    return store.rows.filter { $0.name.lowercased().contains(q) || $0.keys.lowercased().contains(q) }
  }

  private var hint: String {
    store.captureRow >= 0
      ? "Press the new shortcut, or Esc to cancel."
      : "Choose a shortcut, then press its replacement. Esc cancels. Conflicts swap shortcuts."
  }

  var body: some View {
    VStack(spacing: 0) {
      HStack(alignment: .top, spacing: 0) {
        // View
        VStack(alignment: .leading, spacing: 12) {
          Text("View").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
          SettingsToggle(title: "Filmstrip when opening a folder", bit: SettingsStore.filmstripFolder, store: store)
          SettingsToggle(title: "Filmstrip when opening an image", bit: SettingsStore.filmstripImage, store: store)
          SettingsToggle(title: "Wrap at the end of the folder", bit: SettingsStore.wrap, store: store)
          SettingsToggle(title: "Sticky zoom (keep pan and zoom on next)", bit: SettingsStore.stickyZoom, store: store)
          Text("Canvas background").font(MVTheme.font()).foregroundStyle(MVTheme.body)
          Picker("", selection: Binding(get: { store.background }, set: { store.background = $0 })) {
            Text("Dark").tag(0)
            Text("Gray").tag(1)
            Text("White").tag(2)
            Text("Checkerboard").tag(3)
          }
          .labelsHidden()
          .pickerStyle(.menu)
          .frame(width: 200)
          Spacer()
        }
        .padding(EdgeInsets(top: 16, leading: 20, bottom: 16, trailing: 20))
        .frame(width: 360, alignment: .leading)

        Rectangle().fill(MVTheme.hairline).frame(width: 1)

        // Keys
        VStack(alignment: .leading, spacing: 8) {
          HStack {
            Text("Keyboard shortcuts").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
            Spacer()
            SettingsButton(title: "Reset to default") { store.resetKeys() }
          }
          TextField("Search commands or keys", text: $filter)
            .textFieldStyle(.roundedBorder)
            .font(MVTheme.font())
            .onExitCommand { store.close() }
          ScrollView {
            LazyVStack(spacing: 0) {
              ForEach(shown) { row in
                HStack {
                  Text(row.name).font(MVTheme.font()).foregroundStyle(MVTheme.title)
                  Spacer()
                  Button(store.captureRow == row.row ? "Press shortcut…"
                         : (row.keys.isEmpty ? "Unbound" : SettingsStore.macLabel(row.keys))) {
                    store.beginCapture(row.row)
                  }
                  .buttonStyle(SettingsButtonStyle())
                }
                .padding(.vertical, 3)
                Rectangle().fill(MVTheme.hairline.opacity(0.5)).frame(height: 1)
              }
              if shown.isEmpty {
                Text("No matching shortcuts").font(MVTheme.font()).foregroundStyle(MVTheme.body)
                  .padding(.top, 12)
              }
            }
          }
        }
        .padding(EdgeInsets(top: 16, leading: 20, bottom: 12, trailing: 20))
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      HStack(spacing: 12) {
        SettingsButton(title: "Close  Esc") { store.close() }
        Text(hint).font(MVTheme.font()).foregroundStyle(MVTheme.body)
        Spacer()
        if store.captureRow >= 0 {
          SettingsButton(title: "Cancel change") { store.cancelCapture() }
        }
      }
      .padding(.horizontal, 20).padding(.vertical, 10)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(MVTheme.canvas)
    .preferredColorScheme(.dark)
  }
}
