// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 (docs/design/10: "SwiftUI adjust pane"), grown into the full Adjust
// list (docs/plans/edit-adjust.md slice 1): Light, Colour, Black & White and
// Vignette sections, each collapsible with its own reset and (but the
// vignette) its own Auto; the histogram and the clipping readout. The twin of
// the Windows pane (IslandHost.Adjust.cs): same sections, sliders, ranges,
// histogram bins (edit::pack_histogram), same "Preparing" rule -- the sliders
// stay disabled until the FP16 working image exists, which for a RAW is
// LibRaw's full linear develop, never the embedded preview (docs/design/07).
//
// Keyboard-complete without Full Keyboard Access: ↑ ↓ pick a slider of an open
// section, ← → step it (⇧ for ten steps), 0 sets it back to zero, A runs its
// section's Auto, R resets every slider, Esc returns the keyboard to the
// photo. ⇧A closes the pane.
import MVChromeBridge
import SwiftUI

struct AdjustView: View {
  @ObservedObject private var store = AdjustStore.shared
  @State private var row = 0
  @State private var open: Set<Int> = [0, 1]
  @FocusState private var focused: Bool

  private var status: String {
    switch store.readiness {
    case 0: return "Open a photo to adjust it."
    case 1: return "Preparing the full-resolution image…"
    case 3: return "This file could not be prepared for editing."
    default: return store.fromRaw ? "Editing the RAW's linear data." : ""
    }
  }

  /// The sliders of the open sections, top to bottom: what ↑ ↓ walk.
  private var rows: [Int] {
    AdjustStore.sections.indices.filter { open.contains($0) }.flatMap { AdjustStore.sections[$0].params }
  }

  private var current: Int? {
    let r = rows
    return r.isEmpty ? nil : r[min(row, r.count - 1)]
  }

  private func step(_ param: Int, _ steps: Double) {
    let spec = AdjustStore.specs[param]
    store.set(param, store.values[param] + steps * spec.step)
  }

  private func sectionOf(_ param: Int) -> Int? {
    AdjustStore.sections.firstIndex { $0.params.contains(param) }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
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
      ScrollView {
        VStack(alignment: .leading, spacing: MVTheme.space(4)) {
          ForEach(AdjustStore.sections.indices, id: \.self) { i in
            sectionView(i)
          }
        }
      }
      Button("Reset adjustments") { store.reset() }
        .disabled(!store.ready)
      Text("↑ ↓ choose   ← → change (⇧ ×10)   0 zero   A auto   R reset   Esc photo")
        .font(.caption)
        .foregroundStyle(.secondary)
    }
    .padding(MVTheme.space(14))
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .leading) { Rectangle().fill(Color.primary.opacity(0.15)).frame(width: 1) }
    .focusable()
    .focused($focused)
    .onKeyPress(.upArrow) {
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + n - 1) % n }
      return .handled
    }
    .onKeyPress(.downArrow) {
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + 1) % n }
      return .handled
    }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      guard let p = current else { return .ignored }
      let sign: Double = press.key == .leftArrow ? -1 : 1
      step(p, sign * (press.modifiers.contains(.shift) ? 10 : 1))
      return .handled
    }
    .onKeyPress("0") {
      if let p = current { store.set(p, 0) }
      return .handled
    }
    .onKeyPress("a") {
      if let p = current, let sec = sectionOf(p), AdjustStore.sections[sec].hasAuto { store.auto(sec) }
      return .handled
    }
    .onKeyPress("r") { store.reset(); return .handled }
    .onKeyPress(.escape) { store.blur(); return .handled }
    .onAppear { focused = store.visible }
    .onChange(of: store.visible) { _, nowVisible in if nowVisible { focused = true } }
  }

  // A section's header (chevron, icon, name, reset, Auto; Black & White's on
  // switch) and, when open, its sliders.
  private func sectionView(_ i: Int) -> some View {
    let section = AdjustStore.sections[i]
    let isOpen = open.contains(i)
    let isMono = i == 2
    return VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      HStack(spacing: MVTheme.space(6)) {
        Button {
          if isOpen { open.remove(i) } else { open.insert(i) }
        } label: {
          HStack(spacing: MVTheme.space(6)) {
            Image(systemName: "chevron.right")
              .rotationEffect(.degrees(isOpen ? 90 : 0))
              .frame(width: 12)
            Image(systemName: section.icon)
            Text(section.name).font(.title3)
          }
          .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(section.name), \(isOpen ? "open" : "closed")")
        Spacer()
        if store.changed(i) {
          Button { store.reset(section: i) } label: { Image(systemName: "arrow.uturn.backward") }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .disabled(!store.ready)
            .help("Reset \(section.name)")
        }
        if section.hasAuto {
          if store.autoSection == i {
            ProgressView().controlSize(.small)
          } else {
            Button { store.auto(i) } label: {
              Text("AUTO").font(.caption2.weight(.semibold))
                .padding(.horizontal, MVTheme.space(5))
                .padding(.vertical, MVTheme.space(1))
                .overlay(Capsule().stroke(Color.secondary, lineWidth: 1))
            }
            .buttonStyle(.plain)
            .disabled(!store.ready || store.autoSection >= 0)
            .help("Auto \(section.name)")
          }
        }
        if isMono {
          Toggle("", isOn: Binding(
            get: { store.values[AdjustStore.monoParam] > 0 },
            set: { store.set(AdjustStore.monoParam, $0 ? 100 : 0) }))
            .toggleStyle(.switch)
            .controlSize(.mini)
            .labelsHidden()
            .disabled(!store.ready)
            .accessibilityLabel("Black and white")
        }
      }
      .padding(.vertical, MVTheme.space(4))
      if isOpen {
        ForEach(section.params, id: \.self) { p in
          sliderRow(p)
        }
      }
    }
  }

  private func sliderRow(_ p: Int) -> some View {
    let spec = AdjustStore.specs[p]
    let binding = Binding<Double>(
      get: { store.values[p] },
      set: { store.set(p, $0) })
    let selected = focused && current == p
    return VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      HStack {
        Text(spec.name).foregroundStyle(selected ? .primary : .secondary)
        Spacer()
        Text(spec.format(store.values[p])).monospacedDigit().foregroundStyle(.secondary)
      }
      Slider(value: binding, in: spec.min...spec.max, step: spec.step)
        .disabled(!store.ready)
        .onTapGesture(count: 2) { store.set(p, 0) }
        .accessibilityLabel(spec.name)
    }
    .padding(.vertical, MVTheme.space(2))
    .padding(.horizontal, MVTheme.space(4))
    .padding(.leading, MVTheme.space(14))
    .background(selected ? Color.primary.opacity(0.08) : .clear,
                in: RoundedRectangle(cornerRadius: MVTheme.radius(6)))
  }

  // R, G, B and luma as four translucent filled paths, the Windows pane's drawing.
  private var histogramView: some View {
    Canvas { ctx, size in
      ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(nsColor: .textBackgroundColor)))
      guard store.histogramValid else { return }
      let colours: [Color] = [
        Color(red: 0.92, green: 0.31, blue: 0.31).opacity(0.45),
        Color(red: 0.35, green: 0.82, blue: 0.43).opacity(0.45),
        Color(red: 0.35, green: 0.55, blue: 0.94).opacity(0.45),
        Color.primary.opacity(0.6),
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
    .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(4)))
  }
}
