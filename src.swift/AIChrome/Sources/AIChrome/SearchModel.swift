// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search panel's model over mv.ai.1 (plan/17 "Search", "UI and commands"):
// the query, scope and kind chips, the debounced search, result tiles and their
// thumbnails, the empty states, find-similar, the status footer, opening the
// results as a gallery listing, and the matching moments of the clip on screen
// (scrub markers, N / Shift+N). The Mac twin of the WinUI search panel.
//
// Threads: [no-block] calls run on the main actor; result reads, thumbnails
// and clip matches run in detached tasks. Searching itself runs on the pack's
// own workers and reports MV_ADDON_EVENT_AI_SEARCH_DONE.
import AppKit
import CAiApi
import Foundation
import SwiftUI

struct AIResult: Identifiable, Equatable, Sendable {
  let index: Int
  let search: UInt64
  let path: String
  let ptsMs: Int64          // -1 for a photo
  let kind: UInt32
  let more: UInt32          // other matching moments in the same clip
  let match: UInt32         // MV_AI_MATCH_*: picture, sound, speech (0 from an older pack)
  var snippet = ""          // the words said, for a speech match ("…happy birthday Anna…")
  var matchedSound: Bool { match & MV_AI_MATCH_SOUND != 0 }
  var matchedSpeech: Bool { match & MV_AI_MATCH_SPEECH != 0 }
  /// Stable across a re-run of the same search while the index grows, so a
  /// tile that is already on screen keeps its picture and does not fade in again.
  var id: String { "\(path)|\(ptsMs)" }
  var name: String { (path as NSString).lastPathComponent }
  var isClip: Bool { ptsMs >= 0 }
}

enum SearchScope: UInt32, CaseIterable, Identifiable {
  case folder = 0, tree = 1, all = 2
  var id: UInt32 { rawValue }
  var label: String {
    switch self {
    case .folder: return "This folder"
    case .tree: return "This folder and subfolders"
    case .all: return "Everything indexed"
    }
  }
}

enum SearchKinds: UInt32, CaseIterable, Identifiable {
  case all = 3, photos = 1, videos = 2
  var id: UInt32 { rawValue }
  var label: String {
    switch self {
    case .all: return "All"
    case .photos: return "Photos"
    case .videos: return "Videos"
    }
  }
}

@MainActor
final class SearchModel: ObservableObject {
  let table: AITable
  weak var chrome: MVAIChrome?

  // The field and the chips.
  @Published var query = ""
  @Published var scope: SearchScope = .folder
  @Published var kinds: SearchKinds = .all
  /// What to find (2026-09-27): Pictures · Sounds · Speech. Empty = all three.
  @Published var finds: Set<Find> = []

  enum Find: UInt32, CaseIterable, Identifiable {
    case pictures = 0x10, sounds = 0x20, speech = 0x40
    var id: UInt32 { rawValue }
    var label: String {
      switch self {
      case .pictures: return "Pictures"
      case .sounds: return "Sounds"
      case .speech: return "Speech"
      }
    }
    var symbol: String {
      switch self {
      case .pictures: return "photo"
      case .sounds: return "speaker.wave.2"
      case .speech: return "text.bubble"
      }
    }
  }

  /// The kinds word with the FIND bits OR'ed in (none set = all).
  private var kindBits: UInt32 { finds.reduce(kinds.rawValue) { $0 | $1.rawValue } }

  /// Sounds and speech exist only with the ai-audio piece loaded.
  var audioReady: Bool { status.audioReady }
  /// Find-similar / a person: shown as a removable chip in place of the text.
  @Published var reference: Reference?

  struct Reference: Equatable {
    enum Kind: Equatable { case similar(path: String, ptsMs: Int64), person(id: UInt64) }
    let kind: Kind
    let label: String       // "Similar to IMG_0412.MOV", "Photos of Sam"
  }

  // What is shown.
  @Published private(set) var results: [AIResult] = []
  @Published var selected: Int = 0
  @Published private(set) var searching = false
  @Published private(set) var finished = false      // a search came back (maybe empty)
  @Published private(set) var coverage: UInt32 = 2  // 0 not indexed, 1 indexing, 2 complete
  @Published private(set) var status = StatusLine()
  @Published private(set) var folder = ""           // the scope folder ("" none open)
  @Published private(set) var resultsGeneration = 0 // moves with every new result set

  private var slots: [String: ImageSlot] = [:]
  private var pending: UInt64 = 0      // the search whose results are awaited
  private var shown: UInt64 = 0        // the search on screen
  private var listed: UInt64 = 0       // the search whose results the viewer lists
  private var debounce: Task<Void, Never>?
  private var statusTimer: Timer?
  private var rerunTimer: Timer?
  private(set) var visible = false

  // Scrub markers: the clip on screen and its matching moments.
  private var markerPath = ""
  private var markerMs: [Int64] = []

  init(table: AITable) { self.table = table }

  // MARK: folder and visibility

  func folderChanged(_ dir: String) {
    folder = dir
    if !dir.isEmpty { _ = table.a.note_folder_opened?(table.ctx, dir) }
    if visible { refreshCoverage() }
  }

  func refreshCoverage() {
    guard !folder.isEmpty else { coverage = 2; return }
    var state: UInt32 = 2
    if table.a.folder_coverage?(table.ctx, folder, &state) == MV_OK { coverage = state }
  }

  /// The panel opened: status at 4 Hz while it is visible (the brief), and a
  /// re-run while indexing so results grow with the index.
  func appeared() {
    visible = true
    refreshCoverage()
    pollStatus()
    statusTimer?.invalidate()
    statusTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.pollStatus() }
    }
    rerunTimer?.invalidate()
    rerunTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.rerunWhileIndexing() }
    }
  }

  func disappeared() {
    visible = false
    statusTimer?.invalidate()
    statusTimer = nil
    rerunTimer?.invalidate()
    rerunTimer = nil
  }

  func pollStatus() {
    guard let s = table.status() else { return }
    let line = StatusLine(s)
    if line != status { status = line }
  }

  func setPaused(_ paused: Bool) {
    _ = table.a.pause?(table.ctx, paused ? 1 : 0)
    pollStatus()
  }

  private func rerunWhileIndexing() {
    guard visible, status.indexing, !searching else { return }
    if reference != nil || !query.trimmingCharacters(in: .whitespaces).isEmpty { run(keepSelection: true) }
  }

  // MARK: searching

  /// Typing: ~200 ms debounce, then search.
  func queryChanged() {
    if reference != nil { return }
    debounce?.cancel()
    debounce = Task { [weak self] in
      try? await Task.sleep(nanoseconds: 200_000_000)
      guard !Task.isCancelled else { return }
      self?.run(keepSelection: false)
    }
  }

  func chipsChanged() { run(keepSelection: false) }

  private var scopeDir: String? { scope == .all || folder.isEmpty ? nil : folder }
  private var effectiveScope: UInt32 { folder.isEmpty ? SearchScope.all.rawValue : scope.rawValue }

  private var keepPath = ""
  private var runSeq = 0
  private var rerun = false

  func run(keepSelection: Bool) {
    runSeq += 1
    rerun = keepSelection
    keepPath = keepSelection && results.indices.contains(selected) ? results[selected].path : ""
    let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
    var id: UInt64 = 0
    var st = MV_ERR_INVALID_ARG
    if let reference {
      switch reference.kind {
      case .similar(let path, let pts):
        st = withOptionalCString(scopeDir) { dir in
          table.a.search_similar?(table.ctx, path, pts, dir, effectiveScope, kinds.rawValue, &id) ?? MV_ERR_INVALID_ARG
        }
      case .person(let person):
        st = withOptionalCString(scopeDir) { dir in
          table.a.search_person?(table.ctx, person, dir, effectiveScope, &id) ?? MV_ERR_INVALID_ARG
        }
      }
    } else if !text.isEmpty {
      st = withOptionalCString(scopeDir) { dir in
        table.a.search_text?(table.ctx, text, dir, effectiveScope, kindBits, &id) ?? MV_ERR_INVALID_ARG
      }
    } else {
      // An empty field: nothing to show, nothing pending.
      let previous = pending
      pending = 0
      releaseIfUnused(previous)
      searching = false
      finished = false
      setResults([], search: 0)
      return
    }
    guard st == MV_OK, id != 0 else {
      searching = false
      finished = true
      return
    }
    let previous = pending
    pending = id
    releaseIfUnused(previous)
    searching = true
  }

  /// MV_ADDON_EVENT_AI_SEARCH_DONE: id = search, payload = count.
  func searchDone(_ id: UInt64, status: UInt32, count: Int64) {
    guard id == pending else {
      releaseIfUnused(id)
      return
    }
    pending = 0
    guard status == MV_OK.rawValue else {
      searching = false
      finished = true
      releaseIfUnused(id)
      return
    }
    let t = table
    let n = Int(max(0, min(count, 500)))
    let seq = runSeq
    Task.detached {
      var out: [AIResult] = []
      var seen = Set<String>()
      out.reserveCapacity(n)
      for i in 0..<n {
        var r = mv_ai_result()
        guard t.a.result_at?(t.ctx, id, UInt32(i), &r) == MV_OK else { continue }
        let path = t.path { t.a.result_path?(t.ctx, id, UInt32(i), $0, $1) ?? MV_ERR_INVALID_ARG } ?? ""
        guard !path.isEmpty else { continue }
        var result = AIResult(index: i, search: id, path: path, ptsMs: r.pts_ms, kind: r.kind,
                              more: r.more_in_clip, match: r.match)
        // The words that matched (a speech result), read here on the worker.
        if result.matchedSpeech, t.hasAudio {
          result.snippet = t.path { t.a.result_snippet?(t.ctx, id, UInt32(i), $0, $1) ?? MV_ERR_INVALID_ARG } ?? ""
        }
        guard seen.insert(result.id).inserted else { continue }
        out.append(result)
      }
      let list = out
      await MainActor.run {
        // A newer search started while this one was read: it is not shown.
        guard seq == self.runSeq else {
          self.releaseIfUnused(id)
          return
        }
        self.searching = false
        self.finished = true
        self.setResults(list, search: id)
      }
    }
  }

  private func setResults(_ list: [AIResult], search: UInt64) {
    let old = shown
    shown = search
    if old != search { releaseIfUnused(old) }
    if !rerun {
      // A new query: fresh tiles, and the staggered entrance plays again.
      slots.removeAll()
      resultsGeneration += 1
    }
    rerun = false
    results = list
    if !keepPath.isEmpty, let i = list.firstIndex(where: { $0.path == keepPath }) {
      selected = i
    } else {
      selected = 0
    }
    keepPath = ""
  }

  private func releaseIfUnused(_ id: UInt64) {
    guard id != 0, id != pending, id != shown, id != listed else { return }
    _ = table.a.search_release?(table.ctx, id)
  }

  func releaseAll() {
    for id in Set([pending, shown, listed]) where id != 0 { _ = table.a.search_release?(table.ctx, id) }
    pending = 0
    shown = 0
    listed = 0
  }

  func clearReference() {
    reference = nil
    run(keepSelection: false)
  }

  // MARK: the index offer

  func indexFolder(recursive: Bool) {
    guard !folder.isEmpty else { return }
    var root: UInt64 = 0
    _ = table.a.index_folder?(table.ctx, folder, recursive ? 1 : 0, &root)
    refreshCoverage()
    pollStatus()
  }

  // MARK: tiles

  func slot(for r: AIResult) -> ImageSlot {
    if let s = slots[r.id] { return s }
    let s = ImageSlot()
    slots[r.id] = s
    return s
  }

  /// A tile appeared: its thumbnail, from the viewer's JPEG-512 cache (made
  /// on a miss by the pack, [worker-thread]), decoded off the main thread.
  func requestThumb(_ r: AIResult) {
    let s = slot(for: r)
    guard !s.requested else { return }
    s.requested = true
    let key = "tile|\(r.path)|\(r.ptsMs)"
    if let hit = ImageCache.shared.get(key) {
      s.image = hit
      return
    }
    let t = table
    Task.detached(priority: .utility) {
      let jpeg = t.path { t.a.result_thumb?(t.ctx, r.search, UInt32(r.index), $0, $1) ?? MV_ERR_INVALID_ARG }
      let image = jpeg.flatMap { ImageLoad.decode(path: $0, maxPixel: 384) }
      if let image { ImageCache.shared.put(key, image) }
      await MainActor.run { s.image = image }
    }
  }

  // MARK: opening results in the viewer

  var listTitle: String {
    if let reference { return reference.label }
    return query.trimmingCharacters(in: .whitespacesAndNewlines)
  }

  /// Enter: the results as a gallery listing, the chosen tile on the canvas
  /// (a clip paused on its moment). Cmd+Enter: the gallery grid.
  func openResults(gallery: Bool) -> Bool {
    guard !results.isEmpty else { return false }
    let request: NSDictionary = [
      "title": listTitle,
      "paths": results.map { $0.path },
      "moments": results.map { NSNumber(value: $0.ptsMs) },
      "select": NSNumber(value: min(max(selected, 0), results.count - 1)),
      "gallery": NSNumber(value: gallery),
    ]
    guard chrome?.hostOpenList(request) == true else { return false }
    let old = listed
    listed = shown
    if old != listed { releaseIfUnused(old) }
    markerPath = ""
    return true
  }

  // MARK: find similar

  /// Ctrl/Cmd+Shift+F: the still, or the clip's current frame.
  func findSimilar(path: String, isVideo: Bool, positionMs: Int64) {
    guard !path.isEmpty else { return }
    let pts = isVideo ? max(0, positionMs) : -1
    reference = Reference(kind: .similar(path: path, ptsMs: pts),
                          label: "Similar to \((path as NSString).lastPathComponent)")
    run(keepSelection: false)
  }

  func showPerson(id: UInt64, name: String) {
    reference = Reference(kind: .person(id: id), label: name.isEmpty ? "This person" : "Photos of \(name)")
    run(keepSelection: false)
  }

  // MARK: matching moments of the clip on screen

  /// The canvas item changed: if it is a clip in the listed (or shown) search,
  /// its matches become the scrub markers; the current one is where it opened.
  func itemChanged(_ path: String, isVideo: Bool) {
    let search = listed != 0 ? listed : shown
    guard isVideo, search != 0, !path.isEmpty else {
      if !markerPath.isEmpty { clearMarkers() }
      return
    }
    let opened = results.first(where: { $0.path == path && $0.search == search })?.ptsMs ?? -1
    let t = table
    Task.detached {
      let ms = SearchModel.clipMatches(t, search: search, path: path)
      await MainActor.run {
        self.markerPath = path
        self.markerMs = ms
        let current = opened >= 0 ? SearchModel.nearest(ms, to: opened) : -1
        self.chrome?.hostSetMarkers(path: path, ms: ms, current: current)
      }
    }
  }

  private func clearMarkers() {
    chrome?.hostSetMarkers(path: markerPath, ms: [], current: -1)
    markerPath = ""
    markerMs = []
  }

  nonisolated static func clipMatches(_ t: AITable, search: UInt64, path: String) -> [Int64] {
    guard let fn = t.a.clip_matches else { return [] }
    var count: UInt32 = 0
    guard fn(t.ctx, search, path, nil, nil, 0, &count) == MV_OK, count > 0 else { return [] }
    var ms = [Int64](repeating: 0, count: Int(count))
    var got: UInt32 = 0
    let st = ms.withUnsafeMutableBufferPointer { fn(t.ctx, search, path, $0.baseAddress, nil, count, &got) }
    guard st == MV_OK else { return [] }
    return Array(ms.prefix(Int(min(got, count))))
  }

  nonisolated static func nearest(_ ms: [Int64], to t: Int64) -> Int {
    guard !ms.isEmpty else { return -1 }
    var best = 0
    for i in ms.indices where abs(ms[i] - t) < abs(ms[best] - t) { best = i }
    return best
  }

  /// N / Shift+N: the next / previous matching moment, an exact seek that
  /// keeps a paused clip paused. False when there is nowhere to go.
  func stepMatch(forward: Bool, path: String, positionMs: Int64) -> Bool {
    let search = listed != 0 ? listed : shown
    guard search != 0, !path.isEmpty else { return false }
    // Read now if the markers are for another clip (a no-block count and copy).
    if markerPath != path {
      markerMs = SearchModel.clipMatches(table, search: search, path: path)
      markerPath = path
    }
    guard !markerMs.isEmpty else { return false }
    // Half a frame of slack so "the match I am on" is not the next one.
    let slack: Int64 = 20
    let target: Int?
    if forward {
      target = markerMs.firstIndex(where: { $0 > positionMs + slack })
    } else {
      target = markerMs.lastIndex(where: { $0 < positionMs - slack })
    }
    guard let i = target else { return false }
    chrome?.hostSeek(markerMs[i])
    chrome?.hostSetMarkers(path: path, ms: markerMs, current: i)
    return true
  }
}
