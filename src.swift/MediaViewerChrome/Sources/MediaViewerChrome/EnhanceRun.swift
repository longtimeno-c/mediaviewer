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
  /// The run's verb alone ("Enhancing", "Downloading Sharpen & restore"), for
  /// the one progress line; "" from an Enhance before API 13.
  @Published private(set) var verb = ""
  @Published private(set) var runID: UInt64 = 0
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
  /// Try again: the card's own button for the tool that ran.
  func retry() { SlotStore.shared.send(Self.address, node: "go." + tool, kind: 1, value: nil) }

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
    let verb = Self.find("run.verb", in: tree)?.string("text") ?? ""
    if self.verb != verb { self.verb = verb }
    if runID != id { runID = id }
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

/// The tool bar's line while a run shows, in the one progress language
/// (ProgressLine.swift, docs/design/20 "Craft rules"): the bar and "Enhancing ·
/// 40 % · about 1 min left" with Cancel (Esc too); then, in the same slot, the
/// result with Move to Trash, or why it stopped with Try again, and a quiet ×.
/// Its width is fixed, so the bar under the photo never reflows as the words change.
struct EnhanceRunRow: View {
  var compact = false
  @ObservedObject private var run = EnhanceRun.shared

  private var verb: String {
    if !run.verb.isEmpty { return run.verb }
    // An Enhance from before API 13: its line's first words.
    let first = run.line.components(separatedBy: CharacterSet(charactersIn: "…·")).first ?? ""
    return first.trimmingCharacters(in: .whitespaces).isEmpty ? "Working" : first.trimmingCharacters(in: .whitespaces)
  }

  private var outcome: ProgressOutcome? {
    switch run.state {
    case "done": return .done(run.line, action: run.canDiscard ? run.discardLabel : nil)
    case "failed": return .failed(run.line.isEmpty ? "It did not finish." : run.line, retry: !["", "remove", "extend"].contains(run.tool))  // those two run from the bar, not a card's button
    case "cancelled": return .info(run.line.isEmpty ? "Cancelled. The photo is unchanged." : run.line)
    default: return nil
    }
  }

  var body: some View {
    HStack(spacing: MVTheme.space(4)) {
      ProgressLine(verb: verb, fraction: run.fraction >= 0 ? min(1, run.fraction) : nil,
                   key: "enhance-run-\(run.runID)", timed: run.working, outcome: outcome,
                   escCancels: true,
                   onCancel: run.working ? { run.cancel() } : nil,
                   onAction: { run.discard() },
                   onRetry: { run.retry() })
      if !run.working {
        IconButton(symbol: "xmark", help: "Dismiss") { run.dismiss() }
          .accessibilityLabel("Dismiss")
      }
    }
    .frame(width: compact ? 260 : 380)
    .padding(.top, MVTheme.space(2))
  }
}
