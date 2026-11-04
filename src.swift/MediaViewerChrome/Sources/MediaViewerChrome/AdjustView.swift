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
  @State private var brushSize = 0.0
  @State private var curvesChannel = 0
  @FocusState private var focused: Bool
  /// The request field has the keyboard: the pane's own keys (0, A, R, arrows)
  /// step aside, or typing "warmer" would reset every slider on its "r".
  @FocusState private var typing: Bool

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
      // Slice 5b: say what you want; the sliders it names move (one undo step).
      // It and the header stay put; everything under them scrolls
      // (docs/plans/edit-refresh.md §2.5 "Height rule"), so nothing is cut off
      // at any width or height.
      TextField("Describe a change — a bit warmer, less noise…", text: $request)
        .textFieldStyle(.roundedBorder)
        .disabled(!store.ready)
        .focused($typing)
        .onSubmit {
          store.prompt(request)
          request = ""
        }
        .accessibilityLabel("Describe a change")
      ScrollView {
        VStack(alignment: .leading, spacing: MVTheme.space(10)) {
          if !store.promptNote.isEmpty {
            Text(store.promptNote).font(.caption).foregroundStyle(.secondary).lineLimit(2)
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
          VStack(alignment: .leading, spacing: MVTheme.space(4)) {
            // Faces and Remove live in the Edit strip's wand menu, with the
            // other Enhance features; the pane is colour and tone.
            ForEach(AdjustStore.sections.indices.filter {
              $0 != AdjustStore.facesSection && $0 != AdjustStore.removeSection
            }, id: \.self) { i in
              sectionView(i)
            }
          }
          Button("Reset adjustments") { store.reset() }
            .disabled(!store.ready)
          Text("↑ ↓ choose   ← → change (⇧ ×10)   0 zero   A auto   R reset   Esc photo")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
      }
    }
    .padding(MVTheme.space(14))
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .leading) { Rectangle().fill(Color.primary.opacity(0.15)).frame(width: 1) }
    .focusable()
    .focused($focused)
    .onKeyPress(.upArrow) {
      if typing { return .ignored }
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + n - 1) % n }
      return .handled
    }
    .onKeyPress(.downArrow) {
      if typing { return .ignored }
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + 1) % n }
      return .handled
    }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      if typing { return .ignored }
      guard let p = current else { return .ignored }
      let sign: Double = press.key == .leftArrow ? -1 : 1
      step(p, sign * (press.modifiers.contains(.shift) ? 10 : 1))
      return .handled
    }
    .onKeyPress("0") {
      if typing { return .ignored }
      if let p = current { store.set(p, 0) }
      return .handled
    }
    .onKeyPress("a") {
      if typing { return .ignored }
      if let p = current, let sec = sectionOf(p), AdjustStore.sections[sec].hasAuto { store.auto(sec) }
      return .handled
    }
    .onKeyPress("r") {
      if typing { return .ignored }
      store.reset()
      return .handled
    }
    .onKeyPress(.escape) {
      if typing {
        typing = false  // out of the field, still in the pane
        return .handled
      }
      store.blur()
      return .handled
    }
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
            // Moving the size picks the tool up: the next click on a pupil fixes it.
            .onChange(of: redEyeSize) { _, v in store.pick(2, size: v) }
        }
        if store.spotCount > 0 {
          Text("\(store.spotCount) fixed").font(.caption).foregroundStyle(.secondary)
        }
      }
      .padding(.leading, MVTheme.space(14))
    }
    if i == AdjustStore.removeSection {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Button {
          store.pick(store.picking == 3 ? 0 : 3, size: brushSize)
        } label: {
          Label(store.picking == 3 ? "Brushing — click here when done" : "Brush over what to remove",
                systemImage: "paintbrush.pointed")
        }
        .buttonStyle(.plain)
        .foregroundStyle(store.picking == 3 ? Color.accentColor : .secondary)
        .disabled(!store.ready)
        HStack {
          Text("Size").foregroundStyle(.secondary)
          Slider(value: $brushSize, in: -100...100, step: 5)
            .onChange(of: brushSize) { _, v in if store.picking == 3 { store.pick(3, size: v) } }
        }
        HStack {
          Button("Undo stroke") { store.brush(0) }.disabled(store.brushStrokes == 0)
          Button("Clear") { store.brush(1) }.disabled(store.brushStrokes == 0)
          Spacer()
          Button("Remove…") { store.brush(2) }
            .disabled(store.brushStrokes == 0)
            .help("Enhance fills in what you brushed from its surroundings and saves a new file.")
        }
        if store.brushFull {
          Text("The brush is full: remove this, then brush more.").font(.caption).foregroundStyle(.secondary)
        }
      }
      .padding(.leading, MVTheme.space(14))
    }
    if i == AdjustStore.facesSection {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Button("Open closed eyes") { store.openEyes() }
          .disabled(!store.ready)
          .help("Eyes shut? Use the same person's open eyes from a photo taken moments before or after. Saves a new file.")
        Text("Uses People in Local search to find the same person in the photos taken within 10 seconds.")
          .font(.caption)
          .foregroundStyle(.secondary)
      }
      .padding(.leading, MVTheme.space(14))
    }
    if i == AdjustStore.curvesSection {
      // A square, fully visible at every width (docs/plans/edit-refresh.md §2.6):
      // the pane's content width when compact, at most 340 from regular up
      // (shell/pane_width.h curve_side).
      CurveEditor(store: store, channel: curvesChannel)
        .aspectRatio(1, contentMode: .fit)
        .frame(maxWidth: 340, alignment: .leading)
        .padding(.leading, MVTheme.space(14))
        .disabled(!store.ready)
      Text("Drag the line to add a point · double-click or drag a point off the square to remove it")
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
  // The clipping triangles at the top corners light when shadows crush or
  // highlights clip; a click shows the blinkies (`C`) on the photo.
  private var histogramView: some View {
    histogramPlot
      .overlay(alignment: .topLeading) { clipTriangle(lit: store.histogramValid && store.clipLow > 0, left: true) }
      .overlay(alignment: .topTrailing) { clipTriangle(lit: store.histogramValid && store.clipHigh > 0, left: false) }
  }

  private func clipTriangle(lit: Bool, left: Bool) -> some View {
    Button {
      mv_chrome_run_command(ViewCommand.clipping)
    } label: {
      Canvas { ctx, size in
        var p = Path()
        if left {
          p.move(to: .zero); p.addLine(to: CGPoint(x: size.width, y: 0)); p.addLine(to: CGPoint(x: 0, y: size.height))
        } else {
          p.move(to: .zero); p.addLine(to: CGPoint(x: size.width, y: 0)); p.addLine(to: CGPoint(x: size.width, y: size.height))
        }
        p.closeSubpath()
        ctx.fill(p, with: .color(lit ? (left ? Color.blue : Color.red) : Color.secondary.opacity(0.35)))
      }
      .frame(width: 12, height: 12)
      .padding(3)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(left ? "Crushed shadows: show clipping on the photo (C)" : "Clipped highlights: show clipping on the photo (C)")
    .accessibilityLabel(left ? "Show crushed shadows" : "Show clipped highlights")
  }

  private var histogramPlot: some View {
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

/// Slice 2: one channel's curve, drawn over its channel's histogram. A drag that
/// starts near a point moves it (the ends move up and down only); one that
/// starts on the line adds a point there; a point dragged off the square, or
/// double-clicked, goes. Every move is sent (AdjustStore.setCurve); the core
/// makes one drag one op. The plot sits 6 pt inside the frame, so the end
/// points and the edge lines are whole and hittable (docs/plans/edit-refresh.md
/// §2.6); drawing and hit-testing share that one rect.
struct CurveEditor: View {
  @ObservedObject var store: AdjustStore
  let channel: Int
  @State private var dragging: Int? = nil
  @State private var lastPress: (index: Int, at: Date)? = nil

  static let inset: CGFloat = 6

  private var points: [CGPoint] {
    let p = store.curves[channel]
    return p.count >= 2 ? p : [CGPoint(x: 0, y: 0), CGPoint(x: 1, y: 1)]
  }

  private static let tints: [Color] = [.primary, .red, .green, .blue]

  static func plotRect(_ size: CGSize) -> CGRect {
    CGRect(origin: .zero, size: size).insetBy(dx: inset, dy: inset)
  }

  var body: some View {
    GeometryReader { geo in
      let size = geo.size
      Canvas { ctx, sz in
        let frame = CGRect(origin: .zero, size: sz)
        let r = Self.plotRect(sz)
        let shape = RoundedRectangle(cornerRadius: MVTheme.radius(4))
        ctx.fill(shape.path(in: frame), with: .color(Color(nsColor: .textBackgroundColor)))
        func at(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: r.minX + x * r.width, y: r.minY + (1 - y) * r.height) }
        if store.histogramValid {
          let bins = store.histogram[channel == 0 ? 3 : channel - 1]
          var h = Path()
          h.move(to: at(0, 0))
          for (i, v) in bins.enumerated() {
            h.addLine(to: at((Double(i) + 0.5) / Double(bins.count), v))
          }
          h.addLine(to: at(1, 0))
          ctx.fill(h, with: .color(Self.tints[channel].opacity(channel == 0 ? 0.18 : 0.25)))
        }
        var grid = Path()
        for k in 1..<4 {
          let t = Double(k) / 4
          grid.move(to: at(t, 0)); grid.addLine(to: at(t, 1))
          grid.move(to: at(0, t)); grid.addLine(to: at(1, t))
        }
        ctx.stroke(grid, with: .color(Color.secondary.opacity(0.2)), lineWidth: 0.5)
        ctx.stroke(Path(r), with: .color(Color.secondary.opacity(0.35)), lineWidth: 0.5)
        var line = Path()
        let pts = points
        for s in 0...96 {
          let x = Double(s) / 96
          let pt = at(x, AdjustStore.evaluate(pts, x))
          if s == 0 { line.move(to: pt) } else { line.addLine(to: pt) }
        }
        ctx.stroke(line, with: .color(Self.tints[channel]), lineWidth: 1.5)
        for p in pts {
          let c = at(p.x, p.y)
          ctx.fill(Path(ellipseIn: CGRect(x: c.x - 4, y: c.y - 4, width: 8, height: 8)),
                   with: .color(Self.tints[channel]))
        }
        // The frame is a stroke, not a clip: nothing of a point at a corner is cut.
        ctx.stroke(shape.path(in: frame.insetBy(dx: 0.5, dy: 0.5)), with: .color(MVTheme.hairline), lineWidth: 1)
      }
      .overlay(alignment: .topLeading) {
        if let d = dragging, d >= 0, d < points.count {
          let p = points[d]
          Text("\(Int((p.x * 255).rounded())) → \(Int((p.y * 255).rounded()))")
            .font(.caption.monospacedDigit())
            .padding(.horizontal, 4)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 3))
            .padding(Self.inset + 2)
            .allowsHitTesting(false)
        }
      }
      .contentShape(Rectangle())
      .gesture(DragGesture(minimumDistance: 0)
        .onChanged { g in drag(g.startLocation, g.location, size) }
        .onEnded { _ in dragging = nil })
      .accessibilityLabel("\(AdjustStore.channelNames[channel]) curve")
    }
  }

  private func drag(_ start: CGPoint, _ at: CGPoint, _ size: CGSize) {
    let r = Self.plotRect(size)
    guard r.width > 0, r.height > 0 else { return }
    var pts = points
    func unit(_ p: CGPoint) -> CGPoint { CGPoint(x: (p.x - r.minX) / r.width, y: 1 - (p.y - r.minY) / r.height) }
    if dragging == nil {
      let s = unit(start)
      let near = pts.indices.min { hypot(pts[$0].x - s.x, pts[$0].y - s.y) < hypot(pts[$1].x - s.x, pts[$1].y - s.y) }
      if let n = near, hypot(pts[n].x - s.x, pts[n].y - s.y) < 0.06 {
        // A double-click on an inner point removes it.
        let now = Date()
        if n > 0, n < pts.count - 1, let last = lastPress, last.index == n,
           now.timeIntervalSince(last.at) < NSEvent.doubleClickInterval {
          lastPress = nil
          pts.remove(at: n)
          dragging = -1  // the rest of this press moves nothing
          store.setCurve(channel, pts)
          return
        }
        lastPress = (n, now)
        dragging = n
      } else if pts.count < 8, s.x > 0.02, s.x < 0.98 {
        pts.append(CGPoint(x: s.x, y: AdjustStore.evaluate(pts, s.x)))
        pts.sort { $0.x < $1.x }
        dragging = pts.firstIndex { $0.x == s.x }
        lastPress = nil
      } else {
        return
      }
    }
    guard let d = dragging, d >= 0, d < pts.count else { return }
    let u = unit(at)
    let end = d == 0 || d == pts.count - 1
    // Off the square: an inner point goes.
    if !end && (u.y < -0.08 || u.y > 1.08 || u.x < -0.08 || u.x > 1.08) {
      pts.remove(at: d)
      dragging = -1
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
