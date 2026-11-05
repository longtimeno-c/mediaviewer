// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S2 (§3.1): the Edit workspace's Enhance tab (tab 5)
// and the header's ✦ popover. Five tools, one card each: Remove objects,
// Restyle, Faces, Enhance image, Extend edges. A card says what the tool does,
// whether it can run now (its chip) and has one primary button. They launch
// what exists: the Remove brush on the photo, Enhance's own sheet started on
// the right choice (its "open" and "restyle" commands; Extend through the
// sheet's photo choice), and the Faces tool (S8: a box per face on the photo
// and FacesPanelView here). Every result is a new file (or a new
// Photos-library photo); the original never changes.
// The Windows twin is IslandHost.EnhanceHub.cs.
//
// One-click (owner 2026-11-05, docs/design/29 "One click"): no sheet and no
// panel. Enhance's "tool" screen is the tab's cards (EnhanceRun.swift): each
// card draws its tool's compact choices, the time the run will take and its
// one button ("Enhance", or "Download (161 MB) and enhance") from the
// add-on's `card.<word>` stack. One click runs it: the glow over the photo,
// the tool bar's line, then the new photo opens with Edit kept on this tab.
// Remove and Extend run from the tool bar under the photo (Remove ↵, Fill ↵).
import Combine
import MVChromeBridge
import SwiftUI

/// One tool as both the cards and the popover show it.
struct EnhanceTool: Identifiable {
  /// restoreFaces (S10) is the Faces card's second button and a popover row
  /// of its own, not a sixth card.
  enum Kind { case remove, restyle, faces, detail, extend, restoreFaces }
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
    case .restoreFaces: return "faces"
    case .faces: return nil
    }
  }

  /// The add-on's card stack this card draws (`card.<word>`): its own tool's,
  /// and for the Faces card Restore faces' (the Faces tool staying the host's).
  var cardWord: String? { kind == .faces ? "faces" : photo }

  var title: String {
    switch kind {
    case .remove: return "Remove objects"
    case .restyle: return "Restyle"
    case .faces: return "Faces"
    case .detail: return "Enhance image"
    case .extend: return "Extend edges"
    case .restoreFaces: return "Restore faces"
    }
  }

  var purpose: String {
    switch kind {
    // One short line each (docs/design/20 "Craft rules": one surface, one line).
    case .remove: return "Brush over something to take it out."
    case .restyle: return "Describe a new look: a snowy day, golden hour."
    case .faces: return "Open closed eyes, or a better face of the same person."
    case .detail: return "Sharpen, clean up noise or blur, or enlarge."
    case .extend: return "Add more picture around the edges."
    case .restoreFaces:
      return "Sharper, clearer faces, each checked by People to still look like the same person. Saves a new file."
    }
  }

  var symbol: String {
    switch kind {
    case .remove: return "eraser"
    case .restyle: return "paintpalette"
    case .faces: return "face.smiling"
    case .detail: return "plus.magnifyingglass"  // more detail; never a sparkle
    case .extend: return "arrow.up.left.and.arrow.down.right"
    case .restoreFaces: return "person.crop.circle.badge.checkmark"
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
    case .restyle, .detail:
      // One-click: offered whenever the add-on's cards have the tool (never a
      // palette command: the owner's "Not offered", 2026-11-05).
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else { (chip, chipText, action, enabled) = Self.offered(tool) }
    case .faces:
      // S8: the card arms the Faces tool; FacesPanelView takes over while it is.
      let armed = FacesStore.shared.armed
      if store.eyesBusy { (chip, chipText, action, enabled) = (.busy, "Working…", "Choose faces", false) }
      else if armed { (chip, chipText, action, enabled) = (.brushing, "Choosing", "Done", true) }
      else if store.facesReady { (chip, chipText, action, enabled) = (.ready, "Ready", "Choose faces", true) }
      else { (chip, chipText, action, enabled) = (.needsPeople, "Needs People", "Choose faces", true) }
    case .extend:
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else { (chip, chipText, action, enabled) = (.ready, "Ready", "Set the new frame", store.offersExtend) }
    case .restoreFaces:
      if !store.enhance { (chip, chipText, action, enabled) = (.needsAddon, "Needs Enhance", "Get Enhance…", true) }
      else if EnhanceRun.shared.card("faces") == nil { (chip, chipText, action, enabled) = Self.offered(tool) }
      else if store.facesReady {
        (chip, chipText, action, enabled) = (.ready, "Ready", "Restore faces", true)
      } else {
        (chip, chipText, action, enabled) = (.needsPeople, "Needs People", "Restore faces", true)
      }
    }
  }

  /// A tool the cards draw: ready when the add-on's tree has its card, else
  /// why not (the cards still opening, an Enhance to update, or one without it).
  private static func offered(_ tool: EnhanceTool) -> (Chip, String, String, Bool) {
    let run = EnhanceRun.shared
    if let word = tool.cardWord, run.card(word) != nil {
      return (.ready, run.working && run.tool == word ? "Working…" : "Ready", tool.title, true)
    }
    if run.outdated { return (.needsAddon, "Update Enhance", "Update Enhance…", true) }
    if run.cards == nil { return (.busy, "Opening…", tool.title, false) }
    return (.needsAddon, "Not in this Enhance", tool.title, false)
  }

  /// The primary button's work.
  static func run(_ tool: EnhanceTool, _ store: EditStore) {
    let cardsTool = tool.kind == .restyle || tool.kind == .detail || tool.kind == .restoreFaces
    let needsAddon = (!store.enhance && tool.kind != .faces) || (cardsTool && EnhanceRun.shared.outdated)
    if needsAddon {
      AddonNav.shared.show("enhance")
      return
    }
    switch tool.kind {
    case .remove:
      // One-click: Remove hands the strokes over and runs (the glow, then the
      // new photo); the tool bar's Remove ↵ does the same.
      if store.brushOn && store.brushed { store.brushAction(2) }
      else if store.brushOn { store.setBrush(false, size: 0) }
      else { store.tool(.remove) }
    case .faces: store.tool(.faces)  // S8: the Faces tool; a second press puts it down
    // S6: the frame on the canvas (a second press puts it down); Fill ↵ on
    // the tool bar runs it.
    case .extend: store.tool(.extend)
    // The popover's rows: Enhance image and Restore faces run at once with
    // the card's choices (downloading first when the model is not here);
    // Restyle needs its words, so it shows the tab.
    case .detail: store.enhanceTool("detail", run: true)
    case .restoreFaces: store.enhanceTool("faces", run: true)
    case .restyle:
      if EnhanceRun.shared.outdated { AddonNav.shared.show("enhance") } else { store.select(.enhance) }
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
  @ObservedObject private var faces = FacesStore.shared
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
        // "The original is not changed" is the result's to say, once
        // (docs/design/20 "Craft rules"), not every card's and header's.
      }
      if !store.enhance {
        InstallBanner()
      }
      // S8: the Faces tool's panel, first while the tool is armed.
      if faces.armed {
        FacesPanelView()
          .transition(.opacity.combined(with: .move(edge: .top)))
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
    .animation(.easeOut(duration: 0.18), value: faces.armed)
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

  @ObservedObject private var run = EnhanceRun.shared

  /// The add-on's stack for this card (`card.<word>`), drawn inside it.
  private var cardTree: ScreenNode? {
    guard store.enhance, let word = tool.cardWord else { return nil }
    return run.card(word)
  }

  /// Whether the card's own (host) button shows: the brush's and the frame's,
  /// the Faces tool's, and Get / Update Enhance; the cards' tools run from
  /// the add-on's own button inside the card.
  private func hostButton(_ state: EnhanceToolState) -> Bool {
    switch tool.kind {
    case .remove, .faces, .extend: return true
    case .restyle, .detail, .restoreFaces: return state.chip == .needsAddon && state.enabled
    }
  }

  var body: some View {
    let state = EnhanceToolState(tool, store)
    let cardTree = self.cardTree
    let chosen = state.chip == .brushing || state.chip == .brushed || (run.working && run.tool == tool.cardWord)
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      HStack(alignment: .center, spacing: MVTheme.space(8)) {
        Image(systemName: tool.symbol)
          .font(.system(size: 15, weight: .medium))
          .foregroundStyle(MVTheme.accent)
          .frame(width: 20, height: 20)
          .accessibilityHidden(true)
        Text(tool.title)
          .font(MVTheme.font(15))
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
        Spacer(minLength: MVTheme.space(8))
        // Only when something is needed: "Ready" is not worth a chip.
        if state.chip == .needsAddon || state.chip == .needsPeople || state.chip == .busy {
          StateChip(state: state)
        }
      }
      Text(tool.purpose)
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: MVTheme.space(6)) {
        if hostButton(state) {
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
        Spacer(minLength: 0)
      }
      if tool.kind == .faces && store.enhance && cardTree != nil {
        // S10: Enhance's face model, guarded by People: its Look, Strength,
        // the time a face takes and its one button, below Choose faces.
        Text("Restore faces").font(MVTheme.font(12).weight(.semibold)).foregroundStyle(MVTheme.body)
          .padding(.top, MVTheme.space(4))
          .accessibilityAddTraits(.isHeader)
          .help(EnhanceTool(kind: .restoreFaces).purpose)
      }
      if tool.kind == .extend && store.enhance {
        Text("Drag the frame's edges past the photo, then Fill (↩).")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }
      if tool.kind == .remove && store.brushFull {
        Text("Brush full: Remove, then brush more.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }
      if let cardTree {
        ScreenNodeView(node: cardTree, address: EnhanceRun.address, revision: run.revision)
          .environment(\.screenWindow, true)
      }
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    // One surface, no ring: the active card (brushing, choosing, running)
    // lifts to a lighter step of the surface; a ring is keyboard focus's
    // (docs/design/20 "Craft rules").
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(10))
      .fill(chosen ? MVTheme.raised : (hover && cardTree == nil ? MVTheme.hoverWash : MVTheme.surface2)))
    .onHover { hover = $0 }
    .animation(.easeOut(duration: 0.12), value: hover)
    .animation(.easeOut(duration: 0.15), value: chosen)
    .animation(.easeOut(duration: 0.18), value: cardTree != nil)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(tool.title), \(state.chipText)")
  }
}

private struct StateChip: View {
  let state: EnhanceToolState

  var body: some View {
    // What is needed, in words on a quiet fill: no dot, no outline.
    Text(state.chipText).font(MVTheme.font(11)).lineLimit(1)
      .foregroundStyle(MVTheme.body)
      .padding(.horizontal, MVTheme.space(7))
      .padding(.vertical, MVTheme.space(2))
      .background(Capsule().fill(MVTheme.surface2))
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
