// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 14 clip tools sheet (⌘S on a clip; docs/design/10: "SwiftUI job panel entries").
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
  /// docs/design/20 "Craft rules": the chosen row is a lifted surface that
  /// slides as ↑ ↓ move it, not a ▶ typed in front of its name.
  @Namespace private var cursorSpace
  @State private var shown = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private var quick: Animation? { reduceMotion ? nil : .easeOut(duration: 0.15) }

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
        .opacity(shown ? 1 : 0)
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Text("Clip tools").font(MVTheme.font(15).weight(.semibold)).foregroundStyle(MVTheme.title)
        Text("New files beside the clip; the original is never changed.")
          .font(MVTheme.font(13))
          .foregroundStyle(MVTheme.body)
          .padding(.bottom, MVTheme.space(6))
        VStack(alignment: .leading, spacing: MVTheme.space(2)) {
          ForEach(0..<Self.tools.count, id: \.self) { i in
            Button { run(i) } label: {
              Text(Self.tools[i].name)
                .foregroundStyle(!enabled(i) ? MVTheme.disabled : i == row ? MVTheme.title : MVTheme.body)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, MVTheme.space(8))
                .padding(.vertical, MVTheme.space(3))
                .background {
                  if i == row {
                    RoundedRectangle(cornerRadius: MVTheme.radius(4)).fill(Color.primary.opacity(0.11))
                      .matchedGeometryEffect(id: "cursor", in: cursorSpace)
                  }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!enabled(i))
            .accessibilityAddTraits(i == row ? [.isSelected] : [])
          }
        }
        .font(MVTheme.font(14))
        .animation(quick, value: row)
        // The rows' words on the title's edge; their lifted surface reaches past it.
        .padding(.horizontal, -MVTheme.space(8))
        Text("↑ ↓ choose   Return run   Esc cancel   ·   jobs: ⌘J")
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
          .padding(.top, MVTheme.space(6))
        HStack {
          Spacer()
          Button("Cancel") { mv_chrome_clip_tool_cancel() }
            .keyboardShortcut(.cancelAction)
          Button("Run") { run(row) }
            .keyboardShortcut(.defaultAction)
        }
      }
      .padding(MVTheme.space(20))
      .frame(width: 440)
      // An opaque sheet over the scrim (nothing translucent over something
      // translucent): the canvas with a hairline, as Windows' sheet presenter.
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(12)).fill(MVTheme.canvas))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(12)).strokeBorder(MVTheme.hairline, lineWidth: 1))
      .opacity(shown ? 1 : 0)
      .offset(y: shown ? 0 : 8)
      .focusable()
      .focused($focused)
      .onKeyPress(.upArrow) { move(-1); return .handled }
      .onKeyPress(.downArrow) { move(1); return .handled }
      .onAppear {
        if !enabled(row) { move(1) }
        focused = true
        // The sheet arrives (a short fade and a few points of rise), it
        // does not pop; Reduce Motion shows it at once.
        withAnimation(reduceMotion ? nil : .easeOut(duration: 0.18)) { shown = true }
      }
    }
  }
}
