// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S2 (§3.1): the Edit workspace's Enhance tab (tab 5)
// and the header's ✦ popover. Five tools, one card each: Remove objects,
// Restyle, Faces, Enhance image, Extend edges. A card says what the tool does,
// whether it can run now (its chip) and has one primary button. They launch
// what exists: the Remove brush on the photo, Enhance's own sheet started on
// the right choice (its "open" and "restyle" commands; Extend through the
// sheet's photo choice), and Open closed eyes / Improve faces. Every result
// is a new file (or a new Photos-library photo); the original never changes.
// The Windows twin is IslandHost.EnhanceHub.cs.
//
// S5 (owner call B): Remove, Restyle, Enhance image and Extend run inside the
// pane. A card asks Enhance for its docked screen (place "edit", the
// workspace's enhance/tool command), which is drawn under that card
// (DockedToolPanel): the tool's knobs, the preview, the glowing progress with
// the time left, Cancel, and the result row. The row's Show and Keep editing
// are the viewer's (ScreenButtonHook); Move to Trash is Enhance's (the host
// table's recycle_file). An Enhance without the docked screen opens its sheet.
import Combine
import MVChromeBridge
import SwiftUI

/// One tool as both the cards and the popover show it.
struct EnhanceTool: Identifiable {
  enum Kind { case remove, restyle, faces, detail, extend }
  let kind: Kind
  var id: Kind { kind }

  /// Enhance's word for the tool (its enhance/tool "photo"; the docked tree's
  /// root is "tool.<word>"). Faces is the host's own.
  var photo: String? {
    switch kind {
    case .remove: return "remove"
    case .restyle: return "restyle"
    case .detail: return "detail"
    case .extend: return "extend"
    case .faces: return nil
    }
  }

  var title: String {
    switch kind {
    case .remove: return "Remove objects"
    case .restyle: return "Restyle"
    case .faces: return "Faces"
    case .detail: return "Enhance image"
    case .extend: return "Extend edges"
    }
  }

  var purpose: String {
    switch kind {
    case .remove: return "Brush over what to take out; it is filled in from around it."
    case .restyle: return "Describe a new look in words: a snowy day, golden hour."
    case .faces: return "Open closed eyes, or a better face of the same person from your photos."
    case .detail: return "Sharper detail and less noise, restored by a model on this Mac."
    case .extend: return "More picture around the edges, to a wider or taller frame."
    }
  }

  var symbol: String {
    switch kind {
    case .remove: return "eraser"
    case .restyle: return "paintpalette"
    case .faces: return "face.smiling"
    case .detail: return "sparkles"
    case .extend: return "arrow.up.left.and.arrow.down.right"
    }
  }

  static let all: [EnhanceTool] = [.init(kind: .remove), .init(kind: .restyle), .init(kind: .faces),
                                   .init(kind: .detail), .init(kind: .extend)]
}

/// What a card's chip and button say for the photo on screen, from the edit
/// view's wand bits (shell/adjust_pane.h kWand*).
@MainActor
struct EnhanceToolState {
  enum Chip { case ready, busy, needsAddon, needsPeople, brushing, brushed }
  let chip: Chip
  let chipText: String
  let action: String
  let enabled: Bool

  init(_ tool: EnhanceTool, _ store: EditStore) {
    switch tool.kind {
    case .remove:
      if !store.enhance {
        (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true)
      } else if store.brushOn && store.brushed {
        (chip, chipText, action, enabled) = (.brushed, store.brushFull ? "Brush full" : "Brushed", "Remove", true)
      } else if store.brushOn {
        (chip, chipText, action, enabled) = (.brushing, "Brushing", "Done brushing", true)
      } else {
        (chip, chipText, action, enabled) = (.ready, "Ready", "Start brushing", true)
      }
    case .restyle:
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else if store.offersRestyle { (chip, chipText, action, enabled) = (.ready, "Ready", "Set up Restyle", true) }
      else { (chip, chipText, action, enabled) = (.needsAddon, "Not offered", "Set up Restyle", false) }
    case .faces:
      if store.eyesBusy { (chip, chipText, action, enabled) = (.busy, "Working…", "Open closed eyes", false) }
      else if store.facesReady { (chip, chipText, action, enabled) = (.ready, "Ready", "Open closed eyes", true) }
      else { (chip, chipText, action, enabled) = (.needsPeople, "Needs People", "Open closed eyes", true) }
    case .detail:
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else { (chip, chipText, action, enabled) = (.ready, "Ready", "Set up", store.offersDetail) }
    case .extend:
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else { (chip, chipText, action, enabled) = (.ready, "Ready", "Choose a frame", store.offersExtend) }
    }
  }

  /// The primary button's work.
  static func run(_ tool: EnhanceTool, _ store: EditStore) {
    let needsAddon = !store.enhance && tool.kind != .faces
    if needsAddon {
      AddonNav.shared.show("enhance")
      return
    }
    switch tool.kind {
    case .remove:
      // Remove hands the strokes to the docked panel and runs (S5).
      if store.brushOn && store.brushed { store.brushAction(2) }
      else if store.brushOn { store.setBrush(false, size: 0) }
      else {
        store.tool(.remove)
        store.enhanceTool("remove")  // its knobs and, later, its result under the card
      }
    case .faces: store.openClosedEyes()
    case .restyle, .detail, .extend:
      if let photo = tool.photo { store.enhanceTool(photo) }
    }
  }
}

/// docs/plans/edit-refresh.md S9 (§3.3): the performance level Enhance runs at,
/// as its settings page names it ("Default level", the "tierlevel" node). That
/// page's screen is open while it shows in Settings; the Enhance tab opens it
/// too while it shows and keeps the last text read between (the level changes
/// only in Settings, whose tree passes through here). The Windows twin is
/// NoteEnhanceLevel in IslandHost.EnhanceHub.cs.
@MainActor
final class EnhanceLevel: ObservableObject {
  static let shared = EnhanceLevel()
  static let addon = "enhance"
  @Published private(set) var text = ""
  private var shown = 0  // Enhance tabs on screen
  private var opened = false  // the tab opened the screen
  private var watch: [AnyCancellable] = []

  private init() {
    watch.append(SlotStore.shared.$screens.sink { [weak self] in self?.read($0) })
    // Enhance loaded (or again) while the tab shows: ask for its page.
    watch.append(SlotStore.shared.$settingsScreens.sink { [weak self] screens in
      guard let self, self.shown > 0, let id = screens[Self.addon] else { return }
      DispatchQueue.main.async { self.open(ScreenAddress(addon: Self.addon, screen: id)) }
    })
  }

  private var address: ScreenAddress? {
    SlotStore.shared.settingsScreens[Self.addon].map { ScreenAddress(addon: Self.addon, screen: $0) }
  }

  private func read(_ screens: [SlotScreen]) {
    guard let at = address,
          let tree = screens.first(where: { $0.addon == at.addon && $0.screen == at.screen })?.tree,
          let t = Self.node("tierlevel", in: tree)?.string("text"), !t.isEmpty, t != text else { return }
    text = t
  }

  private static func node(_ id: String, in n: ScreenNode) -> ScreenNode? {
    if n.id == id { return n }
    for c in n.children { if let hit = node(id, in: c) { return hit } }
    return nil
  }

  private func open(_ at: ScreenAddress) {
    SlotStore.shared.openSettingsScreen(at)  // a no-op in the core while it is open
    opened = true
  }

  func appeared() {
    shown += 1
    if let at = address { open(at) }
  }

  func disappeared() {
    shown = max(0, shown - 1)
    guard shown == 0, opened, let at = address else { return }
    opened = false
    // Settings drawing Enhance's page keeps it (and closes it itself).
    if SettingsStore.shared.visible && AddonNav.shared.page == Self.addon { return }
    SlotStore.shared.close(at)
  }
}

/// The header's chip: the level, a click away from changing it.
private struct LevelChip: View {
  @ObservedObject private var level = EnhanceLevel.shared
  @State private var hover = false

  var body: some View {
    if !level.text.isEmpty {
      Button { AddonNav.shared.show(EnhanceLevel.addon) } label: {
        HStack(spacing: MVTheme.space(4)) {
          Image(systemName: "gauge.with.dots.needle.67percent")
            .font(.system(size: 10, weight: .medium))
            .foregroundStyle(MVTheme.accent)
          Text(level.text).font(MVTheme.font(11)).lineLimit(1)
        }
        .foregroundStyle(MVTheme.title)
        .padding(.horizontal, MVTheme.space(7))
        .padding(.vertical, MVTheme.space(2))
        .background(Capsule().fill(hover ? MVTheme.hoverWash : MVTheme.surface2))
        .overlay(Capsule().stroke(MVTheme.hairline, lineWidth: 0.5))
        .contentShape(Capsule())
      }
      .buttonStyle(.plain)
      .fixedSize()
      .onHover { hover = $0 }
      .help("Enhance's performance level. Change it in Settings → Add-ons → Enhance.")
      .accessibilityLabel("Performance level: \(level.text)")
      .accessibilityHint("Opens Enhance's settings")
    }
  }
}

/// The Enhance tab: five cards, two columns from 560 pt.
struct EnhanceHubView: View {
  @ObservedObject private var store = EditStore.shared
  @Environment(\.paneWidthClass) private var cls

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(12)) {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        HStack(alignment: .center, spacing: MVTheme.space(8)) {
          Text("Enhance").font(MVTheme.font(17)).foregroundStyle(MVTheme.title)
            .accessibilityAddTraits(.isHeader)
          Spacer(minLength: 0)
          if store.enhance { LevelChip() }
        }
        Text("Each tool writes a new file; the original is never changed.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      if !store.enhance {
        InstallBanner()
      }
      if cls.twoColumns {
        let tools = EnhanceTool.all
        HStack(alignment: .top, spacing: MVTheme.space(10)) {
          VStack(spacing: MVTheme.space(10)) {
            ForEach(tools.indices.filter { $0 % 2 == 0 }, id: \.self) { EnhanceCard(tool: tools[$0]) }
          }
          VStack(spacing: MVTheme.space(10)) {
            ForEach(tools.indices.filter { $0 % 2 == 1 }, id: \.self) { EnhanceCard(tool: tools[$0]) }
          }
        }
      } else {
        VStack(spacing: MVTheme.space(10)) {
          ForEach(EnhanceTool.all) { EnhanceCard(tool: $0) }
        }
      }
    }
    .disabled(!store.canEdit || store.isClip)
    .onAppear { EnhanceLevel.shared.appeared() }
    .onDisappear { EnhanceLevel.shared.disappeared() }
  }
}

private struct InstallBanner: View {
  var body: some View {
    HStack(alignment: .top, spacing: MVTheme.space(10)) {
      Image(systemName: "puzzlepiece.extension")
        .font(.system(size: 18))
        .foregroundStyle(MVTheme.accent)
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
        Text("Add the Enhance add-on").font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        Text("Remove, Restyle, Enhance image and Extend run on this Mac with models it downloads when you first use them.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
        Button("Open Add-ons…") { AddonNav.shared.show("enhance") }
          .buttonStyle(AccentButtonStyle())
      }
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(10)).fill(MVTheme.selectionWash))
  }
}

/// One tool: its icon, name, chip, what it does and its primary button.
struct EnhanceCard: View {
  let tool: EnhanceTool
  @ObservedObject private var store = EditStore.shared
  @ObservedObject private var slots = SlotStore.shared
  @State private var hover = false

  /// Enhance's docked screen when it is open on this card's tool.
  private var docked: SlotScreen? {
    guard let photo = tool.photo,
          let screen = slots.screens.first(where: { $0.place == "edit" && $0.addon == "enhance" }),
          let root = screen.tree?.id, root == "tool." + photo
    else { return nil }
    return screen
  }

  var body: some View {
    let state = EnhanceToolState(tool, store)
    let docked = self.docked
    let chosen = docked != nil || state.chip == .brushing || state.chip == .brushed
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      HStack(alignment: .center, spacing: MVTheme.space(10)) {
        Image(systemName: tool.symbol)
          .font(.system(size: 15, weight: .medium))
          .foregroundStyle(MVTheme.accent)
          .frame(width: 30, height: 30)
          .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.selectionWash))
        Text(tool.title)
          .font(MVTheme.font(15))
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
        Spacer(minLength: MVTheme.space(4))
        StateChip(state: state)
      }
      Text(tool.purpose)
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: MVTheme.space(6)) {
        // Docked, the panel below carries the run (Remove keeps its brush buttons).
        if docked == nil || tool.kind == .remove {
          Button(state.action) { EnhanceToolState.run(tool, store) }
            .buttonStyle(AccentButtonStyle(filled: state.chip != .needsAddon && state.chip != .needsPeople))
            .disabled(!state.enabled)
        }
        if tool.kind == .remove && store.brushOn {
          Button("Undo stroke") { store.brushAction(0) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(!store.brushed)
          Button("Clear") { store.brushAction(1) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(!store.brushed)
        }
        if tool.kind == .faces {
          Button("Improve faces") { store.improveFaces() }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(store.eyesBusy)
            .help("Each face replaced by a clearly better one of the same person from your photos (People in Local search). Saves a new file.")
        }
        Spacer(minLength: 0)
      }
      if tool.kind == .remove && store.brushFull {
        Text("Brush full: Remove, then brush more.")
          .font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
      }
      if let docked {
        DockedToolPanel(screen: docked)
          .transition(.opacity.combined(with: .move(edge: .top)))
      }
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(10))
      .fill(hover && docked == nil ? MVTheme.hoverWash : MVTheme.surface2))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(10))
      .stroke(chosen ? MVTheme.accent : MVTheme.hairline.opacity(0.6), lineWidth: chosen ? 1.5 : 0.5))
    .onHover { hover = $0 }
    .animation(.easeOut(duration: 0.12), value: hover)
    .animation(.easeOut(duration: 0.18), value: docked?.revision != nil)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(tool.title), \(state.chipText)")
  }
}

/// S5: Enhance's docked screen (place "edit") under the chosen card, drawn
/// with the add-on screens' renderer. Show and Keep editing are the viewer's:
/// they navigate to the new file (or Photos-library photo) the row's
/// thumbnail names. Remove has no run button of its own here: the card's
/// Remove (or Return in the brush) hands the strokes over and runs it.
struct DockedToolPanel: View {
  let screen: SlotScreen
  @ObservedObject private var slots = SlotStore.shared
  @ObservedObject private var store = EditStore.shared

  var body: some View {
    let address = ScreenAddress(addon: screen.addon, screen: screen.screen)
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
      HStack(spacing: MVTheme.space(6)) {
        Rectangle().fill(MVTheme.hairline).frame(height: 0.5)
        Button { slots.close(address) } label: {
          Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
        }
        .buttonStyle(FlatButtonStyle(compact: true))
        .help("Close (a run in progress stops)")
        .accessibilityLabel("Close the tool panel")
      }
      if let tree = screen.tree {
        ScreenNodeView(node: tree, address: address, revision: screen.revision)
          .environment(\.screenWindow, true)
          .environment(\.screenButtonHook, ScreenButtonHook { _, id in take(id, tree) })
      } else {
        HStack(spacing: MVTheme.space(8)) {
          ProgressView().controlSize(.small)
          Text("Opening…").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        }
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }

  /// The host's buttons: true when the click was the viewer's.
  private func take(_ id: String, _ tree: ScreenNode) -> Bool {
    switch id {
    case "result.show", "result.keep":
      guard let path = Self.find("result.thumb", in: tree)?.string("path"), !path.isEmpty else { return true }
      store.enhanceResult(path: path, keep: id == "result.keep")
      return true
    default:
      return false
    }
  }

  private static func find(_ id: String, in node: ScreenNode) -> ScreenNode? {
    if node.id == id { return node }
    for child in node.children {
      if let hit = find(id, in: child) { return hit }
    }
    return nil
  }
}

private struct StateChip: View {
  let state: EnhanceToolState

  var body: some View {
    let ready = state.chip == .ready || state.chip == .brushing || state.chip == .brushed
    HStack(spacing: MVTheme.space(4)) {
      if state.chip == .busy {
        ProgressView().controlSize(.mini)
      } else {
        Circle().fill(ready ? Color.green : MVTheme.body.opacity(0.6)).frame(width: 6, height: 6)
      }
      Text(state.chipText).font(MVTheme.font(11)).lineLimit(1)
    }
    .foregroundStyle(ready ? MVTheme.title : MVTheme.body)
    .padding(.horizontal, MVTheme.space(7))
    .padding(.vertical, MVTheme.space(2))
    .background(Capsule().fill(MVTheme.surface2))
    .overlay(Capsule().stroke(MVTheme.hairline, lineWidth: 0.5))
    .fixedSize()
  }
}

/// A primary action: filled with the accent (Save copy…, a card's button), or
/// outlined when it leads elsewhere (Get Enhance…).
struct AccentButtonStyle: ButtonStyle {
  var filled = true
  func makeBody(configuration: Configuration) -> some View {
    AccentBody(configuration: configuration, filled: filled)
  }
  private struct AccentBody: View {
    let configuration: Configuration
    let filled: Bool
    @State private var hover = false
    @Environment(\.isEnabled) private var enabled
    var body: some View {
      configuration.label
        .font(MVTheme.font(14))
        .lineLimit(1)
        .foregroundStyle(!enabled ? MVTheme.disabled : (filled ? MVTheme.onAccent : MVTheme.accent))
        .padding(.horizontal, MVTheme.space(12)).padding(.vertical, MVTheme.space(5))
        .background(
          RoundedRectangle(cornerRadius: MVTheme.radius(6))
            .fill(filled ? (enabled ? MVTheme.accent : MVTheme.disabled.opacity(0.3)) : Color.clear))
        .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(6))
          .stroke(filled ? Color.clear : MVTheme.accent, lineWidth: 1))
        .brightness(configuration.isPressed ? -0.08 : (hover && enabled ? 0.06 : 0))
        .contentShape(Rectangle())
        .onHover { hover = $0 }
    }
  }
}

/// The header's ✦ chevron: the five tools in a popover (compact widths keep
/// them a click away). The same rows the Enhance tab's cards run.
struct EnhanceToolsPopover: View {
  @ObservedObject private var store = EditStore.shared
  let dismiss: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      Text("Enhance tools").font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .padding(.horizontal, MVTheme.space(8)).padding(.bottom, MVTheme.space(4))
      ForEach(EnhanceTool.all) { tool in
        PopoverRow(tool: tool, state: EnhanceToolState(tool, store)) {
          dismiss()
          EnhanceToolState.run(tool, store)
        }
      }
      Divider().padding(.vertical, MVTheme.space(4))
      Button {
        dismiss()
        store.select(.enhance)
      } label: {
        Label("Show the Enhance tab", systemImage: "sidebar.right")
          .font(MVTheme.font(13))
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .buttonStyle(FlatButtonStyle(compact: true))
    }
    .padding(MVTheme.space(10))
    .frame(width: 300)
  }
}

private struct PopoverRow: View {
  let tool: EnhanceTool
  let state: EnhanceToolState
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      HStack(spacing: MVTheme.space(10)) {
        Image(systemName: tool.symbol)
          .font(.system(size: 14))
          .foregroundStyle(MVTheme.accent)
          .frame(width: 26, height: 26)
          .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(MVTheme.selectionWash))
        VStack(alignment: .leading, spacing: 1) {
          Text(tool.title).font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
          Text(state.chip == .ready ? state.action : state.chipText)
            .font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
        }
        Spacer(minLength: 0)
      }
      .padding(.horizontal, MVTheme.space(6))
      .padding(.vertical, MVTheme.space(4))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(hover ? MVTheme.hoverWash : .clear))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!state.enabled)
    .onHover { hover = $0 }
    .help(tool.purpose)
    .accessibilityLabel("\(tool.title), \(state.chipText)")
  }
}
