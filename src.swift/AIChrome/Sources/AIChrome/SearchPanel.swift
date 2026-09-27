// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search panel (the chrome brief, "Search panel"): a floating, translucent
// panel centred over the viewer window, never modal — the viewer stays usable
// behind it. A borderless NSPanel that becomes key, a child of the viewer
// window so it moves with it, hosting SwiftUI drawn on a behind-window
// material with 12 pt corners.
//
// Motion: in = opacity 0→1 and scale 0.96→1 on a spring (response 0.32,
// damping 0.86); out = 140 ms ease-out; tiles fade and rise in, staggered 25 ms
// for the first 16; hover lifts a tile; the selection ring moves between tiles
// with matchedGeometryEffect. Reduce motion keeps the fades and drops the rest.
// Nothing here runs on the canvas's render path.
//
// Keyboard-complete (plan/17 PR 22 verify: "the whole flow … works without the
// mouse"): typing searches; Down enters the grid; arrows move; Return opens the
// results in the viewer on the chosen tile; Cmd+Return opens them as the gallery
// grid; Esc goes from the grid back to the field, then closes. Down leaves the
// field through a local key monitor: the field editor would take moveDown:
// before a SwiftUI key handler on the TextField sees it.
import AppKit
import SwiftUI

/// A borderless panel still takes the keyboard.
final class SearchPanel: NSPanel {
  override var canBecomeKey: Bool { true }
  override var canBecomeMain: Bool { false }
}

@MainActor
final class PanelState: ObservableObject {
  @Published var shown = false
  /// Moves on every "focus the field" (Cmd+F again, a fresh open).
  @Published var focusSeq = 0
  /// Moves on Down in the field: the grid takes the keyboard.
  @Published var gridSeq = 0
}

@MainActor
final class SearchPanelController: NSObject {
  let model: SearchModel
  let state = PanelState()
  private let panel: SearchPanel
  private weak var parentWindow: NSWindow?
  private var hideSeq = 0
  private var keyMonitor: Any?
  private var observers: [NSObjectProtocol] = []
  /// Room around the rounded card for its soft shadow; transparent to clicks.
  static let margin: CGFloat = 28

  init(model: SearchModel) {
    self.model = model
    panel = SearchPanel(contentRect: NSRect(x: 0, y: 0, width: 860, height: 620),
                        styleMask: [.borderless, .fullSizeContentView],
                        backing: .buffered, defer: true)
    super.init()
    panel.isOpaque = false
    panel.backgroundColor = .clear
    panel.hasShadow = false  // SwiftUI draws the card's shadow
    panel.isReleasedWhenClosed = false
    panel.hidesOnDeactivate = true
    panel.isMovableByWindowBackground = false
    panel.collectionBehavior = [.fullScreenAuxiliary, .moveToActiveSpace]
    panel.animationBehavior = .none  // SwiftUI animates the card
    panel.setAccessibilityTitle("Search photos and videos")
    let root = SearchRootView(model: model, panel: state, close: { [weak self] in self?.hide() })
    let hosting = NSHostingView(rootView: root)
    hosting.sizingOptions = []
    panel.contentView = hosting
    keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
      let window = event.windowNumber, code = event.keyCode, flags = event.modifierFlags
      let taken = MainActor.assumeIsolated { self?.key(window: window, code: code, flags: flags) ?? false }
      return taken ? nil : event
    }
    // hidesOnDeactivate hides the panel with the app: it stops polling too.
    let center = NotificationCenter.default
    for (name, active) in [(NSApplication.didResignActiveNotification, false),
                           (NSApplication.didBecomeActiveNotification, true)] {
      observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
        MainActor.assumeIsolated { self?.model.appActiveChanged(active) }
      })
    }
  }

  /// Down in the search field enters the grid (when there is one).
  private func key(window: Int, code: UInt16, flags: NSEvent.ModifierFlags) -> Bool {
    guard window == panel.windowNumber, state.shown, code == 125,  // kVK_DownArrow
          flags.intersection([.command, .option, .control, .shift]).isEmpty,
          let editor = panel.firstResponder as? NSTextView, editor.isFieldEditor,
          !model.results.isEmpty else { return false }
    state.gridSeq += 1
    return true
  }

  var isVisible: Bool { panel.isVisible && state.shown }

  func show(over parent: NSWindow?) {
    hideSeq += 1  // a hide still fading out does not take the panel away
    let wanted = CGSize(width: 820 + 2 * Self.margin, height: 580 + 2 * Self.margin)
    if let parent {
      let f = parent.frame
      let w = min(wanted.width, max(420, f.width - 24)), h = min(wanted.height, max(320, f.height - 40))
      // A little above centre, where the eye goes for a search box.
      panel.setFrame(NSRect(x: f.midX - w / 2, y: f.midY - h / 2 + min(40, f.height * 0.05), width: w, height: h),
                     display: false)
      if panel.parent !== parent {
        panel.parent?.removeChildWindow(panel)
        parent.addChildWindow(panel, ordered: .above)
      }
      parentWindow = parent
    } else {
      panel.center()
    }
    let wasShown = state.shown && panel.isVisible
    panel.makeKeyAndOrderFront(nil)
    if !wasShown {
      withAnimation(.spring(response: 0.32, dampingFraction: 0.86)) { state.shown = true }
      model.appeared()
    }
    state.focusSeq += 1
  }

  func hide() {
    guard state.shown else { return }
    withAnimation(.easeOut(duration: 0.14)) { state.shown = false }
    model.disappeared()
    hideSeq += 1
    let seq = hideSeq
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
      MainActor.assumeIsolated {
        guard let self, self.hideSeq == seq, !self.state.shown else { return }
        self.parentWindow?.removeChildWindow(self.panel)
        self.panel.orderOut(nil)
        self.parentWindow?.makeKeyAndOrderFront(nil)
      }
    }
  }

  func close() {
    hideSeq += 1
    state.shown = false
    model.disappeared()
    parentWindow?.removeChildWindow(panel)
    panel.orderOut(nil)
    if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
    keyMonitor = nil
    observers.forEach { NotificationCenter.default.removeObserver($0) }
    observers = []
  }
}

/// The material behind the card.
private struct PanelMaterial: NSViewRepresentable {
  func makeNSView(context: Context) -> NSVisualEffectView {
    let v = NSVisualEffectView()
    v.material = .popover
    v.blendingMode = .behindWindow
    v.state = .active
    return v
  }
  func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}

struct SearchRootView: View {
  @ObservedObject var model: SearchModel
  @ObservedObject var panel: PanelState
  let close: () -> Void

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @FocusState private var focus: Focus?
  @Namespace private var ring
  @State private var columns = 5
  @State private var scrollSeq = 0  // moves on a keyboard move: scroll to the selection

  enum Focus: Hashable { case field, grid }

  private static let top = "top"
  private let tile: CGFloat = 138
  private let spacing: CGFloat = 12

  var body: some View {
    VStack(spacing: 0) {
      header
      chips
      Rectangle().fill(AITheme.hairline).frame(height: 1)
      content
        .frame(maxWidth: .infinity, maxHeight: .infinity)
      Rectangle().fill(AITheme.hairline).frame(height: 1)
      footer
    }
    .background(PanelMaterial())
    .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
    .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(AITheme.hairline, lineWidth: 1))
    .shadow(color: .black.opacity(0.28), radius: 22, y: 10)
    .scaleEffect(panel.shown || reduceMotion ? 1 : 0.96)
    .opacity(panel.shown ? 1 : 0)
    .padding(SearchPanelController.margin)
    .onExitCommand {
      if focus == .grid && model.reference == nil { focus = .field } else { close() }
    }
    .onKeyPress(characters: ["f"]) { press in
      guard press.modifiers.contains(.command) else { return .ignored }
      focusField()
      return .handled
    }
    .onChange(of: panel.focusSeq) { _, _ in focusField() }
    .onChange(of: panel.gridSeq) { _, _ in if !model.results.isEmpty { focus = .grid } }
    // A reference chip stands in for the text: the (hidden) field gives up
    // the keyboard to the grid, so keys never go into an invisible field.
    .onChange(of: model.reference) { _, r in if r != nil { focusField() } }
    .onChange(of: model.results.isEmpty) { _, empty in
      if !empty, model.reference != nil, focus == nil { focus = .grid }
    }
    .onAppear { focusField() }
  }

  private func focusField() {
    if model.reference != nil {
      focus = model.results.isEmpty ? nil : .grid
      return
    }
    focus = .field
    // The previous query stays, selected, so typing replaces it.
    DispatchQueue.main.async {
      MainActor.assumeIsolated {
        _ = NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil)
      }
    }
  }

  // MARK: the field

  private var header: some View {
    HStack(spacing: 10) {
      Image(systemName: "magnifyingglass")
        .font(.system(size: 18, weight: .medium))
        .foregroundStyle(AITheme.body)
      if let reference = model.reference {
        HStack(spacing: 6) {
          Text(reference.label).font(AITheme.font(15)).foregroundStyle(AITheme.title).lineLimit(1)
          Button { model.clearReference(); focusField() } label: {
            Image(systemName: "xmark.circle.fill").foregroundStyle(AITheme.body)
          }
          .buttonStyle(.plain)
          .focusable()
          .onKeyPress(keys: [.space, .return]) { _ in
            model.clearReference()
            focusField()
            return .handled
          }
          .accessibilityLabel("Remove \(reference.label)")
        }
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(Capsule().fill(Color.accentColor.opacity(0.18)))
        .transition(.opacity.combined(with: .scale(scale: 0.94)))
        Spacer(minLength: 0)
      }
      TextField(model.reference == nil ? "Describe a photo or a moment — “dog on a beach”" : "",
                text: $model.query)
        .textFieldStyle(.plain)
        .font(AITheme.font(22))
        .foregroundStyle(AITheme.title)
        .focused($focus, equals: .field)
        .disabled(model.reference != nil)
        .opacity(model.reference == nil ? 1 : 0)
        .frame(maxWidth: model.reference == nil ? .infinity : 0)
        .onChange(of: model.query) { _, _ in model.queryChanged() }
        .onSubmit { open(gallery: NSEvent.modifierFlags.contains(.command)) }
        .accessibilityLabel("Search photos and videos")
      // Its room is kept while hidden, so the field does not change width.
      ProgressView().controlSize(.small)
        .opacity(model.searching ? 1 : 0)
        .animation(.easeOut(duration: 0.15), value: model.searching)
        .accessibilityHidden(!model.searching)
    }
    .padding(.horizontal, 18).padding(.top, 16).padding(.bottom, 10)
    .animation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.86), value: model.reference)
  }

  // MARK: scope and kind chips

  private var chips: some View {
    HStack(spacing: 6) {
      ForEach(SearchScope.allCases) { s in
        Chip(label: s.label, on: model.scope == s, disabled: s != .all && model.folder.isEmpty) {
          model.scope = s
          model.refreshCoverage()
          model.chipsChanged()
        }
      }
      Rectangle().fill(AITheme.hairline).frame(width: 1, height: 16).padding(.horizontal, 6)
      ForEach(SearchKinds.allCases) { k in
        Chip(label: k.label, on: model.kinds == k, disabled: false) {
          model.kinds = k
          model.chipsChanged()
        }
      }
      // 2026-09-27: what to find in them. None on = all three. Sounds and
      // Speech need the Sound piece (Settings -> Local search).
      Rectangle().fill(AITheme.hairline).frame(width: 1, height: 16).padding(.horizontal, 6)
      ForEach(SearchModel.Find.allCases) { f in
        let needsAudio = f != .pictures && !model.audioReady
        Chip(label: f.label, symbol: f.symbol, on: model.finds.contains(f), disabled: needsAudio) {
          if !model.finds.insert(f).inserted { model.finds.remove(f) }
          model.chipsChanged()
        }
        .help(needsAudio ? "Install Sound in Settings → Local search to find videos by what you hear."
                         : "Find by \(f.label.lowercased()). With none chosen, all are searched.")
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 16).padding(.bottom, 10)
  }

  // MARK: results or an empty state

  @ViewBuilder
  private var content: some View {
    if model.results.isEmpty {
      emptyState.transition(.opacity)
    } else {
      grid
    }
  }

  @ViewBuilder
  private var emptyState: some View {
    VStack(spacing: 12) {
      Spacer(minLength: 0)
      if model.coverage == 0 && model.scope != .all && !model.folder.isEmpty {
        Image(systemName: "rectangle.stack.badge.plus").font(.system(size: 34)).foregroundStyle(AITheme.body)
        Text("This folder is not indexed yet")
          .font(AITheme.font(17)).foregroundStyle(AITheme.title)
        Text("Indexing runs in the background, on this Mac, while you keep viewing. Results appear as it goes.")
          .font(AITheme.font(13)).foregroundStyle(AITheme.body)
          .multilineTextAlignment(.center).frame(maxWidth: 420)
        HStack(spacing: 10) {
          Button("Index this folder") { model.indexFolder(recursive: false) }
            .keyboardShortcut(.defaultAction)
          Button("Index this folder and subfolders") { model.indexFolder(recursive: true) }
        }
        .controlSize(.large)
      } else if model.searching {
        Text("Searching…").font(AITheme.font(14)).foregroundStyle(AITheme.body)
      } else if model.failed {
        Image(systemName: "exclamationmark.magnifyingglass").font(.system(size: 30)).foregroundStyle(AITheme.body)
        Text("Search did not finish.").font(AITheme.font(15)).foregroundStyle(AITheme.title)
        Text("Try again, or change the words.").font(AITheme.font(12)).foregroundStyle(AITheme.body)
      } else if model.finished && (model.reference != nil || !model.query.isEmpty) {
        Image(systemName: "sparkle.magnifyingglass").font(.system(size: 30)).foregroundStyle(AITheme.body)
        if let reference = model.reference {
          Text("Nothing close to \(reference.label.lowercased()) yet.")
            .font(AITheme.font(15)).foregroundStyle(AITheme.title)
        } else {
          Text("Nothing matches “\(model.query)”. Try describing what's in the picture: “dog on a beach”.")
            .font(AITheme.font(15)).foregroundStyle(AITheme.title)
            .multilineTextAlignment(.center).frame(maxWidth: 460)
        }
        if model.indexing {
          Text("Indexing is still running; more may match soon.")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
        }
      } else {
        Text("Describe what you're looking for: “guy on a skateboard”, “sunset over water”, “birthday cake”.")
          .font(AITheme.font(14)).foregroundStyle(AITheme.body)
          .multilineTextAlignment(.center).frame(maxWidth: 460)
        if model.indexing {
          Text("Results appear as the index grows.")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
        }
      }
      Spacer(minLength: 0)
    }
    .padding(24)
    .frame(maxWidth: .infinity)
  }

  private var grid: some View {
    GeometryReader { geo in
      let cols = max(1, Int((geo.size.width - 32 + spacing) / (tile + spacing)))
      ScrollViewReader { proxy in
        ScrollView {
          Color.clear.frame(height: 0).id(Self.top)
          LazyVGrid(columns: Array(repeating: GridItem(.fixed(tile), spacing: spacing), count: cols),
                    spacing: spacing) {
            ForEach(Array(model.results.enumerated()), id: \.element.id) { order, r in
              ResultTile(result: r, order: order, selected: order == model.selected && focus == .grid,
                         hinted: order == model.selected && focus != .grid,
                         ring: ring, slot: model.slot(for: r), size: tile)
                .id(r.id)  // stable across a re-run: a kept tile does not play its entrance again
                .onAppear { model.requestThumb(r) }
                .onDisappear { model.tileGone(r) }
                .onTapGesture(count: 2) {
                  model.selected = order
                  open(gallery: false)
                }
                .onTapGesture {
                  withAnimation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.82)) {
                    model.selected = order
                  }
                  focus = .grid
                }
            }
          }
          .padding(16)
          .id(model.resultsGeneration)  // a new result set starts its stagger again
        }
        // Only a keyboard move scrolls: a re-run as the index grows leaves
        // the scroll where the user put it.
        .onChange(of: scrollSeq) { _, _ in
          guard model.results.indices.contains(model.selected) else { return }
          let id = model.results[model.selected].id
          withAnimation(reduceMotion ? nil : .easeOut(duration: 0.18)) { proxy.scrollTo(id) }
        }
        .onChange(of: model.resultsGeneration) { _, _ in proxy.scrollTo(Self.top, anchor: .top) }
      }
      .onAppear { columns = cols }
      .onChange(of: cols) { _, c in columns = c }
    }
    .focusable()
    .focused($focus, equals: .grid)
    .focusEffectDisabled()
    .onKeyPress(keys: [.leftArrow, .rightArrow, .upArrow, .downArrow, .return]) { press in
      handleGridKey(press)
    }
    .accessibilityLabel("\(model.results.count) results")
  }

  private func handleGridKey(_ press: KeyPress) -> KeyPress.Result {
    let n = model.results.count
    guard n > 0 else { return .ignored }
    var i = model.selected
    switch press.key {
    case .return:
      open(gallery: press.modifiers.contains(.command))
      return .handled
    case .leftArrow: i -= 1
    case .rightArrow: i += 1
    case .downArrow: i += columns
    case .upArrow:
      if i - columns < 0 {
        if model.reference == nil { focus = .field }
        return .handled
      }
      i -= columns
    default:
      return .ignored
    }
    i = min(max(i, 0), n - 1)
    withAnimation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.82)) { model.selected = i }
    scrollSeq += 1
    return .handled
  }

  private func open(gallery: Bool) {
    _ = model.openResults(gallery: gallery)
  }

  // MARK: footer

  private var footer: some View {
    HStack(spacing: 12) {
      // Its own view over the status: 4 Hz updates re-render the pill only.
      SearchStatusPill(status: model.status) { model.setPaused($0) }
        .frame(maxWidth: 520, alignment: .leading)
      Spacer(minLength: 0)
      if !model.results.isEmpty {
        Text("↩ Open   ⌘↩ Gallery   esc Close")
          .font(AITheme.font(11)).foregroundStyle(AITheme.body)
          .lineLimit(1)
      }
    }
    .padding(.horizontal, 14).padding(.vertical, 9)
  }
}

private struct SearchStatusPill: View {
  @ObservedObject var status: SearchStatus
  let onPause: (Bool) -> Void

  var body: some View { StatusPill(line: status.line, onPause: onPause) }
}

private struct Chip: View {
  let label: String
  var symbol: String? = nil
  let on: Bool
  let disabled: Bool
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      HStack(spacing: 4) {
        if let symbol { Image(systemName: symbol).font(.system(size: 10, weight: .semibold)) }
        Text(label)
      }
        .font(AITheme.font(12))
        .foregroundStyle(on ? Color.white : AITheme.title)
        .padding(.horizontal, 10).padding(.vertical, 4)
        .background(Capsule().fill(on ? Color.accentColor : Color.primary.opacity(hover ? 0.10 : 0.06)))
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .disabled(disabled)
    // Reachable with Tab, and Space or Return toggles it.
    .focusable(!disabled)
    .onKeyPress(keys: [.space, .return]) { _ in
      action()
      return .handled
    }
    .opacity(disabled ? 0.45 : 1)
    .onHover { hover = $0 }
    .accessibilityAddTraits(on ? .isSelected : [])
    .animation(.easeOut(duration: 0.15), value: on)
  }
}

/// One result: the moment's thumbnail, a m:ss badge for a clip, "+N in this
/// clip" when the clip has other matches, what matched (picture, sound, speech)
/// and, for speech, the words that were said.
private struct ResultTile: View {
  let result: AIResult
  let order: Int
  let selected: Bool
  let hinted: Bool         // the tile Return would open while the field has focus
  let ring: Namespace.ID
  @ObservedObject var slot: ImageSlot
  let size: CGFloat

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var appeared: Bool
  @State private var hover = false

  init(result: AIResult, order: Int, selected: Bool, hinted: Bool, ring: Namespace.ID,
       slot: ImageSlot, size: CGFloat) {
    self.result = result
    self.order = order
    self.selected = selected
    self.hinted = hinted
    self.ring = ring
    _slot = ObservedObject(wrappedValue: slot)
    self.size = size
    // The slot remembers the entrance: scrolled back into view, it is just there.
    _appeared = State(initialValue: slot.appeared)
  }

  private var badges: [String] {
    var out: [String] = []
    // The picture badge only beside another: a plain picture match needs none.
    if result.matchedPicture && (result.matchedSound || result.matchedSpeech) { out.append("photo") }
    if result.matchedSound { out.append("speaker.wave.2") }
    if result.matchedSpeech { out.append("text.bubble") }
    return out
  }

  private var picture: some View {
    let h = size * 0.75
    return ZStack {
      RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.primary.opacity(0.07))
      if let image = slot.image {
        Image(decorative: image, scale: 1)
          .resizable()
          .aspectRatio(contentMode: .fill)
          .frame(width: size, height: h)
          .clipped()
          .transition(.opacity)
      } else {
        Image(systemName: result.isClip ? "film" : "photo").foregroundStyle(AITheme.body)
      }
    }
    .frame(width: size, height: h)
    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
    .overlay(alignment: .topLeading) {
      // Only when something other than the picture matched: a sound or words.
      if !badges.isEmpty {
        HStack(spacing: 3) {
          ForEach(badges, id: \.self) { Image(systemName: $0) }
        }
        .font(.system(size: 10, weight: .semibold))
        .foregroundStyle(.white)
        .padding(.horizontal, 5).padding(.vertical, 3)
        .background(Capsule().fill(.black.opacity(0.6)))
        .padding(5)
      }
    }
    .overlay(alignment: .bottomLeading) {
      if result.isClip {
        Label(momentText(result.ptsMs), systemImage: "play.fill")
          .labelStyle(.titleAndIcon)
          .font(.system(size: 10, weight: .semibold).monospacedDigit())
          .foregroundStyle(.white)
          .padding(.horizontal, 5).padding(.vertical, 2)
          .background(Capsule().fill(.black.opacity(0.6)))
          .padding(5)
      }
    }
    .overlay(alignment: .bottomTrailing) {
      if result.more > 0 {
        Text("+\(result.more) in this clip")
          .font(.system(size: 10, weight: .medium))
          .foregroundStyle(.white)
          .padding(.horizontal, 5).padding(.vertical, 2)
          .background(Capsule().fill(.black.opacity(0.6)))
          .padding(5)
      }
    }
    .overlay {
      if selected {
        RoundedRectangle(cornerRadius: 10, style: .continuous)
          .stroke(Color.accentColor, lineWidth: 2.5)
          .padding(-3)
          .matchedGeometryEffect(id: "selection", in: ring)
      } else if hinted {
        RoundedRectangle(cornerRadius: 10, style: .continuous)
          .stroke(Color.accentColor.opacity(0.45), lineWidth: 1.5)
          .padding(-3)
      }
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      picture
      if !slot.snippet.isEmpty {
        // The words that matched, quoted, under the frame they were said over.
        Text("“\(slot.snippet)”")
          .font(AITheme.font(11))
          .foregroundStyle(AITheme.body)
          .lineLimit(2)
          .truncationMode(.middle)
          .frame(width: size, alignment: .leading)
          .transition(.opacity)
      }
    }
    .frame(width: size, alignment: .topLeading)
    .scaleEffect(hover && !reduceMotion ? 1.03 : 1)
    .shadow(color: .black.opacity(hover ? 0.25 : 0), radius: hover ? 10 : 0, y: hover ? 4 : 0)
    .animation(.easeOut(duration: 0.15), value: hover)
    .opacity(appeared ? 1 : 0)
    .offset(y: appeared || reduceMotion ? 0 : 8)
    .onHover { hover = $0 }
    .onAppear {
      guard !appeared else { return }
      slot.appeared = true
      // Staggered ~25 ms apart, the first 16 only; the rest just appear.
      let delay = order < 16 ? Double(order) * 0.025 : 0
      withAnimation(.easeOut(duration: reduceMotion ? 0.15 : 0.22).delay(delay)) { appeared = true }
    }
    .help(result.name)
    .accessibilityElement()
    .accessibilityLabel(accessibilityText)
    .accessibilityAddTraits(selected ? .isSelected : [])
  }

  private var accessibilityText: String {
    var text = result.isClip ? "\(result.name), at \(momentText(result.ptsMs))" : result.name
    if result.matchedSound { text += ", matched by sound" }
    if result.matchedSpeech { text += ", matched by speech" + (slot.snippet.isEmpty ? "" : ": \(slot.snippet)") }
    return text
  }
}
