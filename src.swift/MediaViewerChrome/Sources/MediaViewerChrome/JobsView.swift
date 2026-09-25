// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 13 / 14 (plan/08: "All jobs go to a queue panel: progress, ETA, speed,
// cancel, retry, reveal"): the twin of the Windows Jobs pane
// (IslandHost.Clip.cs). Never a modal progress dialog. Keyboard-complete
// without Full Keyboard Access: ↑ ↓ choose a job, Delete cancels it, R retries,
// Return shows the output in Finder, Esc returns to the canvas; ⌘J closes.
import MVChromeBridge
import SwiftUI

struct JobsView: View {
  @ObservedObject private var store = JobsStore.shared
  @State private var cursor = 0
  @FocusState private var focused: Bool

  private func current() -> ClipJobRow? {
    store.rows.indices.contains(cursor) ? store.rows[cursor] : nil
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Text("Jobs").font(.headline)
        Spacer()
        Button { store.close() } label: { Image(systemName: "xmark") }
          .buttonStyle(.plain)
          .help("Close (⌘J)")
      }
      if store.rows.isEmpty {
        Text("No clip jobs. On a clip: ⌘T trims, ⌘S opens the clip tools.")
          .foregroundStyle(.secondary)
      }
      ScrollView {
        VStack(alignment: .leading, spacing: 8) {
          ForEach(Array(store.rows.enumerated()), id: \.element.id) { i, row in
            jobRow(row, selected: i == cursor && focused)
              .onTapGesture { cursor = i }
          }
        }
      }
      Button("Clear finished") { store.clearFinished() }
      Text("↑ ↓ choose · Delete cancel · R retry · Return show in Finder · Esc photo")
        .font(.caption)
        .foregroundStyle(.secondary)
      Spacer(minLength: 0)
    }
    .padding(14)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .leading) { Rectangle().fill(Color.primary.opacity(0.15)).frame(width: 1) }
    .focusable()
    .focused($focused)
    .onKeyPress(.upArrow) { cursor = max(0, cursor - 1); return .handled }
    .onKeyPress(.downArrow) { cursor = min(max(store.rows.count - 1, 0), cursor + 1); return .handled }
    .onKeyPress(.delete) { if let r = current() { store.cancel(r.id) }; return .handled }
    .onKeyPress(.deleteForward) { if let r = current() { store.cancel(r.id) }; return .handled }
    .onKeyPress("r") { if let r = current() { store.retry(r.id) }; return .handled }
    .onKeyPress(.return) { if let r = current() { store.reveal(r.id) }; return .handled }
    .onKeyPress(.escape) { store.blur(); return .handled }
    .onAppear { focused = store.visible }
    .onChange(of: store.visible) { _, nowVisible in if nowVisible { focused = true } }
    .onChange(of: store.rows.count) { _, n in cursor = min(cursor, max(n - 1, 0)) }
  }

  private func jobRow(_ row: ClipJobRow, selected: Bool) -> some View {
    VStack(alignment: .leading, spacing: 4) {
      Text("\(row.title) — \(row.source)").lineLimit(2)
      if row.running {
        if row.state == 2 && row.fraction <= 0 {
          ProgressView().progressViewStyle(.linear)
        } else {
          ProgressView(value: row.fraction).progressViewStyle(.linear)
        }
      }
      Text(row.status).font(.caption).foregroundStyle(.secondary)
      HStack(spacing: 8) {
        if row.running { Button("Cancel") { store.cancel(row.id) } }
        if row.state == 4 || row.state == 5 { Button("Retry") { store.retry(row.id) } }
        if row.state == 3 { Button("Show in Finder") { store.reveal(row.id) } }
      }
      .controlSize(.small)
    }
    .padding(8)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 6).strokeBorder(
      selected ? Color.orange : Color.primary.opacity(0.15), lineWidth: 1))
  }
}
