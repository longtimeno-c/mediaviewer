// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 14 clip tools sheet (⌘S on a clip; plan/10: "SwiftUI job panel entries").
// The twin of the Windows flyout (IslandHost.Clip.cs BuildClipTools): the same
// entries in the same order, the same packed answer (shell/trim_state.h
// pack_clip_choice: op | option << 8). Keyboard-complete: ↑ ↓ choose, Return
// runs, Esc cancels. Entries the clip cannot do are shown dimmed.
import MVChromeBridge
import SwiftUI

struct ClipToolsView: View {
  private struct Tool {
    let name: String
    let op: Int32
    let option: Int32
    let needs: Int32  // trim_state.h kClipFlag*: 1 markers, 2 audio
  }

  // mv_clip_op: 1 trim keyframe, 2 re-encode, 3 rotate, 4 split, 5 remove,
  // 6 remux, 7 frame, 8 audio, 9 animation.
  private static let tools: [Tool] = [
    Tool(name: "Rotate 90° right (lossless)", op: 3, option: 1, needs: 0),
    Tool(name: "Rotate 90° left (lossless)", op: 3, option: 2, needs: 0),
    Tool(name: "Rotate 180° (lossless)", op: 3, option: 3, needs: 0),
    Tool(name: "Split at the playhead", op: 4, option: 0, needs: 0),
    Tool(name: "Save this frame as PNG", op: 7, option: 1, needs: 0),
    Tool(name: "Save this frame as JPEG", op: 7, option: 2, needs: 0),
    Tool(name: "Extract audio (copy, no re-encode)", op: 8, option: 1, needs: 2),
    Tool(name: "Extract audio as WAV", op: 8, option: 2, needs: 2),
    Tool(name: "Extract audio as FLAC", op: 8, option: 3, needs: 2),
    Tool(name: "Convert to MP4 (remux, no re-encode)", op: 6, option: 1, needs: 0),
    Tool(name: "Convert to MKV (remux, no re-encode)", op: 6, option: 2, needs: 0),
    Tool(name: "GIF of in–out (or the next 5 s)", op: 9, option: 1, needs: 0),
    Tool(name: "WebP of in–out (or the next 5 s)", op: 9, option: 2, needs: 0),
    Tool(name: "Trim to in–out (keyframe, instant)", op: 1, option: 0, needs: 1),
    Tool(name: "Trim to in–out (re-encode, frame-accurate, slower)", op: 2, option: 0, needs: 1),
    Tool(name: "Remove in–out", op: 5, option: 0, needs: 1),
  ]

  private let flags = mv_chrome_clip_tool_flags()
  @State private var row = 0
  @FocusState private var focused: Bool

  private func enabled(_ i: Int) -> Bool { Self.tools[i].needs & flags == Self.tools[i].needs }

  private func move(_ delta: Int) {
    for _ in 0..<Self.tools.count {
      row = (row + delta + Self.tools.count) % Self.tools.count
      if enabled(row) { break }
    }
  }

  private func run(_ i: Int) {
    guard enabled(i) else { return }
    mv_chrome_clip_tool_confirm(Self.tools[i].op | (Self.tools[i].option << 8))
  }

  var body: some View {
    ZStack {
      Color.black.opacity(0.35)
        .ignoresSafeArea()
        .onTapGesture { mv_chrome_clip_tool_cancel() }
      VStack(alignment: .leading, spacing: 4) {
        Text("Clip tools").font(.title3.bold())
        Text("New files beside the clip; the original is never changed.")
          .foregroundStyle(.secondary)
          .padding(.bottom, 6)
        ForEach(0..<Self.tools.count, id: \.self) { i in
          Button { run(i) } label: {
            Text((i == row ? "▶ " : "   ") + Self.tools[i].name)
              .foregroundStyle(!enabled(i) ? .tertiary : i == row ? .primary : .secondary)
          }
          .buttonStyle(.plain)
          .disabled(!enabled(i))
        }
        Text("↑ ↓ choose   Return run   Esc cancel   ·   jobs: ⌘J")
          .font(.caption)
          .foregroundStyle(.secondary)
          .padding(.top, 6)
        HStack {
          Spacer()
          Button("Cancel") { mv_chrome_clip_tool_cancel() }
            .keyboardShortcut(.cancelAction)
          Button("Run") { run(row) }
            .keyboardShortcut(.defaultAction)
        }
      }
      .padding(20)
      .frame(width: 440)
      .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
      .focusable()
      .focused($focused)
      .onKeyPress(.upArrow) { move(-1); return .handled }
      .onKeyPress(.downArrow) { move(1); return .handled }
      .onAppear {
        if !enabled(row) { move(1) }
        focused = true
      }
    }
  }
}
