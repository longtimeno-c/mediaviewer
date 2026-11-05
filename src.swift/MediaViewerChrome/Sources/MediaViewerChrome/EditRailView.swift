// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S2: the Edit workspace's 44 pt icon rail, docked at
// the window's right edge while the workspace is open (main_mac.mm railHost).
// Top: the tabs (Crop, Adjust, Enhance, Info; Trim and Jobs on a clip). Then a
// hairline and the tools (Remove, Red-eye, Faces, Extend). At the foot: Undo,
// Compare and Done. The active tab's icon folds the inspector to the rail
// (shell/edit_workspace.h rail_pick); any icon of a folded rail unfolds it.
// The Windows twin is IslandHost.Rail.cs.
import MVChromeBridge
import SwiftUI

struct EditRailView: View {
  @ObservedObject private var store = EditStore.shared

  var body: some View {
    VStack(spacing: MVTheme.space(2)) {
      ForEach(store.tabs, id: \.rawValue) { t in
        let active = store.tab == t
        RailButton(symbol: t.symbol,
                   label: t.label,
                   help: railHelp(t, active: active),
                   selected: active && !store.collapsed,
                   marked: active) { store.rail(t) }
          .accessibilityAddTraits(active ? [.isSelected] : [])
      }
      if !store.isClip {
        Rectangle().fill(MVTheme.hairline).frame(width: 24, height: 1).padding(.vertical, MVTheme.space(6))
        RailButton(symbol: store.brushOn ? "paintbrush.pointed.fill" : "eraser",
                   label: "Remove",
                   help: store.enhance ? "Remove objects: brush over what to take out"
                                       : "Remove objects (needs the Enhance add-on)",
                   selected: store.brushOn) {
          // The rail's Remove arms or puts down the brush (the card runs Remove).
          if store.brushOn { store.setBrush(false, size: 0) } else { store.tool(.remove) }
        }
        // S3: Red-eye is a canvas tool; a second click puts it down.
        RailButton(symbol: "eye", label: "Red-eye",
                   help: "Red-eye: click each red pupil on the photo",
                   selected: store.toolBar.tool == .redEye) { store.tool(.redEye) }
        RailButton(symbol: "face.smiling", label: "Faces",
                   help: "Faces: open closed eyes, improve faces from your photos") { store.tool(.faces) }
        RailButton(symbol: "arrow.up.left.and.arrow.down.right", label: "Extend",
                   help: store.enhance ? "Extend edges: more picture around the photo (Enhance)"
                                       : "Extend edges (needs the Enhance add-on)") { store.tool(.extend) }
      }
      Spacer(minLength: MVTheme.space(8))
      if !store.isClip {
        RailButton(symbol: "arrow.uturn.backward", label: "Undo",
                   help: withKey("Undo the last edit", "Undo edit")) { store.run(EditCommand.undo) }
          .disabled(store.editCount == 0)
        RailButton(symbol: "square.split.2x1", label: "Compare",
                   help: store.canCompare ? withKey(store.compareHelp, "Show original (hold)") : store.compareHelp,
                   selected: store.showOriginal) { store.setShowOriginal(!store.showOriginal) }
          .disabled(!store.canCompare)
      }
      RailButton(symbol: "checkmark", label: "Done",
                 help: withKey("Done: close the editor", "Edit image / Edit video"),
                 prominent: true) { store.close() }
    }
    .padding(.vertical, MVTheme.space(8))
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Edit tools")
  }

  private func railHelp(_ t: EditTab, active: Bool) -> String {
    let base: String
    if store.collapsed { base = "Show \(t.label)" }
    else if active { base = "\(t.label): click again to hide the panel" }
    else { base = t.label }
    return t.key.isEmpty ? base : "\(base) (\(t.key))"
  }
}

/// One 32 pt icon of the rail: a wash on hover, the accent and a 2 pt bar on
/// its leading edge while selected; the tooltip and the screen reader carry
/// the name and the live key.
struct RailButton: View {
  let symbol: String
  let label: String
  let help: String
  var selected = false
  /// The rail's current tab while the panel is folded: the bar, no wash.
  var marked = false
  var prominent = false
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      Image(systemName: symbol)
        .font(.system(size: 15, weight: selected ? .semibold : .regular))
        .frame(width: 32, height: 32)
        .foregroundStyle(!enabled ? MVTheme.disabled
                         : (selected ? MVTheme.accent : (prominent ? MVTheme.title : MVTheme.body)))
        .background(
          RoundedRectangle(cornerRadius: MVTheme.radius(7))
            .fill(selected ? MVTheme.selectionWash : (hover && enabled ? MVTheme.hoverWash : .clear)))
        .overlay(alignment: .leading) {
          if selected || marked {
            Capsule().fill(MVTheme.accent).frame(width: 2.5, height: 16).offset(x: -5)
          }
        }
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .help(help)
    .accessibilityLabel(label)
    .accessibilityHint(help)
    .animation(.easeOut(duration: 0.12), value: hover)
    .animation(.easeOut(duration: 0.15), value: selected)
  }
}
