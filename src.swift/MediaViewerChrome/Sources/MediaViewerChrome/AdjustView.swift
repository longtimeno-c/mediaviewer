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
  @State private var levelsChannel = 0
  @State private var band = 0
  @State private var request = ""
  @State private var redEyeSize = 0.0
  @State private var curvesChannel = 0
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
    AdjustStore.sections.indices.filter { open.contains($0) }.flatMap { visibleParams($0) }
  }

  /// What a section shows: Levels only the picked channel's three, Curves none.
  private func visibleParams(_ i: Int) -> [Int] {
    if i == AdjustStore.levelsSection { return AdjustStore.levelsParams(levelsChannel) }
    if i == AdjustStore.selectiveSection { return AdjustStore.selectiveParams(band) }
    return AdjustStore.sections[i].params
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
      // Slice 5b: say what you want; the sliders it names move (one undo step).
      TextField("Describe a change — a bit warmer, less noise…", text: $request)
        .textFieldStyle(.roundedBorder)
        .disabled(!store.ready)
        .onSubmit {
          store.prompt(request)
          request = ""
        }
        .accessibilityLabel("Describe a change")
      if !store.promptNote.isEmpty {
        Text(store.promptNote).font(.caption).foregroundStyle(.secondary).lineLimit(2)
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
        sectionExtras(i)
        ForEach(visibleParams(i), id: \.self) { p in
          sliderRow(p)
        }
      }
    }
  }

  // What a section shows above its sliders: channel and band pickers, the
  // curve editor, the white-balance picker and the red-eye tool.
  @ViewBuilder
  private func sectionExtras(_ i: Int) -> some View {
    if i == AdjustStore.levelsSection || i == AdjustStore.curvesSection {
      Picker("Channel", selection: i == AdjustStore.levelsSection ? $levelsChannel : $curvesChannel) {
        ForEach(0..<4, id: \.self) { ch in Text(AdjustStore.channelNames[ch]).tag(ch) }
      }
      .pickerStyle(.segmented)
      .labelsHidden()
      .padding(.leading, MVTheme.space(14))
    }
    if i == 1 {
      Button {
        store.pick(store.picking == 1 ? 0 : 1)
      } label: {
        Label(store.picking == 1 ? "Click something white or grey…" : "Pick white balance",
              systemImage: "eyedropper")
      }
      .buttonStyle(.plain)
      .foregroundStyle(store.picking == 1 ? Color.accentColor : .secondary)
      .disabled(!store.ready)
      .padding(.leading, MVTheme.space(14))
      .help("Click a neutral part of the photo to set temperature and tint")
    }
    if i == AdjustStore.selectiveSection {
      HStack(spacing: MVTheme.space(6)) {
        ForEach(0..<8, id: \.self) { b in
          Button { band = b } label: {
            Circle().fill(AdjustStore.bandColours[b]).frame(width: 18, height: 18)
              .overlay(Circle().stroke(Color.primary, lineWidth: band == b ? 2 : 0))
          }
          .buttonStyle(.plain)
          .accessibilityLabel(AdjustStore.bandNames[b])
          .help(AdjustStore.bandNames[b])
        }
      }
      .padding(.leading, MVTheme.space(14))
    }
    if i == AdjustStore.redEyeSection {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Button {
          store.pick(store.picking == 2 ? 0 : 2, size: redEyeSize)
        } label: {
          Label(store.picking == 2 ? "Click each red eye — click here when done" : "Fix red eyes",
                systemImage: "eye")
        }
        .buttonStyle(.plain)
        .foregroundStyle(store.picking == 2 ? Color.accentColor : .secondary)
        .disabled(!store.ready)
        HStack {
          Text("Size").foregroundStyle(.secondary)
          Slider(value: $redEyeSize, in: -100...100, step: 5)
            .onChange(of: redEyeSize) { _, v in if store.picking == 2 { store.pick(2, size: v) } }
        }
        if store.spotCount > 0 {
          Text("\(store.spotCount) fixed").font(.caption).foregroundStyle(.secondary)
        }
      }
      .padding(.leading, MVTheme.space(14))
    }
    if i == AdjustStore.curvesSection {
      CurveEditor(store: store, channel: curvesChannel)
        .aspectRatio(1, contentMode: .fit)
        .padding(.leading, MVTheme.space(14))
        .disabled(!store.ready)
      Text("Drag the line to add a point · drag a point off the square to remove it")
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.leading, MVTheme.space(14))
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

/// Slice 2: one channel's curve, drawn over the luma histogram. A drag that
/// starts near a point moves it (the ends move up and down only); one that
/// starts on the line adds a point there; a point dragged off the square goes.
/// Every move is sent (AdjustStore.setCurve); the core makes one drag one op.
struct CurveEditor: View {
  @ObservedObject var store: AdjustStore
  let channel: Int
  @State private var dragging: Int? = nil

  private var points: [CGPoint] {
    let p = store.curves[channel]
    return p.count >= 2 ? p : [CGPoint(x: 0, y: 0), CGPoint(x: 1, y: 1)]
  }

  private static let tints: [Color] = [.primary, .red, .green, .blue]

  var body: some View {
    GeometryReader { geo in
      let size = geo.size
      Canvas { ctx, sz in
        ctx.fill(Path(CGRect(origin: .zero, size: sz)), with: .color(Color(nsColor: .textBackgroundColor)))
        if store.histogramValid {
          let bins = store.histogram[channel == 0 ? 3 : channel - 1]
          var h = Path()
          h.move(to: CGPoint(x: 0, y: sz.height))
          for (i, v) in bins.enumerated() {
            h.addLine(to: CGPoint(x: sz.width * (Double(i) + 0.5) / Double(bins.count), y: sz.height * (1 - v)))
          }
          h.addLine(to: CGPoint(x: sz.width, y: sz.height))
          ctx.fill(h, with: .color(Color.secondary.opacity(0.25)))
        }
        var grid = Path()
        for k in 1..<4 {
          let t = Double(k) / 4
          grid.move(to: CGPoint(x: sz.width * t, y: 0)); grid.addLine(to: CGPoint(x: sz.width * t, y: sz.height))
          grid.move(to: CGPoint(x: 0, y: sz.height * t)); grid.addLine(to: CGPoint(x: sz.width, y: sz.height * t))
        }
        ctx.stroke(grid, with: .color(Color.secondary.opacity(0.2)), lineWidth: 0.5)
        var line = Path()
        let pts = points
        for s in 0...96 {
          let x = Double(s) / 96
          let pt = CGPoint(x: x * sz.width, y: (1 - AdjustStore.evaluate(pts, x)) * sz.height)
          if s == 0 { line.move(to: pt) } else { line.addLine(to: pt) }
        }
        ctx.stroke(line, with: .color(Self.tints[channel]), lineWidth: 1.5)
        for p in pts {
          let c = CGPoint(x: p.x * sz.width, y: (1 - p.y) * sz.height)
          ctx.fill(Path(ellipseIn: CGRect(x: c.x - 4, y: c.y - 4, width: 8, height: 8)),
                   with: .color(Self.tints[channel]))
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(4)))
      .contentShape(Rectangle())
      .gesture(DragGesture(minimumDistance: 0)
        .onChanged { g in drag(g.startLocation, g.location, size) }
        .onEnded { _ in dragging = nil })
      .accessibilityLabel("\(AdjustStore.channelNames[channel]) curve")
    }
  }

  private func drag(_ start: CGPoint, _ at: CGPoint, _ size: CGSize) {
    guard size.width > 0, size.height > 0 else { return }
    var pts = points
    func unit(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x / size.width, y: 1 - p.y / size.height) }
    if dragging == nil {
      let s = unit(start)
      let near = pts.indices.min { hypot(pts[$0].x - s.x, pts[$0].y - s.y) < hypot(pts[$1].x - s.x, pts[$1].y - s.y) }
      if let n = near, hypot(pts[n].x - s.x, pts[n].y - s.y) < 0.06 {
        dragging = n
      } else if pts.count < 8, s.x > 0.02, s.x < 0.98 {
        pts.append(CGPoint(x: s.x, y: AdjustStore.evaluate(pts, s.x)))
        pts.sort { $0.x < $1.x }
        dragging = pts.firstIndex { $0.x == s.x }
      } else {
        return
      }
    }
    guard let d = dragging, d < pts.count else { return }
    let u = unit(at)
    let end = d == 0 || d == pts.count - 1
    // Off the square: an inner point goes.
    if !end && (u.y < -0.08 || u.y > 1.08 || u.x < -0.08 || u.x > 1.08) {
      pts.remove(at: d)
      dragging = nil
      store.setCurve(channel, pts)
      return
    }
    let lo = d > 0 ? pts[d - 1].x + 1.0 / 64 : 0
    let hi = d < pts.count - 1 ? pts[d + 1].x - 1.0 / 64 : 1
    let x = end ? pts[d].x : min(max(u.x, lo), hi)
    pts[d] = CGPoint(x: x, y: min(max(u.y, 0), 1))
    store.setCurve(channel, pts)
  }
}
