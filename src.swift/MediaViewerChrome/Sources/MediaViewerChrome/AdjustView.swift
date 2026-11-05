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
// docs/plans/edit-refresh.md S2 (§2.5): the layout follows the inspector's
// width class (shell/pane_width.h class_of), read by one GeometryReader at the
// root and handed down only when the class changes, so a drag of the column's
// edge does not rebuild the sections. compact (280–339): one column, labels
// above the sliders, histogram 72. regular (340–559): one column, labels
// inline, histogram 96. wide (560–719): the histogram across, then two
// independent columns (Light, Colour, B&W, Vignette, Detail | Levels, Curves,
// Selective Colour, Red-Eye). xwide (≥ 720): wide plus a scopes row, the
// histogram and a square Curves side by side. Each section is a card with a
// chevron, a changed dot, an AUTO pill and ↺; a value readout is clickable
// to type a number, and a double-click on a slider sets it back to 0.
//
// Keyboard-complete without Full Keyboard Access: ↑ ↓ pick a slider of an open
// section (the left column, then the right), ← → step it (⇧ for ten steps),
// 0 sets it back to zero, A runs its section's Auto, R resets every slider,
// Esc returns the keyboard to the photo. ⇧A closes the pane.
import AppKit
import MVChromeBridge
import SwiftUI

/// The inspector's layout class (shell/pane_width.h width_class, class_of).
enum PaneWidthClass: Int {
  case compact, regular, wide, xwide

  init(width: CGFloat) {
    if width < 340 { self = .compact } else if width < 560 { self = .regular } else if width < 720 {
      self = .wide
    } else { self = .xwide }
  }

  var twoColumns: Bool { self == .wide || self == .xwide }
}

private struct PaneWidthClassKey: EnvironmentKey {
  static let defaultValue = PaneWidthClass.regular
}

extension EnvironmentValues {
  var paneWidthClass: PaneWidthClass {
    get { self[PaneWidthClassKey.self] }
    set { self[PaneWidthClassKey.self] = newValue }
  }
}

/// Reads the pane's width once at the root and hands its class down: the
/// content is rebuilt when the class changes, not on every point of a drag.
struct WidthClassReader<Content: View>: View {
  @ViewBuilder let content: (PaneWidthClass) -> Content
  @State private var cls = PaneWidthClass.regular

  var body: some View {
    content(cls)
      .environment(\.paneWidthClass, cls)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      .background(GeometryReader { geo in
        Color.clear
          .onAppear { cls = PaneWidthClass(width: geo.size.width) }
          .onChange(of: PaneWidthClass(width: geo.size.width)) { _, now in cls = now }
      })
  }
}

struct AdjustView: View {
  var body: some View {
    WidthClassReader { cls in AdjustPane(cls: cls) }
  }
}

private struct AdjustPane: View {
  let cls: PaneWidthClass
  @ObservedObject private var store = AdjustStore.shared
  @State private var row = 0
  @State private var open: Set<Int> = [0, 1]
  @State private var levelsChannel = 0
  @State private var band = 0
  @State private var request = ""
  @State private var curvesChannel = 0
  /// A value readout being typed into (its parameter): the pane's keys step aside.
  @State private var editingValue: Int? = nil
  @FocusState private var focused: Bool
  /// The request field has the keyboard: the pane's own keys (0, A, R, arrows)
  /// step aside, or typing "warmer" would reset every slider on its "r".
  @FocusState private var typing: Bool

  /// Faces and Remove live in the Enhance tab (EnhanceHubView); the pane is
  /// colour and tone.
  private static let leftSections = [0, 1, 2, 3, 8]
  private static let rightSections = [4, 5, 6, 7]
  private static let oneColumn = [0, 1, 2, 3, 4, 5, 6, 7, 8]

  /// The pane's own keys step aside while any text is being edited: the
  /// request field (by SwiftUI's focus, or by AppKit's when the field took
  /// the keyboard another way and `typing` never heard), or a value readout.
  /// Without the AppKit check, "warmer" typed while `typing` was false lost
  /// its r (Reset) and a (Auto) to the pane's keys (owner 2026-11-05).
  private var keysOff: Bool { typing || editingValue != nil || Self.editingText() }

  private var status: String {
    switch store.readiness {
    case 0: return "Open a photo to adjust it."
    case 1: return "Preparing the full-resolution image…"
    case 3: return "This file could not be prepared for editing."
    default: return store.fromRaw ? "Editing the RAW's linear data." : ""
    }
  }

  /// The sections in walking order: one column, or the left then the right.
  private var walkOrder: [Int] {
    cls.twoColumns ? Self.leftSections + Self.rightSections.filter { !(cls == .xwide && $0 == AdjustStore.curvesSection) }
                   : Self.oneColumn
  }

  /// The sliders of the open sections in walking order: what ↑ ↓ walk.
  private var rows: [Int] {
    walkOrder.filter { open.contains($0) }.flatMap { visibleParams($0) }
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

  private var histogramHeight: CGFloat { cls == .compact ? 72 : 96 }

  /// What Return commits: the field editor's text while it edits the field.
  /// The binding only follows edits AppKit reports as typing; a value set
  /// another way (VoiceOver, an accessibility client, dictation's last
  /// phrase) reached the field but not `request`, and Return sent "" ("the
  /// box recognises nothing", 2026-11-05).
  /// Where AppKit has the text: the field editor while it edits the field;
  /// else (Return already ended the editing, so the window is the responder)
  /// the field itself, found by its placeholder. Read on Return only.
  static func submittedText(_ bound: String) -> String {
    let live = liveText()
    if let live, !live.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return live }
    return bound
  }

  /// Empties what AppKit shows: `request = ""` alone does not when the binding
  /// was already "" (the text never reached it).
  static func clearSubmitted() {
    if let editor = NSApp.keyWindow?.firstResponder as? NSTextView, editor.isFieldEditor {
      editor.string = ""
    } else if let field = describeField() {
      field.stringValue = ""
    }
  }

  private static let placeholder = "Describe a change — a bit warmer, less noise…"

  /// A field editor has the keyboard in the key window (any text field).
  static func editingText() -> Bool {
    guard let editor = NSApp.keyWindow?.firstResponder as? NSTextView else { return false }
    return editor.isFieldEditor
  }

  /// The field editor is editing the request field itself.
  private static func editingDescribeField() -> Bool {
    guard let editor = NSApp.keyWindow?.firstResponder as? NSTextView, editor.isFieldEditor,
          let field = editor.delegate as? NSTextField else { return false }
    return field.placeholderString == placeholder || field.placeholderAttributedString?.string == placeholder
  }

  /// Return in the request field: one path whether the field's own submit
  /// or the pane's Return key hears it first. Blank does nothing (a second
  /// submit after the field was emptied must not replace the note). The
  /// host says in the note what it did, or why not now; the text stays in
  /// the field while the photo is still being prepared, so Return again runs it.
  private func submit() {
    let text = Self.submittedText(request)
    guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    let ready = store.ready
    store.prompt(text)
    guard ready else { return }
    Self.clearSubmitted()
    request = ""
  }

  private static func liveText() -> String? {
    guard let window = NSApp.keyWindow else { return nil }
    if let editor = window.firstResponder as? NSTextView, editor.isFieldEditor { return editor.string }
    return describeField()?.stringValue
  }

  private static func describeField() -> NSTextField? {
    guard let root = NSApp.keyWindow?.contentView else { return nil }
    func walk(_ v: NSView) -> NSTextField? {
      if let f = v as? NSTextField, f.isEditable,
         f.placeholderString == placeholder || f.placeholderAttributedString?.string == placeholder {
        return f
      }
      for s in v.subviews {
        if let f = walk(s) { return f }
      }
      return nil
    }
    return walk(root)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
      HStack(spacing: MVTheme.space(8)) {
        Text("Adjust").font(MVTheme.font(16)).foregroundStyle(MVTheme.title)
          .accessibilityAddTraits(.isHeader)
        if store.readiness == 1 { ProgressView().controlSize(.small) }
        Spacer()
        Button { store.close() } label: {
          Image(systemName: "xmark").font(.system(size: 11, weight: .semibold)).frame(width: 22, height: 22)
        }
        .buttonStyle(.plain)
        .foregroundStyle(MVTheme.body)
        .help(withKey("Close", "Adjust pane"))
        .accessibilityLabel("Close Adjust")
      }
      // Slice 5b: say what you want; the sliders it names move (one undo step).
      // It and the header stay put; everything under them scrolls
      // (docs/plans/edit-refresh.md §2.5 "Height rule"), so nothing is cut off
      // at any width or height.
      HStack(spacing: MVTheme.space(6)) {
        Image(systemName: "text.bubble").foregroundStyle(MVTheme.body)
        TextField(Self.placeholder, text: $request)
          .textFieldStyle(.plain)
          .font(MVTheme.font(14))
          .disabled(!store.ready)
          .focused($typing)
          .onSubmit { submit() }
          .accessibilityLabel("Describe a change")
      }
      .padding(.horizontal, MVTheme.space(10))
      .padding(.vertical, MVTheme.space(7))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface2))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8))
        .stroke(typing ? MVTheme.accent : MVTheme.hairline, lineWidth: typing ? 1.5 : 1))
      ScrollView {
        VStack(alignment: .leading, spacing: MVTheme.space(10)) {
          if !store.promptNote.isEmpty {
            Text(store.promptNote).font(MVTheme.font(12)).foregroundStyle(MVTheme.body).lineLimit(2)
          }
          if cls == .xwide {
            scopesRow
          } else {
            histogramView.frame(height: histogramHeight)
          }
          if store.histogramValid {
            Text(String(format: "Clipped highlights %.1f %%  ·  crushed shadows %.1f %%",
                        store.clipHigh * 100, store.clipLow * 100))
              .font(MVTheme.font(12))
              .foregroundStyle(MVTheme.body)
              .lineLimit(2)
          }
          if !status.isEmpty {
            Text(status).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
          }
          if cls.twoColumns {
            // Two independent stacks: a section opening on one side never
            // moves the other.
            HStack(alignment: .top, spacing: MVTheme.space(10)) {
              VStack(alignment: .leading, spacing: MVTheme.space(8)) {
                ForEach(Self.leftSections, id: \.self) { sectionView($0) }
              }
              .frame(maxWidth: .infinity, alignment: .topLeading)
              VStack(alignment: .leading, spacing: MVTheme.space(8)) {
                ForEach(Self.rightSections.filter { !(cls == .xwide && $0 == AdjustStore.curvesSection) },
                        id: \.self) { sectionView($0) }
              }
              .frame(maxWidth: .infinity, alignment: .topLeading)
            }
          } else {
            VStack(alignment: .leading, spacing: MVTheme.space(8)) {
              ForEach(Self.oneColumn, id: \.self) { sectionView($0) }
            }
          }
          HStack {
            Button("Reset adjustments") { store.reset() }
              .buttonStyle(FlatButtonStyle(compact: true))
              .disabled(!store.ready)
              .help(withKey("Every slider back to 0, one undo step", "Adjust: reset"))
            Spacer()
          }
          Text("↑ ↓ choose   ← → change (⇧ ×10)   0 zero   A auto   R reset   Esc photo")
            .font(MVTheme.font(12))
            .foregroundStyle(MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.bottom, MVTheme.space(8))
      }
    }
    .padding(MVTheme.space(14))
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .focusable()
    .focused($focused)
    // Return while the request field has the keyboard is its submit, taken
    // here as well as by onSubmit: the pane's key handlers see a key before
    // the field does, and a Return the field never reports must not be lost.
    .onKeyPress(.return) {
      guard editingValue == nil, typing || Self.editingDescribeField() else { return .ignored }
      submit()
      return .handled
    }
    .onKeyPress(.upArrow) {
      if keysOff { return .ignored }
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + n - 1) % n }
      return .handled
    }
    .onKeyPress(.downArrow) {
      if keysOff { return .ignored }
      let n = rows.count
      if n > 0 { row = (min(row, n - 1) + 1) % n }
      return .handled
    }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      if keysOff { return .ignored }
      guard let p = current else { return .ignored }
      let sign: Double = press.key == .leftArrow ? -1 : 1
      step(p, sign * (press.modifiers.contains(.shift) ? 10 : 1))
      return .handled
    }
    .onKeyPress("0") {
      if keysOff { return .ignored }
      if let p = current { store.set(p, 0) }
      return .handled
    }
    .onKeyPress("a") {
      if keysOff { return .ignored }
      if let p = current, let sec = sectionOf(p), AdjustStore.sections[sec].hasAuto { store.auto(sec) }
      return .handled
    }
    .onKeyPress("r") {
      if keysOff { return .ignored }
      store.reset()
      return .handled
    }
    .onKeyPress(.escape) {
      if editingValue != nil {
        editingValue = nil
        return .handled
      }
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

  // xwide: the histogram and a square Curves side by side, each half the width
  // (the curve's channel picker under it).
  private var scopesRow: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      Color.clear
        .aspectRatio(2, contentMode: .fit)
        .overlay {
          HStack(spacing: MVTheme.space(12)) {
            histogramView.frame(maxWidth: .infinity, maxHeight: .infinity)
            CurveEditor(store: store, channel: curvesChannel)
              .aspectRatio(1, contentMode: .fit)
              .disabled(!store.ready)
          }
        }
      HStack(spacing: MVTheme.space(8)) {
        Spacer(minLength: 0)
        channelPicker($curvesChannel)
          .frame(maxWidth: 260)
      }
    }
  }

  private func channelPicker(_ binding: Binding<Int>) -> some View {
    Picker("Channel", selection: binding) {
      ForEach(0..<4, id: \.self) { ch in Text(AdjustStore.channelNames[ch]).tag(ch) }
    }
    .pickerStyle(.segmented)
    .labelsHidden()
    .controlSize(.small)
  }

  // A section's card: its header (chevron, icon, name, the changed dot, AUTO,
  // reset; Black & White's on switch) and, when open, its sliders.
  private func sectionView(_ i: Int) -> some View {
    let section = AdjustStore.sections[i]
    let isOpen = open.contains(i)
    let isMono = i == 2
    let changed = store.changed(i)
    return VStack(alignment: .leading, spacing: MVTheme.space(4)) {
      HStack(spacing: MVTheme.space(6)) {
        SectionHeaderButton(name: section.name, icon: section.icon, open: isOpen, changed: changed) {
          withAnimation(.easeOut(duration: 0.15)) {
            if isOpen { open.remove(i) } else { open.insert(i) }
          }
        }
        Spacer(minLength: MVTheme.space(4))
        if section.hasAuto {
          if store.autoSection == i {
            ProgressView().controlSize(.small).frame(width: 38)
          } else {
            AutoPill(enabled: store.ready && store.autoSection < 0) { store.auto(i) }
              .help("Auto \(section.name): one undo step")
              .accessibilityLabel("Auto \(section.name)")
          }
        }
        if changed {
          IconButton(symbol: "arrow.uturn.backward", help: "Reset \(section.name)") { store.reset(section: i) }
            .disabled(!store.ready)
            .accessibilityLabel("Reset \(section.name)")
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
      if isOpen {
        VStack(alignment: .leading, spacing: MVTheme.space(4)) {
          sectionExtras(i)
          ForEach(visibleParams(i), id: \.self) { p in
            sliderRow(p)
          }
        }
        .padding(.top, MVTheme.space(2))
        .transition(.opacity)
      }
    }
    .padding(.horizontal, MVTheme.space(10))
    .padding(.vertical, MVTheme.space(8))
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(9)).fill(MVTheme.surface2))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(9)).stroke(MVTheme.hairline.opacity(0.6), lineWidth: 0.5))
  }

  // What a section shows above its sliders: channel and band pickers, the
  // curve editor, the white-balance picker and the red-eye tool.
  @ViewBuilder
  private func sectionExtras(_ i: Int) -> some View {
    if i == AdjustStore.levelsSection {
      channelPicker($levelsChannel)
    }
    if i == AdjustStore.curvesSection {
      channelPicker($curvesChannel)
    }
    if i == 1 {
      ToolChip(symbol: "eyedropper",
               title: store.picking == 1 ? "Click something white or grey…" : "Pick white balance",
               on: store.picking == 1) { store.pick(store.picking == 1 ? 0 : 1) }
        .disabled(!store.ready)
        .help("Click a neutral part of the photo to set temperature and tint")
    }
    if i == AdjustStore.selectiveSection {
      HStack(spacing: MVTheme.space(6)) {
        ForEach(0..<8, id: \.self) { b in
          Button { band = b } label: {
            Circle().fill(AdjustStore.bandColours[b]).frame(width: 18, height: 18)
              .overlay(Circle().stroke(MVTheme.title, lineWidth: band == b ? 2 : 0).padding(-2))
              .padding(2)
              .contentShape(Circle())
          }
          .buttonStyle(.plain)
          .accessibilityLabel(AdjustStore.bandNames[b])
          .accessibilityAddTraits(band == b ? [.isSelected] : [])
          .help(AdjustStore.bandNames[b])
        }
      }
    }
    if i == AdjustStore.redEyeSection {
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
        ToolChip(symbol: "eye",
                 title: store.picking == 2 ? "Click each red eye — click here when done" : "Fix red eyes",
                 on: store.picking == 2) { store.pick(store.picking == 2 ? 0 : 2) }
          .disabled(!store.ready)
        // docs/plans/edit-refresh.md S3: the size, Find eyes and Done are the
        // context bar's, below the photo, while the tool is armed.
        Text("Size, Find eyes and Done are in the bar below the photo")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        if store.spotCount > 0 {
          Text(store.spotCount == 1 ? "1 fixed" : "\(store.spotCount) fixed")
            .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        }
      }
    }
    if i == AdjustStore.curvesSection {
      // A square, fully visible at every width (docs/plans/edit-refresh.md §2.6):
      // the card's content width when compact, at most 340 from regular up
      // (shell/pane_width.h curve_side). At xwide it is in the scopes row.
      CurveEditor(store: store, channel: curvesChannel)
        .aspectRatio(1, contentMode: .fit)
        .frame(maxWidth: cls == .compact ? .infinity : 340, alignment: .leading)
        .disabled(!store.ready)
      Text("Drag the line to add a point · double-click or drag a point off the square to remove it")
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  /// Sliders whose track shows what they do to the picture.
  private func gradient(_ p: Int) -> [Color]? {
    switch p {
    case 0: return [Color(white: 0.08), Color(white: 0.95)]                      // exposure
    case 2, 9: return [Color(white: 0.55), MVTheme.warm, MVTheme.tintM, MVTheme.cool]  // saturation, vibrance
    case 3: return [MVTheme.cool, Color(white: 0.85), MVTheme.warm]              // temperature
    case 4: return [MVTheme.tintG, Color(white: 0.85), MVTheme.tintM]            // tint
    default:
      // Selective colour's hue: the band's neighbours either side.
      if p >= 28 && p < 52 && (p - 28) % 3 == 0 {
        let b = (p - 28) / 3
        return [AdjustStore.bandColours[(b + 7) % 8], AdjustStore.bandColours[b], AdjustStore.bandColours[(b + 1) % 8]]
      }
      return nil
    }
  }

  private func sliderRow(_ p: Int) -> some View {
    let spec = AdjustStore.specs[p]
    let selected = focused && current == p
    let inline = cls == .regular || cls == .xwide
    let name = Text(spec.name)
      .font(MVTheme.font(13))
      .foregroundStyle(selected ? MVTheme.title : MVTheme.body)
      .lineLimit(1)
    let slider = ToneSlider(value: store.values[p], spec: spec, gradient: gradient(p), enabled: store.ready) {
      store.set(p, $0)
    }
    .accessibilityLabel(spec.name)
    let readout = ValueReadout(text: spec.format(store.values[p]), value: store.values[p], enabled: store.ready,
                               param: p, editing: $editingValue) { store.set(p, $0) }
    return Group {
      if inline {
        HStack(spacing: MVTheme.space(8)) {
          name.frame(width: 92, alignment: .leading)
            .onTapGesture(count: 2) { store.set(p, 0) }
          slider
          readout
        }
      } else {
        VStack(alignment: .leading, spacing: MVTheme.space(1)) {
          HStack {
            name.onTapGesture(count: 2) { store.set(p, 0) }
            Spacer(minLength: MVTheme.space(4))
            readout
          }
          slider
        }
      }
    }
    .padding(.vertical, MVTheme.space(3))
    .padding(.horizontal, MVTheme.space(6))
    .background(selected ? MVTheme.selectionWash : .clear,
                in: RoundedRectangle(cornerRadius: MVTheme.radius(6)))
    .help("\(spec.name): double-click to set it back to 0")
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
        ctx.fill(p, with: .color(lit ? (left ? Color.blue : Color.red) : MVTheme.body.opacity(0.35)))
      }
      .frame(width: 12, height: 12)
      .padding(4)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(left ? "Crushed shadows: show clipping on the photo (C)" : "Clipped highlights: show clipping on the photo (C)")
    .accessibilityLabel(left ? "Show crushed shadows" : "Show clipped highlights")
  }

  private var histogramPlot: some View {
    HistogramPlot(store: store)
  }
}

/// The histogram: R, G, B and luma as four translucent filled paths. Its own
/// view observing the store, as CurveEditor is, with the bins read in `body`:
/// a Canvas drawn from the pane's body kept the blank it drew while the pane
/// was hidden (the working image rebuilt on reopen) after the bins came back,
/// while Curves beside it redrew (2026-11-05).
struct HistogramPlot: View {
  @ObservedObject var store: AdjustStore

  private static let colours: [Color] = [
    Color(red: 0.92, green: 0.31, blue: 0.31).opacity(0.45),
    Color(red: 0.35, green: 0.82, blue: 0.43).opacity(0.45),
    Color(red: 0.35, green: 0.55, blue: 0.94).opacity(0.45),
    Color.primary.opacity(0.6),
  ]

  var body: some View {
    let bins = store.histogramValid ? store.histogram : []
    Canvas { ctx, size in
      let frame = RoundedRectangle(cornerRadius: MVTheme.radius(6)).path(in: CGRect(origin: .zero, size: size))
      ctx.fill(frame, with: .color(Color(nsColor: .textBackgroundColor)))
      for (c, channel) in bins.enumerated() where c < Self.colours.count && !channel.isEmpty {
        var path = Path()
        path.move(to: CGPoint(x: 0, y: size.height))
        for (i, v) in channel.enumerated() {
          let x = size.width * (Double(i) + 0.5) / Double(channel.count)
          path.addLine(to: CGPoint(x: x, y: size.height * (1 - v)))
        }
        path.addLine(to: CGPoint(x: size.width, y: size.height))
        path.closeSubpath()
        ctx.fill(path, with: .color(Self.colours[c]))
      }
      ctx.stroke(RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .path(in: CGRect(origin: .zero, size: size).insetBy(dx: 0.5, dy: 0.5)),
                 with: .color(MVTheme.hairline), lineWidth: 1)
    }
    // The bins are the Canvas's identity: new bins, a new drawing.
    .id(store.histogramStamp)
    .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
    .accessibilityLabel("Histogram")
  }
}

// ---- the pane's controls ---------------------------------------------------------------

/// A section card's title: chevron, icon and name, one button; the changed
/// dot beside the name says a slider of it is away from 0.
private struct SectionHeaderButton: View {
  let name: String
  let icon: String
  let open: Bool
  let changed: Bool
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      HStack(spacing: MVTheme.space(7)) {
        Image(systemName: "chevron.right")
          .font(.system(size: 10, weight: .semibold))
          .rotationEffect(.degrees(open ? 90 : 0))
          .frame(width: 12)
          .foregroundStyle(MVTheme.body)
        Image(systemName: icon)
          .font(.system(size: 13))
          .frame(width: 18)
          .foregroundStyle(changed ? MVTheme.accent : MVTheme.title)
        Text(name)
          .font(MVTheme.font(15))
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
        if changed {
          Circle().fill(MVTheme.accent).frame(width: 6, height: 6)
            .accessibilityHidden(true)
        }
      }
      .padding(.vertical, MVTheme.space(3))
      .padding(.horizontal, MVTheme.space(2))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(5)).fill(hover ? MVTheme.hoverWash : .clear))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel("\(name), \(open ? "open" : "closed")\(changed ? ", changed" : "")")
    .accessibilityAddTraits(.isHeader)
  }
}

/// A section's AUTO: a small capsule that fills on hover.
private struct AutoPill: View {
  let enabled: Bool
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      Text("AUTO")
        .font(MVTheme.font(11))
        .foregroundStyle(enabled ? (hover ? MVTheme.accent : MVTheme.body) : MVTheme.disabled)
        .padding(.horizontal, MVTheme.space(7))
        .padding(.vertical, MVTheme.space(2))
        .background(Capsule().fill(hover && enabled ? MVTheme.selectionWash : .clear))
        .overlay(Capsule().stroke(hover && enabled ? MVTheme.accent : MVTheme.hairline, lineWidth: 1))
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .onHover { hover = $0 }
  }
}

/// A small icon button with a hover wash.
struct IconButton: View {
  let symbol: String
  let help: String
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      Image(systemName: symbol)
        .font(.system(size: 11, weight: .semibold))
        .frame(width: 22, height: 22)
        .foregroundStyle(enabled ? MVTheme.body : MVTheme.disabled)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(5)).fill(hover && enabled ? MVTheme.hoverWash : .clear))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .help(help)
    .accessibilityLabel(help)  // not the symbol's name
  }
}

/// A tool that a click on the photo uses (white balance, red-eye): washed in
/// the accent while it is armed.
private struct ToolChip: View {
  let symbol: String
  let title: String
  let on: Bool
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      Label(title, systemImage: symbol)
        .font(MVTheme.font(13))
        .foregroundStyle(!enabled ? MVTheme.disabled : (on ? MVTheme.accent : MVTheme.title))
        .padding(.horizontal, MVTheme.space(8))
        .padding(.vertical, MVTheme.space(4))
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
          .fill(on ? MVTheme.selectionWash : (hover && enabled ? MVTheme.hoverWash : .clear)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityAddTraits(on ? [.isSelected] : [])
  }
}

/// A slider drawn by the chrome: a 4 pt track (a gradient for the sliders that
/// show what they do, else the accent from 0 to the value), a round thumb,
/// a drag anywhere on it, a double-click back to 0, and the screen reader's
/// increment and decrement in the parameter's own step.
struct ToneSlider: View {
  let value: Double
  let spec: AdjustSpec
  let gradient: [Color]?
  let enabled: Bool
  let set: (Double) -> Void
  @State private var hover = false
  @State private var dragging = false

  private static let thumb: CGFloat = 14

  private var zero: Double { min(max(0, spec.min), spec.max) }

  private func fraction(_ v: Double) -> CGFloat {
    let span = spec.max - spec.min
    return span > 0 ? CGFloat((min(max(v, spec.min), spec.max) - spec.min) / span) : 0
  }

  var body: some View {
    GeometryReader { geo in
      let r = Self.thumb / 2
      let usable = max(geo.size.width - Self.thumb, 1)
      let t = fraction(value)
      let z = fraction(zero)
      let midY = geo.size.height / 2
      ZStack(alignment: .topLeading) {
        Group {
          if let gradient {
            Capsule().fill(LinearGradient(colors: gradient, startPoint: .leading, endPoint: .trailing))
          } else {
            Capsule().fill(MVTheme.title.opacity(0.14))
          }
        }
        .frame(width: usable, height: 4)
        .offset(x: r, y: midY - 2)
        if gradient == nil {
          Capsule().fill(enabled ? MVTheme.accent : MVTheme.disabled)
            .frame(width: abs(t - z) * usable, height: 4)
            .offset(x: r + min(t, z) * usable, y: midY - 2)
        }
        Circle()
          .fill(Color.white)
          .overlay(Circle().stroke(Color.black.opacity(0.18), lineWidth: 0.5))
          .shadow(color: .black.opacity(0.25), radius: dragging ? 2.5 : 1.5, y: 0.5)
          .frame(width: Self.thumb, height: Self.thumb)
          .scaleEffect(dragging ? 1.12 : (hover ? 1.06 : 1))
          .offset(x: t * usable, y: midY - r)
      }
      .contentShape(Rectangle())
      .gesture(DragGesture(minimumDistance: 0)
        .onChanged { g in
          dragging = true
          let f = min(max((g.location.x - r) / usable, 0), 1)
          set(spec.min + Double(f) * (spec.max - spec.min))
        }
        .onEnded { _ in dragging = false })
      .simultaneousGesture(TapGesture(count: 2).onEnded { set(zero) })
    }
    .frame(height: 20)
    .frame(minWidth: 60)
    .opacity(enabled ? 1 : 0.45)
    .allowsHitTesting(enabled)
    .onHover { hover = $0 }
    .animation(.easeOut(duration: 0.1), value: hover)
    .accessibilityElement()
    .accessibilityValue(spec.format(value))
    .accessibilityAdjustableAction { direction in
      switch direction {
      case .increment: set(value + spec.step)
      case .decrement: set(value - spec.step)
      @unknown default: break
      }
    }
  }
}

/// A slider's value: click it to type a number (Return sets it, Esc leaves it).
private struct ValueReadout: View {
  let text: String
  let value: Double
  let enabled: Bool
  let param: Int
  @Binding var editing: Int?
  let commit: (Double) -> Void
  @State private var draft = ""
  @State private var hover = false
  @FocusState private var focus: Bool

  var body: some View {
    Group {
      if editing == param {
        TextField("", text: $draft)
          .textFieldStyle(.plain)
          .font(MVTheme.font(13).monospacedDigit())
          .multilineTextAlignment(.trailing)
          .focused($focus)
          .onSubmit {
            let cleaned = draft.replacingOccurrences(of: "EV", with: "")
              .replacingOccurrences(of: ",", with: ".")
              .trimmingCharacters(in: .whitespaces)
            if let v = Double(cleaned) { commit(v) }
            editing = nil
          }
          .onExitCommand { editing = nil }
          .onChange(of: focus) { _, now in if !now && editing == param { editing = nil } }
          .onAppear {
            draft = String(format: abs(value.rounded() - value) < 0.0001 ? "%.0f" : "%.1f", value)
            focus = true
          }
          .padding(.horizontal, MVTheme.space(4))
          .padding(.vertical, MVTheme.space(1))
          .background(RoundedRectangle(cornerRadius: MVTheme.radius(4)).fill(MVTheme.canvas))
          .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(4)).stroke(MVTheme.accent, lineWidth: 1))
          .accessibilityLabel("Type a value")
      } else {
        Button { if enabled { editing = param } } label: {
          Text(text)
            .font(MVTheme.font(13).monospacedDigit())
            .foregroundStyle(enabled ? (hover ? MVTheme.title : MVTheme.body) : MVTheme.disabled)
            .lineLimit(1)
            .padding(.horizontal, MVTheme.space(4))
            .padding(.vertical, MVTheme.space(1))
            .background(RoundedRectangle(cornerRadius: MVTheme.radius(4)).fill(hover && enabled ? MVTheme.hoverWash : .clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
        .help("Click to type a value")
        .accessibilityLabel("Value \(text), click to type")
      }
    }
    .frame(width: 70, alignment: .trailing)
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
    // Read here, not in the Canvas, as HistogramPlot does: the two plots of
    // one store show the same bins (or the same blank) after any change.
    let bins: [Double] = store.histogramValid ? store.histogram[channel == 0 ? 3 : channel - 1] : []
    GeometryReader { geo in
      let size = geo.size
      Canvas { ctx, sz in
        let frame = CGRect(origin: .zero, size: sz)
        let r = Self.plotRect(sz)
        let shape = RoundedRectangle(cornerRadius: MVTheme.radius(4))
        ctx.fill(shape.path(in: frame), with: .color(Color(nsColor: .textBackgroundColor)))
        func at(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: r.minX + x * r.width, y: r.minY + (1 - y) * r.height) }
        if !bins.isEmpty {
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
