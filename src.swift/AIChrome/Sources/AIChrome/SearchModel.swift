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
  var matchedPicture: Bool { match & MV_AI_MATCH_PICTURE != 0 }
  var matchedSound: Bool { match & MV_AI_MATCH_SOUND != 0 }
  var matchedSpeech: Bool { match & MV_AI_MATCH_SPEECH != 0 }
  /// Stable across a re-run of the same search while the index grows, so a
  /// tile that is already on screen keeps its picture and does not fade in again.
  var id: String { "\(path)|\(ptsMs)" }
  var name: String { (path as NSString).lastPathComponent }
  var isClip: Bool { ptsMs >= 0 }
  /// The same tile on screen (a re-run moves only `search` and `index`).
  func looksLike(_ o: AIResult) -> Bool { path == o.path && ptsMs == o.ptsMs && kind == o.kind && more == o.more && match == o.match }
}

/// The status line, apart from the model: it moves at up to 4 Hz, and only
/// the footer reads it, so the grid does not re-render with it.
@MainActor
final class SearchStatus: ObservableObject {
  @Published fileprivate(set) var line = StatusLine()
}

enum SearchScope: UInt32, CaseIterable, Identifiable {
  case folder = 0, tree = 1, all = 2
  var id: UInt32 { rawValue }
  /// Short, so the three sit as one control: the group's caption ("Look in")
  /// and the tooltip say the rest.
  var label: String {
    switch self {
    case .folder: return "This folder"
    case .tree: return "+ Subfolders"
    case .all: return "Everywhere"
    }
  }
  var help: String {
    switch self {
    case .folder: return "Search the open folder only."
    case .tree: return "Search the open folder and the folders inside it."
    case .all: return "Search every folder in the index."
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
  var help: String {
    switch self {
    case .all: return "Show photos and videos."
    case .photos: return "Show photos only."
    case .videos: return "Show videos only."
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
    /// What in a file the words are matched against, under "Match by": how it
    /// looks, what is heard, what is said.
    var label: String {
      switch self {
      case .pictures: return "Picture"
      case .sounds: return "Sound"
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
    var help: String {
      switch self {
      case .pictures: return "Match what is in the picture or the video frame."
      case .sounds: return "Match what you hear in videos: “dog barking”, “applause”."
      case .speech: return "Match words said in videos."
      }
    }
  }

  /// What can be matched here: sounds and speech need the Sound piece.
  var availableFinds: Set<Find> { audioReady ? Set(Find.allCases) : [.pictures] }

  /// Shown on: every available one when none is chosen (the default, all
  /// searched), else the chosen ones. The row reads as "all on" at rest,
  /// never "none on means all".
  func matchOn(_ f: Find) -> Bool { matching.contains(f) }

  /// The ones on, as shown.
  private var matching: Set<Find> {
    let available = availableFinds
    let chosen = finds.intersection(available)
    return chosen.isEmpty ? available : chosen
  }

  /// Turns one off or on again. The last one on stays on (a search matches
  /// something); all available on is stored as none (the engine's "all").
  func toggleMatch(_ f: Find) {
    let available = availableFinds
    guard available.contains(f) else { return }
    var on = matching
    if on.contains(f) {
      guard on.count > 1 else { return }
      on.remove(f)
    } else {
      on.insert(f)
    }
    finds = on == available ? [] : on
    chipsChanged()
  }

  /// The kinds word with the FIND bits OR'ed in (none set = all). Without
  /// the ai-audio piece every search is a picture search: no bits (the
  /// Windows SearchKinds).
  private var kindBits: UInt32 {
    audioReady ? finds.reduce(kinds.rawValue) { $0 | $1.rawValue } : kinds.rawValue
  }

  /// Sounds and speech exist only with the ai-audio piece loaded.
  @Published private(set) var audioReady = false
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
  @Published private(set) var failed = false        // the last search did not finish
  @Published private(set) var coverage: UInt32 = 2  // 0 not indexed, 1 indexing, 2 complete
  let status = SearchStatus()
  @Published private(set) var indexing = false      // status.line.indexing, published on change
  @Published private(set) var folder = ""           // the scope folder ("" none open)
  @Published private(set) var resultsGeneration = 0 // moves with every new result set

  private var slots: [String: ImageSlot] = [:]
  /// Each shown result's search and index now: a re-run that shows the same
  /// tiles does not republish `results`, only these.
  private var refs: [String: (search: UInt64, index: Int)] = [:]
  private var pending: UInt64 = 0      // the search whose results are awaited
  private var reading: UInt64 = 0      // the search whose results are being read
  private var shown: UInt64 = 0        // the search on screen
  private var listed: UInt64 = 0       // the search whose results the viewer lists
  private var debounce: Task<Void, Never>?
  private var statusTimer: Timer?
  private(set) var visible = false
  private var appActive = true         // the panel hides with the app (hidesOnDeactivate)
  /// Return pressed before the answer landed: open when it does (gallery or not).
  private var openWhenReady: Bool?
  /// The title the results on screen were asked for (not the live field).
  private var shownTitle = ""

  // Re-runs while indexing (the Windows OnStatus rule).
  private var framesIndexed: UInt64 = 0
  private var lastRunFrames: UInt64 = 0
  private var lastRunTime = Date.distantPast

  // Scrub markers: the clip on screen and its matching moments.
  private var markerPath = ""
  private var markerMs: [Int64] = []
  private var markerSeq = 0

  init(table: AITable) { self.table = table }

  // MARK: folder and visibility

  func folderChanged(_ dir: String) {
    folder = dir
    if !dir.isEmpty { _ = table.a.note_folder_opened?(table.ctx, dir) }
    if visible { refreshCoverage() }
  }

  /// Settings → Precision changed: an open description answers again under
  /// the new rule (nothing is re-indexed). Similar and a person's photos do not
  /// read it.
  func precisionChanged() {
    guard reference == nil, !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    run(keepSelection: true)
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
    updateTimer()
    // Closed while it indexed: what was indexed meanwhile answers the same
    // words now (the re-run above waits for more while it is still going).
    if debounce == nil, pending == 0, reading == 0, framesIndexed != lastRunFrames,
       reference != nil || !query.trimmingCharacters(in: .whitespaces).isEmpty {
      run(keepSelection: true)
    }
  }

  func disappeared() {
    visible = false
    startedIndexing = nil
    updateTimer()
  }

  /// The app resigned or became active: a panel hidden with the app does not poll.
  func appActiveChanged(_ active: Bool) {
    appActive = active
    updateTimer()
    if active && visible { pollStatus() }
  }

  private func updateTimer() {
    let want = visible && appActive
    guard want != (statusTimer != nil) else { return }
    statusTimer?.invalidate()
    statusTimer = nil
    if want {
      statusTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated { self?.pollStatus() }
      }
    }
  }

  func pollStatus() {
    guard let s = table.status() else { return }
    let line = StatusLine(s)
    if line != status.line { status.line = line }
    if line.indexing != indexing { indexing = line.indexing }
    if line.audioReady != audioReady { audioReady = line.audioReady }
    framesIndexed = s.frames_indexed
    if s.state == MV_AI_STATE_INDEXING.rawValue { rerunWhileIndexing() }
  }

  func setPaused(_ paused: Bool) {
    _ = table.a.pause?(table.ctx, paused ? 1 : 0)
    pollStatus()
  }

  /// Results appear as the index grows: re-ask now and then while it does —
  /// not while typing (a debounce is pending), not over a search in flight,
  /// and only once enough new moments were indexed to change the answer.
  private func rerunWhileIndexing() {
    guard visible, appActive, debounce == nil, pending == 0, reading == 0 else { return }
    guard reference != nil || !query.trimmingCharacters(in: .whitespaces).isEmpty else { return }
    guard Date().timeIntervalSince(lastRunTime) > 2.5,
          framesIndexed > lastRunFrames + max(200, lastRunFrames / 10) else { return }
    run(keepSelection: true)
  }

  // MARK: searching

  /// Typing: ~200 ms debounce, then search.
  func queryChanged() {
    if reference != nil { return }
    if startedIndexing != nil { startedIndexing = nil }
    openWhenReady = nil
    debounce?.cancel()
    debounce = Task { [weak self] in
      try? await Task.sleep(nanoseconds: 200_000_000)
      guard !Task.isCancelled, let self else { return }
      self.debounce = nil
      self.run(keepSelection: false)
    }
  }

  func chipsChanged() { run(keepSelection: false) }

  private var scopeDir: String? { scope == .all || folder.isEmpty ? nil : folder }
  private var effectiveScope: UInt32 { folder.isEmpty ? SearchScope.all.rawValue : scope.rawValue }

  /// What a run asked for, by search id: read back when its answer lands.
  private struct Run {
    let seq: Int
    let rerun: Bool         // a re-run as the index grows: keep the tiles still
    let keep: String        // the selected tile's id to keep selected
    let title: String
  }
  private var runs: [UInt64: Run] = [:]
  private var runSeq = 0

  func run(keepSelection: Bool) {
    debounce?.cancel()
    debounce = nil
    runSeq += 1
    let keep = keepSelection && results.indices.contains(selected) ? results[selected].id : ""
    let title = listTitle
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
      failed = false
      openWhenReady = nil
      setResults([], search: 0, run: nil)
      return
    }
    guard st == MV_OK, id != 0 else {
      // The search did not start: nothing older may answer for it.
      let previous = pending
      pending = 0
      releaseIfUnused(previous)
      showFailure()
      return
    }
    let previous = pending
    pending = id
    releaseIfUnused(previous)
    runs[id] = Run(seq: runSeq, rerun: keepSelection, keep: keep, title: title)
    lastRunTime = Date()
    lastRunFrames = framesIndexed
    searching = true
  }

  /// "Search did not finish" (the Windows ShowEmpty): no results under a
  /// query they were not for.
  private func showFailure() {
    openWhenReady = nil
    if let done = personDone {
      personDone = nil
      done(.failed)
    }
    searching = false
    finished = true
    failed = true
    setResults([], search: 0, run: nil)
  }

  /// MV_ADDON_EVENT_AI_SEARCH_DONE: id = search, payload = count.
  func searchDone(_ id: UInt64, status: UInt32, count: Int64) {
    guard id == pending else {
      runs[id] = nil
      releaseIfUnused(id)
      return
    }
    pending = 0
    let info = runs.removeValue(forKey: id) ?? Run(seq: runSeq, rerun: false, keep: "", title: listTitle)
    guard status == MV_OK.rawValue else {
      releaseIfUnused(id)
      showFailure()
      return
    }
    reading = id
    let t = table
    let n = Int(max(0, min(count, 500)))
    Task.detached {
      var out: [AIResult] = []
      var seen = Set<String>()
      out.reserveCapacity(n)
      for i in 0..<n {
        var r = mv_ai_result()
        guard t.call({ t.a.result_at?(t.ctx, id, UInt32(i), &r) }) == MV_OK else { continue }
        let path = t.path { t.a.result_path?(t.ctx, id, UInt32(i), $0, $1) ?? MV_ERR_INVALID_ARG } ?? ""
        guard !path.isEmpty else { continue }
        let result = AIResult(index: i, search: id, path: path, ptsMs: r.pts_ms, kind: r.kind,
                              more: r.more_in_clip, match: r.match)
        guard seen.insert(result.id).inserted else { continue }
        out.append(result)
      }
      let list = out
      await MainActor.run {
        if self.reading == id { self.reading = 0 }
        // A newer search started while this one was read: it is not shown.
        guard info.seq == self.runSeq else {
          self.releaseIfUnused(id)
          return
        }
        self.searching = false
        self.finished = true
        self.failed = false
        self.setResults(list, search: id, run: info)
        // Return was pressed before this answer landed: open it now.
        if let gallery = self.openWhenReady {
          self.openWhenReady = nil
          let opened = !list.isEmpty && self.openNow(gallery: gallery)
          if let done = self.personDone {
            self.personDone = nil
            done(opened ? .opened : list.isEmpty ? .nothing : .failed)
          }
        }
      }
    }
  }

  private func setResults(_ list: [AIResult], search: UInt64, run: Run?) {
    let old = shown
    shown = search
    if old != search { releaseIfUnused(old) }
    shownTitle = run?.title ?? ""
    let rerun = run?.rerun ?? false
    let keep = run?.keep ?? ""
    var fresh: [String: (search: UInt64, index: Int)] = [:]
    for r in list { fresh[r.id] = (r.search, r.index) }
    refs = fresh
    if !rerun {
      // A new query: fresh tiles, and the staggered entrance plays again.
      for s in slots.values { s.task?.cancel() }
      slots.removeAll()
      resultsGeneration += 1
    } else {
      // Tiles that were not found here are gone; a kept tile whose picture
      // never came (its read failed against the older search) asks again.
      for (key, s) in slots where fresh[key] == nil {
        s.task?.cancel()
        slots[key] = nil
      }
    }
    if !list.elementsEqual(results, by: { $0.looksLike($1) }) { results = list }
    // The selection stays on its item (path and moment), or goes to the first.
    let i = keep.isEmpty ? 0 : (list.firstIndex(where: { $0.id == keep }) ?? 0)
    if i != selected { selected = i }
    if rerun {
      for r in list {
        if let s = slots[r.id], s.onScreen, s.image == nil, !s.requested { requestThumb(r) }
      }
    }
  }

  private func releaseIfUnused(_ id: UInt64) {
    guard id != 0, id != pending, id != reading, id != shown, id != listed else { return }
    runs[id] = nil
    _ = table.a.search_release?(table.ctx, id)
  }

  func releaseAll() {
    debounce?.cancel()
    debounce = nil
    for s in slots.values { s.task?.cancel() }
    for id in Set([pending, reading, shown, listed]) where id != 0 { _ = table.a.search_release?(table.ctx, id) }
    pending = 0
    reading = 0
    shown = 0
    listed = 0
    runs.removeAll()
  }

  func clearReference() {
    reference = nil
    run(keepSelection: false)
  }

  // MARK: the index offer

  /// Set when the offer's Index was chosen in this showing of the panel: the
  /// empty state then says indexing carries on in the background and offers
  /// to close. Cleared by typing, a result, or the panel closing.
  @Published private(set) var startedIndexing: Bool?  // recursive

  var folderName: String {
    let leaf = (folder as NSString).lastPathComponent
    return leaf.isEmpty ? folder : leaf
  }

  func indexFolder(recursive: Bool) {
    guard !folder.isEmpty else { return }
    var root: UInt64 = 0
    let st = table.a.index_folder?(table.ctx, folder, recursive ? 1 : 0, &root)
    refreshCoverage()
    pollStatus()
    if st == MV_OK { startedIndexing = recursive }
    // Results for words already typed grow as the index commits.
    if reference != nil || !query.trimmingCharacters(in: .whitespaces).isEmpty { run(keepSelection: false) }
  }

  /// "Index anyway" while paused on battery: until the Mac is next on power.
  func indexAnyway() {
    table.indexAnyway()
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
  /// on a miss by the pack, [worker-thread]), decoded off the main thread,
  /// a few at a time (ThumbGate); and, for a speech match, the words said.
  func requestThumb(_ r: AIResult) {
    let s = slot(for: r)
    s.onScreen = true
    guard !s.requested, let ref = refs[r.id] else { return }
    let key = "tile|\(r.path)|\(r.ptsMs)"
    let wantSnippet = r.matchedSpeech && !s.snippetRead && table.hasAudio
    if s.image == nil, let hit = ImageCache.shared.get(key) { s.image = hit }
    guard s.image == nil || wantSnippet else { return }
    s.requested = true
    let t = table
    let needImage = s.image == nil
    s.task = Task.detached(priority: .utility) {
      // The words that matched first: a string read, no decode.
      var snippet: String?
      if wantSnippet {
        snippet = t.path { t.a.result_snippet?(t.ctx, ref.search, UInt32(ref.index), $0, $1) ?? MV_ERR_INVALID_ARG }
      }
      var image: CGImage?
      if needImage, !Task.isCancelled {
        await ThumbGate.shared.acquire()
        if !Task.isCancelled {
          let jpeg = t.path { t.a.result_thumb?(t.ctx, ref.search, UInt32(ref.index), $0, $1) ?? MV_ERR_INVALID_ARG }
          image = jpeg.flatMap { ImageLoad.decode(path: $0, maxPixel: 384) }
          if let image { ImageCache.shared.put(key, image) }
        }
        await ThumbGate.shared.release()
      }
      let done = image, words = snippet
      await MainActor.run {
        s.task = nil
        if let words {
          s.snippetRead = true
          if !words.isEmpty { withAnimation(.easeOut(duration: 0.18)) { s.snippet = words } }
        }
        if let done {
          withAnimation(.easeOut(duration: 0.18)) { s.image = done }
          s.requested = true
        } else if needImage || (wantSnippet && words == nil) {
          // Cancelled, or read against a search released meanwhile: ask again
          // when the tile shows (or now, if a newer search holds it).
          s.requested = false
          if s.onScreen, let now = self.refs[r.id], now.search != ref.search { self.requestThumb(r) }
        }
      }
    }
  }

  /// A tile scrolled away: a read that has not started gives its place up.
  func tileGone(_ r: AIResult) {
    guard let s = slots[r.id] else { return }
    s.onScreen = false
    if s.image == nil, let task = s.task {
      task.cancel()
      s.task = nil
      s.requested = false
    }
  }

  // MARK: opening results in the viewer

  var listTitle: String {
    if let reference { return reference.label }
    return query.trimmingCharacters(in: .whitespacesAndNewlines)
  }

  /// Enter: the results as a gallery listing, the chosen tile on the canvas
  /// (a clip paused on its moment). Cmd+Enter: the gallery grid. Typed and
  /// pressed Enter at once: the search runs now and opens when its answer
  /// lands, never the older results under the new words.
  func openResults(gallery: Bool) -> Bool {
    if debounce != nil || pending != 0 || reading != 0 {
      openWhenReady = gallery
      if debounce != nil { run(keepSelection: false) }
      return true
    }
    return openNow(gallery: gallery)
  }

  private func openNow(gallery: Bool) -> Bool {
    guard !results.isEmpty, shown != 0 else { return false }
    let request: NSDictionary = [
      "title": shownTitle.isEmpty ? "Search results" : shownTitle,
      "paths": results.map { $0.path },
      "moments": results.map { NSNumber(value: $0.ptsMs) },
      "select": NSNumber(value: min(max(selected, 0), results.count - 1)),
      "gallery": NSNumber(value: gallery),
    ]
    guard chrome?.hostOpenList(request, from: self) == true else { return false }
    let old = listed
    listed = shown
    if old != listed { releaseIfUnused(old) }
    markerPath = ""
    return true
  }

  /// Whether a SEARCH_DONE for `id` is this model's: the chrome routes each
  /// answer to its owner only (a model releases searches it does not own).
  func owns(_ id: UInt64) -> Bool { id != 0 && (id == pending || id == reading || runs[id] != nil) }

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

  enum PersonOpen { case opened, nothing, failed }
  /// Hears how openPerson ended, once.
  private var personDone: ((PersonOpen) -> Void)?

  /// People → a person: their photos and moments, from every folder, opened
  /// straight into the gallery as a result list, the panel never shown.
  func openPerson(id: UInt64, name: String, done: @escaping (PersonOpen) -> Void) {
    scope = .all
    kinds = .all
    finds = []
    query = ""
    reference = Reference(kind: .person(id: id), label: name.isEmpty ? "This person" : "Photos of \(name)")
    personDone = done
    openWhenReady = true
    run(keepSelection: false)
  }

  // MARK: matching moments of the clip on screen

  /// The canvas item changed: if it is a clip in the listed (or shown) search,
  /// its matches become the scrub markers; the current one is where it opened.
  func itemChanged(_ path: String, isVideo: Bool) {
    let search = listed != 0 ? listed : shown
    markerSeq += 1
    guard isVideo, search != 0, !path.isEmpty else {
      if !markerPath.isEmpty { clearMarkers() }
      return
    }
    let opened = search == shown ? (results.first(where: { $0.path == path })?.ptsMs ?? -1) : -1
    let t = table
    let seq = markerSeq
    Task.detached {
      let ms = SearchModel.clipMatches(t, search: search, path: path)
      await MainActor.run {
        // Another clip (or a step) since: these are not its markers.
        guard seq == self.markerSeq else { return }
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
    t.guarded { () -> [Int64] in
      guard let fn = t.a.clip_matches else { return [] }
      var count: UInt32 = 0
      guard fn(t.ctx, search, path, nil, nil, 0, &count) == MV_OK, count > 0 else { return [] }
      var ms = [Int64](repeating: 0, count: Int(count))
      var got: UInt32 = 0
      let st = ms.withUnsafeMutableBufferPointer { fn(t.ctx, search, path, $0.baseAddress, nil, count, &got) }
      guard st == MV_OK else { return [] }
      return Array(ms.prefix(Int(min(got, count))))
    } ?? []
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
      markerSeq += 1  // an older read still in flight does not overwrite these
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
