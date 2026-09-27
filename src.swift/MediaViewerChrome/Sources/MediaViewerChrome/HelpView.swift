// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `?` cheat sheet (plan/16-commands.md "`?`"): a chrome overlay over the
// canvas listing the current bindings. Generated from the live command table
// (mv_chrome_command_table: the shared table, limited to what this host runs,
// with the user's remaps), the same table Settings edits and Windows' `?`
// reads, so a key added in command_table.cpp shows up here with no edit.
import MVChromeBridge
import SwiftUI

@MainActor
final class HelpStore: ObservableObject {
  static let shared = HelpStore()

  struct Entry: Identifiable {
    let id: Int32
    let name: String
    var keys: String
  }

  @Published private(set) var entries: [Entry] = []

  /// Re-read on every open: Settings may have remapped a key since.
  func reload() {
    let need = Int(mv_chrome_command_table(nil, 0))
    guard need > 0 else { entries = []; return }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = mv_chrome_command_table(&buf, Int32(buf.count))
    var out: [Entry] = []
    var index: [Int32: Int] = [:]
    // "id\tmodes\tname\tkeys\trunnable\trow", one line per binding, in table
    // order. Bindings of one command are joined, as the Windows `?` does; an
    // unbound row (Settings can give it a key) is not listed.
    for line in String(cString: buf).split(separator: "\n") {
      let f = line.split(separator: "\t", omittingEmptySubsequences: false)
      guard f.count >= 4, let id = Int32(f[0]), !f[3].isEmpty else { continue }
      let keys = SettingsStore.macLabel(String(f[3]))
      if let at = index[id] {
        if !out[at].keys.components(separatedBy: "  /  ").contains(keys) {
          out[at].keys += "  /  " + keys
        }
      } else {
        index[id] = out.count
        out.append(Entry(id: id, name: String(f[2]), keys: keys))
      }
    }
    entries = out
  }
}

struct HelpView: View {
  @ObservedObject private var store = HelpStore.shared

  var body: some View {
    VStack(spacing: 16) {
      Text("Keyboard shortcuts").font(.title2.bold())
      ScrollView {
        LazyVGrid(
          columns: [GridItem(.adaptive(minimum: 340), spacing: 24, alignment: .top)],
          alignment: .leading, spacing: 6
        ) {
          ForEach(store.entries) { entry in
            HStack(alignment: .firstTextBaseline) {
              Text(entry.keys)
                .font(.system(.body, design: .monospaced))
                .frame(width: 150, alignment: .leading)
              Text(entry.name).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
          }
        }
        .padding(.horizontal, 32)
      }
      Text("Wheel zooms toward the cursor, drag pans. Keys change in Settings (⌘,). ? or Esc to close.")
        .font(.footnote).foregroundStyle(.secondary)
    }
    .padding(.top, 56)
    .padding(.bottom, 16)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(.regularMaterial)
    .onAppear { store.reload() }
  }
}
