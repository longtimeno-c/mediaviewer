// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 10 export sheet (plan/10: "SwiftUI crop mode and export sheet"). The twin
// of the Windows export flyout (IslandHost.Commands.cs BuildExport): the same
// four rows, the same keys, the same packed answer (shell/edit_session.h
// pack_export). Keyboard-complete: ↑ ↓ pick a row, ← → change it, Return
// exports, Esc cancels. Clicking a value steps it.
import MVChromeBridge
import SwiftUI

struct ExportView: View {
  private static let formats = ["JPEG", "PNG"]
  private static let qualities: [Int32] = [100, 95, 92, 85, 75, 60]
  private static let sizes = ["Full size", "3840 px", "2560 px", "2048 px", "1600 px", "1080 px"]
  private static let policies = ["All metadata", "All but location (GPS)", "None"]
  private static let names = ["Format", "Quality", "Size", "Metadata"]

  @State private var format: Int
  @State private var qualityIndex: Int
  @State private var size: Int
  @State private var policy: Int
  @State private var row = 0
  @FocusState private var focused: Bool

  init() {
    let last = mv_chrome_export_last_choice()
    _format = State(initialValue: Int((last >> 7) & 1))
    _qualityIndex = State(initialValue: Self.qualities.firstIndex(of: last & 0x7F) ?? 2)
    _size = State(initialValue: min(Int((last >> 10) & 7), Self.sizes.count - 1))
    _policy = State(initialValue: min(Int((last >> 8) & 3), Self.policies.count - 1))
  }

  private func value(_ r: Int) -> String {
    switch r {
    case 0: return Self.formats[format]
    case 1: return format == 1 ? "(PNG is lossless)" : "\(Self.qualities[qualityIndex])"
    case 2: return Self.sizes[size]
    default: return Self.policies[policy]
    }
  }

  private func step(_ r: Int, _ delta: Int) {
    switch r {
    case 0: format = (format + delta + 2) % 2
    case 1: if format == 0 { qualityIndex = max(0, min(Self.qualities.count - 1, qualityIndex - delta)) }
    case 2: size = (size + delta + Self.sizes.count) % Self.sizes.count
    default: policy = (policy + delta + Self.policies.count) % Self.policies.count
    }
  }

  private func confirm() {
    let packed = Self.qualities[qualityIndex] | Int32(format << 7) | Int32(policy << 8) | Int32(size << 10)
    mv_chrome_export_confirm(packed)
  }

  var body: some View {
    ZStack {
      Color.black.opacity(0.35)
        .ignoresSafeArea()
        .onTapGesture { mv_chrome_export_cancel() }
      VStack(alignment: .leading, spacing: 10) {
        Text("Export a copy").font(.title3.bold())
        Text("Beside the original as name-edit; never overwrites.")
          .foregroundStyle(.secondary)
        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
          ForEach(0..<4, id: \.self) { r in
            GridRow {
              Text(Self.names[r]).foregroundStyle(.secondary)
              Button {
                row = r
                step(r, 1)
              } label: {
                Text(r == row ? "◀  \(value(r))  ▶" : "    \(value(r))")
                  .foregroundStyle(r == row ? .primary : .secondary)
              }
              .buttonStyle(.plain)
            }
          }
        }
        Text("↑ ↓ choose   ← → change   Return export   Esc cancel")
          .font(.caption)
          .foregroundStyle(.secondary)
        HStack {
          Spacer()
          Button("Cancel") { mv_chrome_export_cancel() }
            .keyboardShortcut(.cancelAction)
          Button("Export") { confirm() }
            .keyboardShortcut(.defaultAction)
        }
      }
      .padding(20)
      .frame(width: 380)
      .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
      .focusable()
      .focused($focused)
      .onKeyPress(.upArrow) { row = (row + 3) % 4; return .handled }
      .onKeyPress(.downArrow) { row = (row + 1) % 4; return .handled }
      .onKeyPress(.leftArrow) { step(row, -1); return .handled }
      .onKeyPress(.rightArrow) { step(row, 1); return .handled }
      .onAppear { focused = true }
    }
  }
}
