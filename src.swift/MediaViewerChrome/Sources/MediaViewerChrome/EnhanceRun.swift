// SPDX-License-Identifier: GPL-3.0-or-later
// One-click Enhance (owner 2026-11-05, docs/design/29 "One click"): Enhance's
// "tool" screen (place "edit") is the Enhance tab's cards. Its tree's root is
// "cards": one stack "card.<word>" per tool, drawn inside that tool's card
// (EnhanceCard), and a hidden "run" node that says what the run is doing.
// This reads them, tells native the run's state (the glow over the photo, the
// tool bar's band, the result opened once: mv_chrome_edit_enhance_run), and
// draws the run's line in the tool bar under the photo (EnhanceRunRow,
// "Enhancing… 40 % · 1 min left · Cancel", then "Saved as … · Move to Trash").
// The Windows twin is IslandHost.EnhanceRun.cs.
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class EnhanceRun: ObservableObject {
  static let shared = EnhanceRun()
  static let address = ScreenAddress(addon: "enhance", screen: "tool")

  /// The cards' tree when this Enhance draws its tools as cards (nil: closed,
  /// or an Enhance from before one-click).
  @Published private(set) var cards: ScreenNode?
  @Published private(set) var revision = 0
  /// The cards screen is open but is not cards: an Enhance to update.
  @Published private(set) var outdated = false
  @Published private(set) var state = "idle"
  @Published private(set) var line = ""
  @Published private(set) var fraction: Double = -1
  @Published private(set) var tool = ""
  @Published private(set) var canDiscard = false
  @Published private(set) var discardLabel = ""
  private var reported: (String, UInt64, String) = ("idle", 0, "")
  private var watch: AnyCancellable?

  private init() {
    watch = SlotStore.shared.$screens.sink { [weak self] in self?.read($0) }
  }

  /// A run shows a line in the tool bar (working, its result, or why not).
  var active: Bool { state != "idle" }
  var working: Bool { state == "running" || state == "downloading" }

  /// The card's own stack, `card.<word>`, when this Enhance has that tool.
  func card(_ word: String) -> ScreenNode? {
    cards?.children.first { $0.id == "card." + word }
  }

  func cancel() { SlotStore.shared.send(Self.address, node: "cancel", kind: 1, value: nil) }
  func dismiss() { SlotStore.shared.send(Self.address, node: "run.dismiss", kind: 1, value: nil) }
  func discard() { SlotStore.shared.send(Self.address, node: "result.discard", kind: 1, value: nil) }

  private func read(_ screens: [SlotScreen]) {
    let screen = screens.first { $0.addon == Self.address.addon && $0.screen == Self.address.screen && $0.place == "edit" }
    guard let screen, let tree = screen.tree else {
      if cards != nil { cards = nil }
      outdated = false
      apply(state: "idle", id: 0, result: "", line: "", fraction: -1, tool: "", discard: nil)
      return
    }
    guard tree.id == "cards" else {
      cards = nil
      outdated = true
      return
    }
    outdated = false
    cards = tree
    revision = screen.revision
    let run = Self.find("run", in: tree)
    let state = run?.label ?? "idle"
    let id = UInt64(Self.find("run.id", in: tree)?.string("text") ?? "") ?? 0
    let result = Self.find("run.result", in: tree)?.string("path") ?? ""
    apply(state: state, id: id, result: state == "done" ? result : "",
          line: Self.find("run.line", in: tree)?.string("text") ?? "",
          fraction: Self.find("run.progress", in: tree)?.double("value") ?? -1,
          tool: Self.find("run.tool", in: tree)?.string("text") ?? "",
          discard: Self.find("result.discard", in: tree))
  }

  private func apply(state: String, id: UInt64, result: String, line: String, fraction: Double, tool: String,
                     discard: ScreenNode?) {
    if self.state != state { self.state = state }
    if self.line != line { self.line = line }
    if self.fraction != fraction { self.fraction = fraction }
    if self.tool != tool { self.tool = tool }
    canDiscard = discard != nil
    discardLabel = discard?.label ?? ""
    guard reported != (state, id, result) else { return }
    reported = (state, id, result)
    let code: Int32
    switch state {
    case "running": code = 1
    case "done": code = 2
    case "failed": code = 3
    case "cancelled": code = 4
    case "downloading": code = 5
    default: code = 0
    }
    mv_chrome_edit_enhance_run(code, id, result)
  }

  static func find(_ id: String, in node: ScreenNode) -> ScreenNode? {
    if node.id == id { return node }
    for child in node.children {
      if let hit = find(id, in: child) { return hit }
    }
    return nil
  }
}

/// The tool bar's line while a run shows: what it is doing and how far, with
/// Cancel; then its result (Move to Trash) or why it stopped (Dismiss).
struct EnhanceRunRow: View {
  @ObservedObject private var run = EnhanceRun.shared

  var body: some View {
    HStack(spacing: MVTheme.space(10)) {
      if run.working {
        if run.fraction >= 0 {
          ProgressView(value: min(1, max(0, run.fraction)))
            .progressViewStyle(.linear)
            .frame(width: 120)
        } else {
          ProgressView().controlSize(.small)
        }
      }
      Text(run.line.isEmpty ? "Working…" : run.line)
        .font(MVTheme.font(13).monospacedDigit())
        .foregroundStyle(run.state == "failed" ? MVTheme.warm : MVTheme.body)
        .lineLimit(1)
        .truncationMode(.middle)
        .help(run.line)
      if run.working {
        Button("Cancel") { run.cancel() }
          .buttonStyle(FlatButtonStyle(compact: true))
          .keyboardShortcut(.cancelAction)
      } else {
        if run.canDiscard {
          Button(run.discardLabel) { run.discard() }
            .buttonStyle(FlatButtonStyle(compact: true))
            .help("Moves the new file to the Trash. The original is never touched.")
        }
        Button { run.dismiss() } label: { Image(systemName: "xmark") }
          .buttonStyle(FlatButtonStyle(compact: true))
          .help("Dismiss")
          .accessibilityLabel("Dismiss")
      }
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(run.line)
  }
}
