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
// mouse"): typing searches; Return or Down enters the grid (Return on words
// still being searched, as while indexing, waits for their answer and enters
// it then; nothing found stays in the field); arrows move; Return in the grid
// opens the results in the viewer on the chosen tile; Cmd+Return opens them as
// the gallery grid; typing in the grid goes back to the field; Esc goes from
// the grid back to the field, then closes. Down and Esc go
// through a local key monitor: the field editor would take moveDown: before a
// SwiftUI key handler on the TextField sees it, and SwiftUI's exit command
// needs a focused view (after a click on a button there is none).
//
// Never in the way (2026-09-27): Esc, the footer's Close, or a click on the
// viewer closes it at any point, indexing included; indexing and searches run
// on without it, the command bar's pill shows the progress, and reopening
// shows the results that arrived meanwhile.
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
  /// Moves on Esc anywhere in the panel (the key monitor): SwiftUI's exit
  /// command needs a focused view, and after a click on a button there is none.
  @Published var escSeq = 0
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
  private var parentKeyObserver: NSObjectProtocol?
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

  /// Down in the search field enters the grid (when there is one); Esc
  /// anywhere in the panel steps back or closes (never a modal: indexing and
  /// searching carry on without it).
  private func key(window: Int, code: UInt16, flags: NSEvent.ModifierFlags) -> Bool {
    guard window == panel.windowNumber, state.shown,
          flags.intersection([.command, .option, .control, .shift]).isEmpty else { return false }
    let editor = panel.firstResponder as? NSTextView
    if code == 53 {  // kVK_Escape
      // An input method's composition takes its own Esc.
      if let editor, editor.hasMarkedText() { return false }
      state.escSeq += 1
      return true
    }
    if code == 48 {  // kVK_Tab: a person's name for the word being typed
      guard let editor, editor.isFieldEditor, !editor.hasMarkedText(), model.acceptSuggestion() else { return false }
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          _ = NSApp.sendAction(#selector(NSResponder.moveToEndOfDocument(_:)), to: nil, from: nil)
        }
      }
      return true
    }
    guard code == 125,  // kVK_DownArrow
          let editor, editor.isFieldEditor, !model.results.isEmpty else { return false }
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
      if parentWindow !== parent || parentKeyObserver == nil {
        // A click on the viewer steps the panel aside (the Windows panel does
        // the same): it never stands between the user and the viewer.
        if let parentKeyObserver { NotificationCenter.default.removeObserver(parentKeyObserver) }
        parentKeyObserver = NotificationCenter.default.addObserver(
          forName: NSWindow.didBecomeKeyNotification, object: parent, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
              guard let self, self.state.shown, self.panel.isVisible else { return }
              self.hide()
            }
          }
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
    if let parentKeyObserver { NotificationCenter.default.removeObserver(parentKeyObserver) }
    parentKeyObserver = nil
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
      if !model.suggestions.isEmpty && model.reference == nil {
        suggestionRow
      }
      chips
      Rectangle().fill(AITheme.hairline).frame(height: 1)
      content
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(alignment: .bottom) { preparingBanner }
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
    .onChange(of: panel.escSeq) { _, _ in
      if model.preparing != nil { model.cancelPreparing() }  // stop opening, stay here
      else if focus == .grid && model.reference == nil { focus = .field } else { close() }
    }
    .onKeyPress(characters: ["f"]) { press in
      guard press.modifiers.contains(.command) else { return .ignored }
      focusField()
      return .handled
    }
    .onChange(of: panel.focusSeq) { _, _ in focusField() }
    .onChange(of: panel.gridSeq) { _, _ in if !model.results.isEmpty { focus = .grid } }
    // Return in the field before its answer: the answer landed with results.
    // Only from the field (a click elsewhere since means the user moved on).
    .onChange(of: model.focusResultsSeq) { _, _ in
      guard focus == .field, !model.results.isEmpty else { return }
      model.selected = 0
      focus = .grid
      scrollSeq += 1
    }
    // A reference chip stands in for the text: the (hidden) field gives up
    // the keyboard to the grid, so keys never go into an invisible field.
    .onChange(of: model.reference) { _, r in if r != nil { focusField() } }
    .onChange(of: model.results.isEmpty) { _, empty in
      if !empty, model.reference != nil, focus == nil { focus = .grid }
      // A re-run found nothing under the grid the user was in: the keyboard
      // goes back to the words, not nowhere.
      if empty, focus == .grid, model.reference == nil { focusFieldAtEnd() }
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

  /// The field, caret after the words (not selected): typing carries on.
  private func focusFieldAtEnd() {
    focus = .field
    DispatchQueue.main.async {
      MainActor.assumeIsolated {
        _ = NSApp.sendAction(#selector(NSResponder.moveToEndOfDocument(_:)), to: nil, from: nil)
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
      TextField(model.reference == nil ? "Describe a photo or a moment, or name someone" : "",
                text: $model.query)
        .textFieldStyle(.plain)
        .font(AITheme.font(22))
        .foregroundStyle(AITheme.title)
        .focused($focus, equals: .field)
        .disabled(model.reference != nil)
        .opacity(model.reference == nil ? 1 : 0)
        .frame(maxWidth: model.reference == nil ? .infinity : 0)
        .onChange(of: model.query) { _, _ in model.queryChanged() }
        .onSubmit {
          // Return goes to the results (Down does the same); Cmd+Return opens
          // them all as the gallery grid.
          if NSEvent.modifierFlags.contains(.command) {
            open(gallery: true)
          } else if model.submitToResults() {
            focus = .grid
            scrollSeq += 1
          }
        }
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

  // MARK: people while typing

  /// "Trist" → Tristan: Tab (or a click) completes the word, so results for
  /// the person follow. Only while a word is being typed (SearchModel).
  private var suggestionRow: some View {
    HStack(spacing: 6) {
      Image(systemName: "person.crop.circle")
        .foregroundStyle(AITheme.body)
        .accessibilityHidden(true)
      ForEach(Array(model.suggestions.enumerated()), id: \.element.id) { i, s in
        Button {
          model.acceptSuggestion(s)
          focusField()
        } label: {
          HStack(spacing: 5) {
            Text(s.name).font(AITheme.font(13)).foregroundStyle(AITheme.title)
            if i == 0 {
              Text("Tab").font(AITheme.font(11)).foregroundStyle(AITheme.body)
                .padding(.horizontal, 4).padding(.vertical, 1)
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(AITheme.hairline, lineWidth: 1))
            }
          }
          .padding(.horizontal, 9).padding(.vertical, 4)
          .background(Capsule().fill(Color.accentColor.opacity(i == 0 ? 0.18 : 0.08)))
        }
        .buttonStyle(.plain)
        .help("Search for \(s.name)")
        .accessibilityLabel("Person: \(s.name)")
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 18).padding(.bottom, 10)
    .transition(.opacity)
  }

  // MARK: scope and kind chips

  // Three groups, each captioned with what it does: where to look (one
  // choice), what to show (one choice), and what to match by (any of the
  // three, all on at rest). One row where it fits, else two; the captions go
  // before the controls do.
  private var chips: some View {
    ViewThatFits(in: .horizontal) {
      HStack(spacing: 18) {
        scopeGroup(captioned: true)
        kindGroup(captioned: true)
        matchGroup(captioned: true)
        Spacer(minLength: 0)
      }
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 18) {
          scopeGroup(captioned: true)
          kindGroup(captioned: true)
          Spacer(minLength: 0)
        }
        HStack { matchGroup(captioned: true); Spacer(minLength: 0) }
      }
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 12) { scopeGroup(captioned: false); Spacer(minLength: 0) }
        HStack(spacing: 12) {
          kindGroup(captioned: false)
          matchGroup(captioned: false)
          Spacer(minLength: 0)
        }
      }
    }
    .padding(.horizontal, 18).padding(.bottom, 12)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: model.audioReady)
  }

  private func scopeGroup(captioned: Bool) -> some View {
    FilterGroup(caption: captioned ? "Look in" : nil) {
      SegmentTrack {
        // "Photos" only once the Photos library is indexed (Settings).
        ForEach(SearchScope.allCases.filter { $0 != .photos || model.photosIndexed }) { s in
          let needsFolder = (s == .folder || s == .tree) && model.folder.isEmpty
          FilterButton(label: s.label, on: model.scope == s, style: .segment,
                       available: !needsFolder,
                       help: needsFolder ? "Open a folder to search just that folder." : s.help) {
            model.scope = s
            model.refreshCoverage()
            model.chipsChanged()
          }
        }
      }
    }
  }

  private func kindGroup(captioned: Bool) -> some View {
    FilterGroup(caption: captioned ? "Show" : nil) {
      SegmentTrack {
        ForEach(SearchKinds.allCases) { k in
          FilterButton(label: k.label, on: model.kinds == k, style: .segment, help: k.help) {
            model.kinds = k
            model.chipsChanged()
          }
        }
      }
    }
  }

  // 2026-09-27: what the words are matched against. Sound and Speech need
  // the Sound piece (Settings → Local search); without it they stay visible,
  // dimmed, and their tooltip says how to get them.
  private func matchGroup(captioned: Bool) -> some View {
    FilterGroup(caption: captioned ? "Match by" : nil) {
      HStack(spacing: 6) {
        ForEach(SearchModel.Find.allCases) { f in
          let available = model.availableFinds.contains(f)
          FilterButton(label: f.label, symbol: f.symbol, on: model.matchOn(f), style: .token,
                       available: available,
                       help: available
                         ? f.help + (model.matchOn(f) ? "" : " Off: click to include it.")
                         : "Needs the Sound piece: install it in Settings → Local search to find videos by what you hear.") {
            model.toggleMatch(f)
          }
        }
      }
    }
  }

  /// Opening Photos results: each becomes a file first (usually milliseconds
  /// each, so this shows only for a long list).
  @ViewBuilder
  private var preparingBanner: some View {
    if let p = model.preparing {
      HStack(spacing: 10) {
        ProgressView(value: p.total == 0 ? 0 : Double(p.done) / Double(p.total))
          .progressViewStyle(.linear).frame(width: 120)
        Text("Getting \(p.total == 1 ? "the photo" : "\(p.total) items") from your Photos library…")
          .font(AITheme.font(12)).foregroundStyle(AITheme.title)
        Button("Cancel") { model.cancelPreparing() }.controlSize(.small)
      }
      .padding(.horizontal, 14).padding(.vertical, 8)
      .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(.regularMaterial))
      .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).stroke(AITheme.hairline, lineWidth: 1))
      .padding(.bottom, 12)
      .transition(.opacity.combined(with: .move(edge: .bottom)))
      .accessibilityElement(children: .combine)
    }
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
      if let recursive = model.startedIndexing, !model.searching {
        // Just asked to index: say plainly that it carries on without the
        // panel, and offer the way out.
        Image(systemName: "arrow.triangle.2.circlepath").font(.system(size: 30)).foregroundStyle(AITheme.body)
        Text(recursive ? "Indexing “\(model.folderName)” and its subfolders in the background"
                       : "Indexing “\(model.folderName)” in the background")
          .font(AITheme.font(17)).foregroundStyle(AITheme.title)
          .multilineTextAlignment(.center).frame(maxWidth: 480)
        Text("You can close this and carry on: indexing continues on its own, at low priority, and "
             + "pauses while you watch or pan. Its progress is in the command bar. "
             + (model.query.isEmpty ? "Search whenever you like: results appear as it goes."
                                    : "Results for “\(model.query)” appear here as it goes."))
          .font(AITheme.font(13)).foregroundStyle(AITheme.body)
          .multilineTextAlignment(.center).frame(maxWidth: 460)
          .fixedSize(horizontal: false, vertical: true)
        HStack(spacing: 10) {
          Button("Continue in background") { close() }  // Esc does the same
            .help("Close this panel. Indexing carries on; reopen search (⌘F) any time.")
          Button("Search now") { focusField() }
        }
        .controlSize(.large)
        .padding(.top, 4)
      } else if model.coverage == 0 && model.scope != .all && !model.folder.isEmpty {
        Image(systemName: "rectangle.stack.badge.plus").font(.system(size: 34)).foregroundStyle(AITheme.body)
        Text("“\(model.folderName)” is not indexed yet")
          .font(AITheme.font(17)).foregroundStyle(AITheme.title)
          .multilineTextAlignment(.center).frame(maxWidth: 480)
        Text("Indexing runs in the background, on this Mac: you can close this panel and keep viewing. "
             + "Results appear as it goes.")
          .font(AITheme.font(13)).foregroundStyle(AITheme.body)
          .multilineTextAlignment(.center).frame(maxWidth: 440)
          .fixedSize(horizontal: false, vertical: true)
        HStack(spacing: 10) {
          Button("Index this folder") { model.indexFolder(recursive: false) }
            .keyboardShortcut(.defaultAction)
          Button("Index this folder and subfolders") { model.indexFolder(recursive: true) }
        }
        .controlSize(.large)
        .padding(.top, 4)
      } else if model.searching {
        Text("Searching…").font(AITheme.font(14)).foregroundStyle(AITheme.body)
      } else if model.failed {
        Image(systemName: "exclamationmark.magnifyingglass").font(.system(size: 30)).foregroundStyle(AITheme.body)
        Text("Search did not finish.").font(AITheme.font(15)).foregroundStyle(AITheme.title)
        Text("Try again, or change the words.").font(AITheme.font(13)).foregroundStyle(AITheme.body)
      } else if model.finished && (model.reference != nil || !model.query.isEmpty) {
        Image(systemName: "sparkle.magnifyingglass").font(.system(size: 30)).foregroundStyle(AITheme.body)
        if let reference = model.reference {
          Text("Nothing close to \(reference.label.lowercased()) yet.")
            .font(AITheme.font(15)).foregroundStyle(AITheme.title)
        } else {
          Text("Nothing matches “\(model.query)”. Try fewer words, or describe what's in the picture.")
            .font(AITheme.font(15)).foregroundStyle(AITheme.title)
            .multilineTextAlignment(.center).frame(maxWidth: 460)
          SyntaxHint()
        }
        if model.indexing {
          Text("Indexing is still running; more may match soon.")
            .font(AITheme.font(13)).foregroundStyle(AITheme.body)
        }
      } else {
        Text("Describe what you're looking for: “guy on a skateboard”, “sunset over water”, “birthday cake”."
             + (model.audioReady ? " Or a sound, “dog barking”, or words someone said." : ""))
          .font(AITheme.font(14)).foregroundStyle(AITheme.body)
          .multilineTextAlignment(.center).frame(maxWidth: 460)
        SyntaxHint()
        if model.indexing {
          Text("Results appear as the index grows.")
            .font(AITheme.font(13)).foregroundStyle(AITheme.body)
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
                // Drag the original out (Final Cut Pro, Finder): the file, never
                // the moment; a clip result carries the whole clip.
                .fileDrag { [(path: r.path, image: model.slot(for: r).image)] }
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
    .onKeyPress(phases: .down) { press in typeIntoField(press) }
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

  /// A printable key in the grid goes back to the words: the field takes it
  /// at the end and searches as typing always does.
  private func typeIntoField(_ press: KeyPress) -> KeyPress.Result {
    guard model.reference == nil, press.modifiers.isDisjoint(with: [.command, .control]),
          !press.characters.isEmpty,
          press.characters.unicodeScalars.allSatisfy({ s in
            !CharacterSet.controlCharacters.contains(s) && !(0xF700...0xF8FF).contains(s.value)  // arrows, F-keys
          }) else { return .ignored }
    model.query += press.characters
    focusFieldAtEnd()
    return .handled
  }

  private func open(gallery: Bool) {
    _ = model.openResults(gallery: gallery)
  }

  // MARK: footer

  private var footer: some View {
    HStack(spacing: 14) {
      // Its own view over the status: 4 Hz updates re-render the pill only.
      SearchStatusPill(status: model.status, onIndexAnyway: { model.indexAnyway() }) { model.setPaused($0) }
        .frame(maxWidth: 520, alignment: .leading)
      Spacer(minLength: 0)
      if !model.results.isEmpty {
        HStack(spacing: 10) {
          // What Return does where the keyboard is.
          KeyHint(key: "↩", label: focus == .grid ? "Open" : "Go to results")
          KeyHint(key: "⌘↩", label: "Open all")
        }
        .fixedSize()
      }
      // Always there: closing never stops indexing or a search.
      Button { close() } label: { KeyHint(key: "esc", label: "Close") }
        .buttonStyle(.plain)
        .fixedSize()
        .help("Close the panel. Indexing carries on in the background.")
        .accessibilityLabel("Close search")
    }
    .padding(.horizontal, 14).padding(.vertical, 9)
  }
}

/// "↩ Open": a keycap and what it does, in the footer.
private struct KeyHint: View {
  let key: String
  let label: String

  var body: some View {
    HStack(spacing: 5) {
      Text(key)
        .font(AITheme.font(12))
        .foregroundStyle(AITheme.title)
        .padding(.horizontal, 5)
        .frame(minWidth: 20, minHeight: 18)
        .background(RoundedRectangle(cornerRadius: 4, style: .continuous).fill(Color.primary.opacity(0.07)))
        .overlay(RoundedRectangle(cornerRadius: 4, style: .continuous).strokeBorder(AITheme.hairline, lineWidth: 1))
      Text(label).font(AITheme.font(13)).foregroundStyle(AITheme.body)
    }
    .fixedSize()
    .accessibilityElement(children: .combine)
  }
}

private struct SearchStatusPill: View {
  @ObservedObject var status: SearchStatus
  let onIndexAnyway: () -> Void
  let onPause: (Bool) -> Void

  var body: some View { StatusPill(line: status.line, onIndexAnyway: onIndexAnyway, onPause: onPause) }
}

// MARK: query syntax

/// The query language in one line (plan/17 "Query syntax"): the pack parses
/// it, so the Windows panel shows the same examples.
private struct SyntaxHint: View {
  var body: some View {
    VStack(spacing: 4) {
      Text("Sam beach   ·   Sam “happy birthday”   ·   Sam or Alex   ·   -video   ·   in:2024")
        .font(.system(size: 12, design: .monospaced))
        .foregroundStyle(AITheme.title.opacity(0.8))
      Text("A name finds that person; “quotes” find words said in videos; - leaves something out; "
           + "video or photo picks a kind; in:, before: and after: use the file's date; file: matches its name.")
        .font(AITheme.font(12))
        .foregroundStyle(AITheme.body)
        .multilineTextAlignment(.center)
        .fixedSize(horizontal: false, vertical: true)
    }
    .frame(maxWidth: 520)
    .padding(.top, 6)
    .accessibilityElement(children: .combine)
  }
}

// MARK: filter controls

/// A caption and its control, on one baseline: "Look in  [This folder | …]".
private struct FilterGroup<Content: View>: View {
  let caption: String?
  @ViewBuilder let content: Content

  var body: some View {
    HStack(spacing: 8) {
      if let caption {
        Text(caption)
          .font(AITheme.font(13))
          .foregroundStyle(AITheme.body)
          .fixedSize()
          .accessibilityHidden(true)  // each control's own label carries it
      }
      content
    }
    .fixedSize()
  }
}

/// The recessed track a single-choice group sits in (a segmented control).
private struct SegmentTrack<Content: View>: View {
  @ViewBuilder let content: Content
  @Environment(\.colorSchemeContrast) private var contrast

  var body: some View {
    HStack(spacing: 2) { content }
      .padding(2)
      .background(RoundedRectangle(cornerRadius: 7, style: .continuous).fill(Color.primary.opacity(0.06)))
      .overlay {
        RoundedRectangle(cornerRadius: 7, style: .continuous)
          .stroke(contrast == .increased ? AITheme.title.opacity(0.6) : AITheme.hairline.opacity(0.6), lineWidth: 1)
      }
  }
}

/// One filter: a segment of a single-choice track, or a token that toggles
/// on its own (Finder / Photos filter tokens). House face at its 13 pt design
/// size; icons 11 pt, centred on the text. Selected reads by fill, weight of
/// colour and (Increase Contrast) an outline, never by colour alone.
/// Unavailable is dimmed but still hovers, so its tooltip can say why (a
/// disabled control shows no tooltip); clicking it does nothing.
private struct FilterButton: View {
  enum Style { case segment, token }

  let label: String
  var symbol: String? = nil
  let on: Bool
  let style: Style
  var available = true
  let help: String
  let action: () -> Void

  @Environment(\.colorScheme) private var scheme
  @Environment(\.colorSchemeContrast) private var contrast
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @FocusState private var focused: Bool
  @State private var hover = false

  private var increased: Bool { contrast == .increased }
  private var radius: CGFloat { style == .segment ? 5 : 12 }

  var body: some View {
    HStack(spacing: 5) {
      if let symbol {
        Image(systemName: symbol)
          .font(.system(size: 11, weight: .medium))
          .frame(width: 14)
      }
      Text(label).font(AITheme.font(13)).fixedSize()
    }
    .foregroundStyle(foreground)
    .padding(.horizontal, 10)
    .frame(height: style == .segment ? 22 : 24)
    .background { RoundedRectangle(cornerRadius: radius, style: .continuous).fill(fill) }
    .overlay { RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(stroke, lineWidth: 1) }
    .shadow(color: style == .segment && on && scheme == .light ? .black.opacity(0.12) : .clear, radius: 1, y: 0.5)
    .overlay {
      // The keyboard focus ring, drawn: a custom control gets none of its own.
      if focused {
        RoundedRectangle(cornerRadius: radius + 2, style: .continuous)
          .stroke(Color(nsColor: .keyboardFocusIndicatorColor), lineWidth: 2.5)
          .padding(-2)
      }
    }
    .opacity(available ? 1 : 0.4)
    .contentShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
    .onTapGesture { if available { action() } }
    .onHover { hover = $0 }
    // Reachable with Tab when it can do something; Space or Return acts.
    .focusable(available)
    .focused($focused)
    .focusEffectDisabled()
    .onKeyPress(keys: [.space, .return]) { _ in
      guard available else { return .ignored }
      action()
      return .handled
    }
    .help(help)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.12), value: on)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.12), value: hover)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(label)
    .accessibilityHint(help)
    .accessibilityAddTraits(on ? [.isButton, .isSelected] : .isButton)
    .accessibilityAction { if available { action() } }
  }

  private var foreground: Color {
    switch style {
    case .segment: return on || hover ? AITheme.title : AITheme.body
    case .token:
      if !on { return hover && available ? AITheme.title : AITheme.body }
      return increased ? AITheme.title : Color.accentColor
    }
  }

  private var fill: Color {
    switch style {
    case .segment:
      if on { return scheme == .dark ? Color.white.opacity(0.16) : Color.white }
      return hover ? Color.primary.opacity(0.05) : .clear
    case .token:
      if on { return Color.accentColor.opacity(scheme == .dark ? 0.24 : 0.14) }
      return hover && available ? Color.primary.opacity(0.06) : .clear
    }
  }

  private var stroke: Color {
    switch style {
    case .segment:
      return on && increased ? AITheme.title.opacity(0.7) : .clear
    case .token:
      if on { return increased ? AITheme.title : Color.accentColor.opacity(0.55) }
      return increased ? AITheme.title.opacity(0.6) : AITheme.hairline
    }
  }
}

/// One result: the moment's thumbnail, a m:ss badge for a clip, "+N in this
/// clip" when the clip has other matches, what matched (picture, sound, speech)
/// and, for speech, the words that were said.
private struct ResultTile: View {
  let result: AIResult
  let order: Int
  let selected: Bool
  let hinted: Bool         // the tile the grid lands on while the field has focus
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
    .overlay(alignment: .topTrailing) {
      // From the Photos library, not a folder (issue #72): opens as a copy.
      if result.isPhotos {
        Image(systemName: "photo.on.rectangle.angled")
          .font(.system(size: 10, weight: .semibold))
          .foregroundStyle(.white)
          .padding(4)
          .background(Circle().fill(.black.opacity(0.6)))
          .padding(5)
          .help("From your Photos library")
          .accessibilityLabel("From your Photos library")
      }
    }
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
          .font(AITheme.font(12))
          .foregroundStyle(AITheme.body)
          .lineLimit(2)
          .truncationMode(.tail)
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
