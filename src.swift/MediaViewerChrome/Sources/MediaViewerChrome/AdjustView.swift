// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 (plan/10: "SwiftUI adjust pane"): exposure, contrast, saturation,
// temperature and tint, the histogram and the clipping readout. The twin of
// the Windows pane (IslandHost.Adjust.cs): same sliders, same ranges, same
// histogram bins (edit::pack_histogram), same "Preparing" rule -- the sliders
// stay disabled until the FP16 working image exists, which for a RAW is
// LibRaw's full linear develop, never the embedded preview (plan/07).
//
// Keyboard-complete without Full Keyboard Access: ↑ ↓ pick a slider, ← →
// step it (⇧ for ten steps), 0 sets it back to zero, R resets every slider,
// Esc returns the keyboard to the photo. ⇧A closes the pane.
import MVChromeBridge
import SwiftUI

struct AdjustView: View {
  @ObservedObject private var store = AdjustStore.shared
  @State private var row = 0
  @FocusState private var focused: Bool

  private var status: String {
    switch store.readiness {
    case 0: return "Open a photo to adjust it."
    case 1: return "Preparing the full-resolution image…"
    case 3: return "This file could not be prepared for editing."
    default: return store.fromRaw ? "Editing the RAW's linear data." : ""
    }
  }

  private func step(_ index: Int, _ steps: Double) {
    let spec = AdjustStore.specs[index]
    store.set(index, store.values[index] + steps * spec.step)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Text("Adjust").font(.headline)
        Spacer()
        if store.readiness == 1 { ProgressView().controlSize(.small) }
        Button { store.close() } label: { Image(systemName: "xmark") }
          .buttonStyle(.plain)
          .help("Close (⇧A)")
      }
      histogramView
        .frame(height: 96)
      if store.histogramValid {
        Text(String(format: "Clipped highlights %.1f %%   ·   crushed shadows %.1f %%",
                    store.clipHigh * 100, store.clipLow * 100))
          .font(.caption)
          .foregroundStyle(.secondary)
      }
      if !status.isEmpty {
        Text(status).foregroundStyle(.secondary)
      }
      ForEach(0..<AdjustStore.specs.count, id: \.self) { i in
        sliderRow(i)
      }
      Button("Reset colour") { store.reset() }
        .disabled(!store.ready)
      Text("↑ ↓ choose   ← → change (⇧ ×10)   0 zero   R reset   Esc photo")
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
    .onKeyPress(.upArrow) { row = (row + AdjustStore.specs.count - 1) % AdjustStore.specs.count; return .handled }
    .onKeyPress(.downArrow) { row = (row + 1) % AdjustStore.specs.count; return .handled }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      let sign: Double = press.key == .leftArrow ? -1 : 1
      step(row, sign * (press.modifiers.contains(.shift) ? 10 : 1))
      return .handled
    }
    .onKeyPress("0") { store.set(row, 0); return .handled }
    .onKeyPress("r") { store.reset(); return .handled }
    .onKeyPress(.escape) { store.blur(); return .handled }
    .onAppear { focused = store.visible }
    .onChange(of: store.visible) { _, nowVisible in if nowVisible { focused = true } }
  }

  private func sliderRow(_ i: Int) -> some View {
    let spec = AdjustStore.specs[i]
    let binding = Binding<Double>(
      get: { store.values[i] },
      set: { store.set(i, $0) })
    return VStack(alignment: .leading, spacing: 2) {
      HStack {
        Text(spec.name).foregroundStyle(i == row && focused ? .primary : .secondary)
        Spacer()
        Text(spec.format(store.values[i])).monospacedDigit().foregroundStyle(.secondary)
      }
      Slider(value: binding, in: spec.min...spec.max, step: spec.step)
        .disabled(!store.ready)
        .onTapGesture(count: 2) { store.set(i, 0) }
    }
    .padding(.vertical, 2)
    .padding(.horizontal, 4)
    .background(i == row && focused ? Color.primary.opacity(0.08) : .clear,
                in: RoundedRectangle(cornerRadius: 6))
  }

  // R, G, B and luma as four translucent filled paths, the Windows pane's drawing.
  private var histogramView: some View {
    Canvas { ctx, size in
      ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color.black.opacity(0.35)))
      guard store.histogramValid else { return }
      let colours: [Color] = [
        Color(red: 0.92, green: 0.31, blue: 0.31).opacity(0.45),
        Color(red: 0.35, green: 0.82, blue: 0.43).opacity(0.45),
        Color(red: 0.35, green: 0.55, blue: 0.94).opacity(0.45),
        Color(white: 0.84).opacity(0.6),
      ]
      for c in 0..<4 {
        let bins = store.histogram[c]
        guard !bins.isEmpty else { continue }
        var path = Path()
        path.move(to: CGPoint(x: 0, y: size.height))
        for (i, v) in bins.enumerated() {
          let x = size.width * (Double(i) + 0.5) / Double(bins.count)
          path.addLine(to: CGPoint(x: x, y: size.height * (1 - v)))
        }
        path.addLine(to: CGPoint(x: size.width, y: size.height))
        path.closeSubpath()
        ctx.fill(path, with: .color(colours[c]))
      }
    }
    .clipShape(RoundedRectangle(cornerRadius: 4))
  }
}
