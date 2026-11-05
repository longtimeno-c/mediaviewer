// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S3 (§4.2): the canvas tools' context bar. A 44 pt
// capsule below the photo while a tool is armed on a still: it takes the
// transport's slot (a still has no transport) and the band the canvas leaves
// for it (main_mac.mm toolBarHost, kToolBarBand), so nothing covers the
// picture. The host owns the tool (shell/canvas_tool.h); this reads
// EditStore.toolBar and posts the bar's buttons back. The Windows twin is
// IslandHost.ToolBar.cs.
import MVChromeBridge
import SwiftUI

struct ToolBarView: View {
  @ObservedObject private var store = EditStore.shared
  @State private var dragSize: Double?

  private var bar: ToolBarState { store.toolBar }

  var body: some View {
    // Labels where they fit, icons where they do not (a narrow canvas).
    ViewThatFits(in: .horizontal) {
      row(compact: false)
      row(compact: true)
    }
    .padding(.horizontal, MVTheme.space(12))
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(Capsule().fill(MVTheme.surface).shadow(color: .black.opacity(0.18), radius: 6, y: 2))
    .overlay(Capsule().stroke(MVTheme.hairline, lineWidth: 1))
    .help(hints)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(accessibleName)
    .accessibilityHint(hints)
  }

  @ViewBuilder
  private func row(compact: Bool) -> some View {
    HStack(spacing: MVTheme.space(compact ? 6 : 10)) {
      switch bar.tool {
      case .brush: brushRow(compact: compact)
      case .redEye: redEyeRow(compact: compact)
      case .faces: facesRow(compact: compact)
      case .extend: extendRow(compact: compact)
      default: pickerRow
      }
    }
    .font(MVTheme.font(13))
    .foregroundStyle(MVTheme.body)
    .fixedSize(horizontal: true, vertical: false)
  }

  // Brush: Paint / Erase · Size · Undo · Redo · Clear · "3 strokes" · Remove ↵ · Done.
  @ViewBuilder
  private func brushRow(compact: Bool) -> some View {
    HStack(spacing: 0) {
      ModeButton(symbol: "paintbrush.pointed", label: "Paint", selected: !bar.erase, compact: compact) {
        store.setToolErase(false)
      }
      ModeButton(symbol: "eraser", label: "Erase", selected: bar.erase, compact: compact) {
        store.setToolErase(true)
      }
    }
    .help("Paint or erase (hold ⌥ to erase while you brush)")
    sizeControl(compact: compact)
    BarIcon(symbol: "arrow.uturn.backward", label: withKey("Undo stroke", "Undo stroke")) { store.toolAction(.undo) }
      .disabled(!bar.canUndo)
    BarIcon(symbol: "arrow.uturn.forward", label: withKey("Redo stroke", "Redo stroke")) { store.toolAction(.redo) }
      .disabled(!bar.canRedo)
    BarIcon(symbol: "trash", label: withKey("Clear strokes", "Clear strokes")) { store.toolAction(.clear) }
      .disabled(bar.strokes == 0)
    statusSlot(compact: compact)
      .foregroundStyle(bar.full ? MVTheme.accent : MVTheme.body)
    Button { store.toolAction(.apply) } label: {
      Text(compact ? "Remove" : "Remove ↵")
    }
    .buttonStyle(AccentButtonStyle())
    .disabled(!bar.canApply)
    .help(store.enhance ? withKey("Remove what is brushed", "Apply tool")
                        : "Remove needs the Enhance add-on (Settings → Add-ons)")
    doneButton
  }

  // Red-eye: "Click a red pupil · 2 fixed" · Size · Find eyes · Undo · Done.
  @ViewBuilder
  private func redEyeRow(compact: Bool) -> some View {
    statusSlot(compact: compact)
    sizeControl(compact: compact)
    Button {
      store.toolAction(.findEyes)
    } label: {
      // One width busy or not: both labels are always laid out (the ZStack
      // takes the wider), only one is seen.
      ZStack {
        Text("Find eyes").opacity(bar.busy ? 0 : 1)
        HStack(spacing: MVTheme.space(4)) {
          ProgressView().controlSize(.mini)
          Text("Looking…")
        }
        .opacity(bar.busy ? 1 : 0)
        .accessibilityHidden(!bar.busy)
      }
    }
    .buttonStyle(AccentButtonStyle(filled: bar.canApply))
    .disabled(bar.busy || !bar.canApply)
    .help(bar.eyes > 0 ? "Fix every red eye on the photo, one undo step"
                       : "Find eyes needs People in Local search (Settings → Add-ons)")
    BarIcon(symbol: "arrow.uturn.backward", label: withKey("Undo the last fix", "Undo stroke")) {
      store.toolAction(.undo)
    }
    .disabled(store.editCount == 0)
    doneButton
  }

  // docs/plans/edit-refresh.md S8, Faces: "3 faces · 2 chosen" · Open eyes ·
  // Improve · Do all · Done, on the chosen faces (the panel has each face's own).
  @ViewBuilder
  private func facesRow(compact: Bool) -> some View {
    if bar.busy {
      ProgressView().controlSize(.small)
    }
    Text(facesStatus).lineLimit(1)
    Button { store.toolAction(.facesOpenEyes) } label: {
      HStack(spacing: MVTheme.space(4)) {
        Image(systemName: "eye")
        if !compact { Text("Open eyes") }
      }
    }
    .buttonStyle(FlatButtonStyle(compact: true))
    .disabled(!bar.canApply)
    .help("Open the chosen faces' closed eyes from another photo of them (a new file)")
    Button { store.toolAction(.facesImprove) } label: {
      HStack(spacing: MVTheme.space(4)) {
        Image(systemName: "person.2.crop.square.stack")
        if !compact { Text("Improve") }
      }
    }
    .buttonStyle(FlatButtonStyle(compact: true))
    .disabled(!bar.canApply)
    .help("A clearly better photo of each chosen face, from your photos (a new file)")
    Button { store.toolAction(.facesDoAll) } label: { Text("Do all") }
      .buttonStyle(AccentButtonStyle())
      .disabled(!bar.canApply)
      .help("Each chosen face's best fix, in one new file")
    doneButton
  }

  // S6, Extend: preset chips · "+18 % · 6048×4032" · Fill ↵ · Cancel.
  @ViewBuilder
  private func extendRow(compact: Bool) -> some View {
    if compact {
      // A narrow canvas: the chips fold into a menu.
      Menu(bar.extendPreset >= 0 ? ToolBarState.extendPresets[bar.extendPreset] : "Frame") {
        ForEach(Array(ToolBarState.extendPresets.enumerated()), id: \.offset) { i, label in
          Button(label) { store.extendPreset(i) }
        }
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
      .help("The Enhance add-on's frames")
    } else {
      HStack(spacing: MVTheme.space(2)) {
        ForEach(Array(ToolBarState.extendPresets.enumerated()), id: \.offset) { i, label in
          ChipButton(label: label, selected: bar.extendPreset == i) { store.extendPreset(i) }
        }
      }
    }
    // Past the 25 % warning: the overlay's caution amber, and it says why.
    Text(bar.extendW > 0 ? bar.extendReadout + (bar.extendWarn && !compact ? " · may smear" : "")
                         : "Drag the edges past the photo")
      .monospacedDigit()
      .foregroundStyle(bar.extendWarn ? MVTheme.warm : MVTheme.body)
      .lineLimit(1)
      .help(bar.extendWarn ? "Past 25 % a side, fills may smear" : "The new picture's size")
    Button { store.toolAction(.apply) } label: {
      Text(compact ? "Fill" : "Fill ↵")
    }
    .buttonStyle(AccentButtonStyle())
    .disabled(!bar.canApply)
    .help(store.enhance ? withKey("Fill the new edges (a frame inside the photo crops it)", "Apply tool")
                        : "Filling new edges needs the Enhance add-on (Settings → Add-ons); a frame inside the photo crops it")
    Button { store.toolAction(.done) } label: { Text("Cancel") }
      .buttonStyle(AccentButtonStyle(filled: false))
      .help("Drop the new frame (Esc)")
  }

  // The white-balance picker: one click, then it puts itself down.
  @ViewBuilder
  private var pickerRow: some View {
    Text("Click something that should be neutral grey").lineLimit(1)
    doneButton
  }

  private var doneButton: some View {
    Button { store.toolAction(.done) } label: { Text("Done") }
      .buttonStyle(AccentButtonStyle(filled: false))
      .help("Put the tool down (Esc)")
  }

  // The size: a log slider over 4–400 pt and "48 px".
  @ViewBuilder
  private func sizeControl(compact: Bool) -> some View {
    let px = dragSize ?? bar.sizePx
    HStack(spacing: MVTheme.space(6)) {
      Slider(
        value: Binding(
          get: { log(max(px, 4)) },
          set: { v in
            let p = exp(v).rounded()
            dragSize = p
            store.setToolSize(p)
          }),
        in: log(4.0)...log(400.0),
        onEditingChanged: { editing in if !editing { dragSize = nil } }
      )
      .frame(width: compact ? 70 : 110)
      .accessibilityLabel("Size")
      .accessibilityValue("\(Int(px)) pixels")
      Text("\(Int(px)) px")
        .monospacedDigit()
        .lineLimit(1)
        .frame(width: 48, alignment: .leading)
    }
    .help(sizeHelp)
  }

  /// The status (the stroke count, the hint) in a slot of one width per tool
  /// and size class, so the bar never reflows when its text changes: before,
  /// "Brush over what to remove" → "1 stroke" and back resized the row,
  /// ViewThatFits flipped between labels and icons, every button moved and
  /// their accessibility frames went stale (2026-11-05). The full text is the
  /// tooltip and the accessibility value.
  private func statusSlot(compact: Bool) -> some View {
    Text(status(compact: compact))
      .lineLimit(1)
      .truncationMode(.tail)
      .minimumScaleFactor(0.85)
      .frame(width: Self.statusWidth(bar.tool, compact: compact), alignment: .leading)
      .help(status(compact: false))
      .accessibilityLabel("Status")
      .accessibilityValue(status(compact: false))
  }

  static func statusWidth(_ tool: ToolBarState.Tool, compact: Bool) -> CGFloat {
    switch tool {
    case .brush: return compact ? 100 : 168
    case .redEye: return compact ? 96 : 160
    default: return 0
    }
  }

  private func status(compact: Bool) -> String {
    switch bar.tool {
    case .brush:
      if bar.full { return compact ? "Brush full" : "Brush full: Remove, then brush more" }
      if bar.strokes == 0 { return compact ? "Brush to remove" : "Brush over what to remove" }
      return bar.strokes == 1 ? "1 stroke" : "\(bar.strokes) strokes"
    case .redEye:
      if compact { return bar.spots == 0 ? "Click a pupil" : "\(bar.spots) fixed" }
      let fixed = bar.spots == 0 ? "" : " · \(bar.spots) fixed"
      return "Click a red pupil\(fixed)"
    default:
      return ""
    }
  }

  /// "3 faces · 2 chosen" from the bar (eyes: faces, spots: chosen), else
  /// the panel's own words (no pack, People off).
  private var facesStatus: String {
    if bar.eyes > 0 {
      return "\(bar.eyes == 1 ? "1 face" : "\(bar.eyes) faces") · \(bar.spots) chosen"
    }
    return FacesStore.shared.summary
  }

  private var sizeHelp: String {
    let smaller = liveKey("Smaller brush"), bigger = liveKey("Bigger brush")
    return smaller.isEmpty && bigger.isEmpty ? "Size" : "Size (\(smaller) \(bigger))"
  }

  /// "[ ] size · ⌥ erase · Space pan · ↩ remove", from the live table.
  private var hints: String {
    var parts: [String] = []
    let smaller = liveKey("Smaller brush"), bigger = liveKey("Bigger brush")
    if !smaller.isEmpty || !bigger.isEmpty { parts.append("\(smaller) \(bigger) size") }
    if bar.tool == .extend {
      // S6: drag the edges; Enter fills, Esc cancels.
      let apply = liveKey("Apply tool")
      return "Drag an edge past the photo · \(apply.isEmpty ? "↩" : apply) fill · Esc cancel"
    }
    if bar.tool == .brush { parts.append("⌥ erase") }
    let pan = liveKey("Pan while brushing")
    parts.append("\(pan.isEmpty ? "Space" : pan) pan")
    let apply = liveKey("Apply tool")
    if bar.tool == .brush, !apply.isEmpty { parts.append("\(apply) remove") }
    if bar.tool == .faces { parts = ["Click a face to choose it"] }
    parts.append("Esc done")
    return parts.joined(separator: " · ")
  }

  private var accessibleName: String {
    switch bar.tool {
    case .brush: return "Remove brush"
    case .redEye: return "Red-eye tool"
    case .faces: return "Faces tool"
    case .extend: return "Extend edges"
    case .whiteBalance: return "White balance picker"
    default: return "Canvas tool"
    }
  }
}

/// S6: one of Extend's preset chips.
private struct ChipButton: View {
  let label: String
  let selected: Bool
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Text(label)
        .padding(.horizontal, MVTheme.space(6)).padding(.vertical, MVTheme.space(3))
        .foregroundStyle(selected ? MVTheme.accent : MVTheme.body)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
          .fill(selected ? MVTheme.selectionWash : Color.clear))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityLabel(label)
    .accessibilityAddTraits(selected ? [.isSelected] : [])
  }
}

/// Paint or Erase: one half of the toggle.
private struct ModeButton: View {
  let symbol: String
  let label: String
  let selected: Bool
  let compact: Bool
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      HStack(spacing: MVTheme.space(4)) {
        Image(systemName: symbol)
        if !compact { Text(label) }
      }
      .padding(.horizontal, MVTheme.space(8)).padding(.vertical, MVTheme.space(4))
      .foregroundStyle(selected ? MVTheme.accent : MVTheme.body)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .fill(selected ? MVTheme.selectionWash : Color.clear))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityLabel(label)
    .accessibilityAddTraits(selected ? [.isSelected] : [])
  }
}

/// A 28 pt icon button with its name (and live key) as the tooltip.
private struct BarIcon: View {
  let symbol: String
  let label: String
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      Image(systemName: symbol)
        .frame(width: 28, height: 28)
        .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
          .fill(hover && enabled ? MVTheme.hoverWash : Color.clear))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .help(label)
    .accessibilityLabel(label)
  }
}
