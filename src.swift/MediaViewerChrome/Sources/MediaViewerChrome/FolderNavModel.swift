// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/16 "Folder navigation" / "Drag and drop into folders": the plain
// values behind the gallery's drop feedback, spring-loading and the scroll a
// Back restores. No bridge and no AppKit, so the test target compiles this file
// as it is (Tests/MediaViewerChromeTests/FolderNavModel.swift is a symlink).
// The rule itself (what a drop becomes) is shell/file_drop.h's, asked through
// mv_chrome_drop_verdict; this only reads its answer and words it.
import Foundation

/// mv_chrome_drop_verdict's flags.
struct DropFlags: OptionSet, Hashable {
  let rawValue: Int32
  static let fromApp = DropFlags(rawValue: 1)
  /// Option: copy what a drag inside the app would move.
  static let copyKey = DropFlags(rawValue: 2)
  /// Command: move what a drag from Finder would copy.
  static let moveKey = DropFlags(rawValue: 4)

  static func of(fromApp: Bool, option: Bool, command: Bool) -> DropFlags {
    var f: DropFlags = []
    if fromApp { f.insert(.fromApp) }
    if option { f.insert(.copyKey) }
    if command { f.insert(.moveKey) }
    return f
  }
}

/// shell/file_drop.h drop_verb / drop_refusal, as the bridge packs them.
struct DropVerdict: Equatable {
  enum Verb: Int32 { case none = 0, copy = 1, move = 2 }
  enum Refusal: Int32 {
    case none = 0, nothing, readOnlyTarget, remoteTarget, intoItself, alreadyThere, remoteSource
  }
  var verb: Verb = .none
  var refusal: Refusal = .nothing

  init(verb: Verb = .none, refusal: Refusal = .nothing) {
    self.verb = verb
    self.refusal = refusal
  }

  /// verb | refusal << 8; unknown values read as no drop.
  init(raw: Int32) {
    verb = Verb(rawValue: raw & 0xFF) ?? .none
    refusal = Refusal(rawValue: (raw >> 8) & 0xFF) ?? .nothing
    if verb == .none && refusal == .none { refusal = .nothing }
  }

  var allowed: Bool { verb != .none }
  /// Refused for a reason worth saying on the target (not just "no files").
  var refusedVisibly: Bool { verb == .none && refusal != .nothing && refusal != .none }

  /// The line over the grid while a drag hovers: "Move 3 items to Trip",
  /// "Copy to Trip" (an outside drag, count unknown until it lands), or the
  /// refusal's words (`reason`, the host's text for the refusal).
  func label(count: Int?, target: String, reason: String) -> String {
    switch verb {
    case .none: return reason
    case .copy, .move:
      let word = verb == .move ? "Move" : "Copy"
      guard let count, count > 0 else { return "\(word) to \(target)" }
      return count == 1 ? "\(word) 1 item to \(target)" : "\(word) \(count) items to \(target)"
    }
  }
}

/// A folder hovered by a drag opens after this long, as Finder's spring-loaded
/// folders do; leaving it first cancels.
enum SpringLoad {
  static let delay: TimeInterval = 0.7
}

/// Where the grid was scrolled in each folder (the top cell on screen), so a
/// Back or Forward puts it back. Bounded; the oldest folder drops off first.
struct ScrollMemory {
  private var tops: [String: Int] = [:]
  private var order: [String] = []
  let capacity: Int

  init(capacity: Int = 200) { self.capacity = capacity }

  mutating func remember(_ dir: String, top: Int?) {
    guard !dir.isEmpty else { return }
    order.removeAll { $0 == dir }
    guard let top, top > 0 else {
      tops[dir] = nil  // at the top: nothing to restore
      return
    }
    tops[dir] = top
    order.append(dir)
    while order.count > capacity { tops[order.removeFirst()] = nil }
  }

  func top(for dir: String) -> Int? { tops[dir] }
}

/// The cells on screen, by id, from their appear / disappear: the top one is
/// the smallest. No geometry read and nothing published.
struct VisibleCells {
  private(set) var shown = Set<Int>()
  mutating func appear(_ id: Int) { shown.insert(id) }
  mutating func disappear(_ id: Int) { shown.remove(id) }
  mutating func reset() { shown.removeAll() }
  var top: Int? { shown.min() }
}

/// What a click on a folder tile does: one click selects it, two open it
/// (Finder). Read from the click's own count, so a single click is not held
/// back waiting to see whether a second follows.
enum TileClick: Equatable {
  case select, open
  static func of(clickCount: Int) -> TileClick { clickCount >= 2 ? .open : .select }
}

/// What the command bar's path area shows: a folder's trail (with Back, Forward, Up and
/// drop-taking crumbs), local or on another computer, or a search's title and count.
enum PathBarMode: Equatable {
  case trail
  case results(String)

  static func of(listTitle: String?, remoteFolder: String) -> PathBarMode {
    guard let listTitle else { return .trail }
    return remoteFolder.isEmpty ? .results(listTitle) : .trail
  }
}

/// What a folder tile is called: the host's name for it when it has one (another
/// computer's folder is a `remote:` key, whose parts are ids), else its path's last part.
enum FolderLabel {
  static func of(path: String, hostName: String?) -> String {
    if let hostName, !hostName.isEmpty { return hostName }
    let leaf = URL(fileURLWithPath: path).lastPathComponent
    return leaf.isEmpty ? path : leaf
  }
}
