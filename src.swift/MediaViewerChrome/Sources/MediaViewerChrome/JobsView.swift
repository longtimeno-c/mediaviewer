// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 13 / 14 (docs/design/08: "All jobs go to a queue panel: progress, ETA, speed,
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
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  /// docs/design/20 "Craft rules": a state change fades quickly; Reduce Motion cuts.
  private var quick: Animation? { reduceMotion ? nil : .easeOut(duration: 0.15) }

  private func current() -> ClipJobRow? {
    store.rows.indices.contains(cursor) ? store.rows[cursor] : nil
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
      HStack {
        Text("Jobs").font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Spacer()
        Button { store.close() } label: {
          Image(systemName: "xmark").font(.system(size: 12, weight: .medium)).foregroundStyle(MVTheme.body)
        }
        .buttonStyle(FlatButtonStyle(compact: true))
        // The glyph's ink on the pane's inset, as the title's is.
        .padding(.trailing, -MVTheme.space(8))
        .help("Close (⌘J)")
        .accessibilityLabel("Close jobs")
      }
      // The list, then what acts on it, from the top: no band of empty pane
      // between the last job and Clear finished.
      ScrollView {
        VStack(alignment: .leading, spacing: MVTheme.space(8)) {
          if store.rows.isEmpty {
            Text("No clip jobs. On a clip: ⌘T trims, ⌘S opens the clip tools.")
              .foregroundStyle(MVTheme.body)
              .fixedSize(horizontal: false, vertical: true)
              .transition(.opacity)
          }
          ForEach(Array(store.rows.enumerated()), id: \.element.id) { i, row in
            jobRow(row, cursor: i == cursor && focused)
              .onTapGesture { cursor = i }
              .transition(.opacity.combined(with: .offset(y: -4)))
          }
          Button("Clear finished") { store.clearFinished() }
            .buttonStyle(FlatButtonStyle(compact: true))
            // Its ink on the list's edge, not its wash's.
            .padding(.leading, -MVTheme.space(8))
            .disabled(!store.rows.contains { !$0.running })
          Text("↑ ↓ choose · Delete cancel · R retry · Return show in Finder · Esc photo")
            .font(MVTheme.font(12))
            .foregroundStyle(MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .animation(quick, value: store.rows.map(\.id))
      }
    }
    .padding(MVTheme.space(14))
    .font(MVTheme.font(14))  // the rows and buttons follow a theme's font
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    // The panes' one ground (MetadataView, Windows' PanelShell), not a material.
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
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

  /// One surface per job: a fill, no outline at rest. The accent outline is the
  /// keyboard's cursor and shows only while the pane has the keyboard.
  private func jobRow(_ row: ClipJobRow, cursor: Bool) -> some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      Text("\(row.title) — \(row.source)").foregroundStyle(MVTheme.title).lineLimit(2)
      // The one progress track (ProgressLine.swift). A finished job keeps the
      // track's slot with the bar faded out, so the row does not jump.
      ProgressTrack(fraction: row.running ? (row.state == 2 && row.fraction <= 0 ? nil : row.fraction) : 1,
                    faded: !row.running)
      // The one line: what the job is doing, and the action its state has at
      // its end (Cancel, Retry or Show in Finder), as ProgressLine's.
      HStack(spacing: MVTheme.space(8)) {
        Text(row.status)
          .font(MVTheme.font(12).monospacedDigit())
          .foregroundStyle(MVTheme.body)
          .lineLimit(1).truncationMode(.tail)
          .contentTransition(.opacity)
        Spacer(minLength: 0)
        if row.running {
          QuietButton(title: "Cancel") { store.cancel(row.id) }.transition(.opacity)
        } else if row.state == 4 || row.state == 5 {
          QuietButton(title: "Retry") { store.retry(row.id) }.transition(.opacity)
        } else if row.state == 3 {
          QuietButton(title: "Show in Finder") { store.reveal(row.id) }.transition(.opacity)
        }
      }
      .frame(minHeight: 22)
      .animation(quick, value: row.state)
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface2))
    .overlay(
      RoundedRectangle(cornerRadius: MVTheme.radius(8))
        .strokeBorder(MVTheme.accent, lineWidth: 1)
        .opacity(cursor ? 1 : 0)
    )
    .animation(quick, value: cursor)
  }
}
