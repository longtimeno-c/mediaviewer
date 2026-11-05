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
    Text(status)
      .foregroundStyle(bar.full ? MVTheme.accent : MVTheme.body)
      .lineLimit(1)
      .frame(minWidth: compact ? 0 : 70, alignment: .leading)
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
    Text(status).lineLimit(1)
    sizeControl(compact: compact)
    Button {
      store.toolAction(.findEyes)
    } label: {
      if bar.busy {
        HStack(spacing: MVTheme.space(6)) {
          ProgressView().controlSize(.small)
          Text("Looking…")
        }
      } else {
        Text("Find eyes")
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
    Text(bar.extendW > 0 ? bar.extendReadout : "Drag the edges past the photo")
      .monospacedDigit()
      .foregroundStyle(bar.extendWarn ? MVTheme.accent : MVTheme.body)
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
        .frame(minWidth: 44, alignment: .leading)
    }
    .help(sizeHelp)
  }

  private var status: String {
    switch bar.tool {
    case .brush:
      if bar.full { return "Brush full: Remove, then brush more" }
      if bar.strokes == 0 { return "Brush over what to remove" }
      return bar.strokes == 1 ? "1 stroke" : "\(bar.strokes) strokes"
    case .redEye:
      let fixed = bar.spots == 0 ? "" : " · \(bar.spots) fixed"
      return "Click a red pupil\(fixed)"
    default:
      return ""
    }
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
    parts.append("Esc done")
    return parts.joined(separator: " · ")
  }

  private var accessibleName: String {
    switch bar.tool {
    case .brush: return "Remove brush"
    case .redEye: return "Red-eye tool"
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
