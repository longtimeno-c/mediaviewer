// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search panel's model against a fake mv.ai.1 table (issue #191): a
// search asked of the open folder asks again when another folder opens, and a
// list of names is only ever taken for the words it was looked up for.
//   cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter SearchModelTests
import XCTest
@testable import AIChrome
import CAiApi

/// What the fake pack was asked; its entries are C functions, so no captures.
private final class FakePack: @unchecked Sendable {
  private let lock = NSLock()
  private var dirs: [String] = []
  private var nextId: UInt64 = 0

  func searched(_ dir: String) -> UInt64 {
    lock.lock()
    defer { lock.unlock() }
    dirs.append(dir)
    nextId += 1
    return nextId
  }

  var asked: [String] {
    lock.lock()
    defer { lock.unlock() }
    return dirs
  }

  func reset() {
    lock.lock()
    defer { lock.unlock() }
    dirs = []
  }
}

private let pack = FakePack()

/// The viewer's placeTerms: (#441), as mv_slots_place_terms answers it: a
/// "place:" word is a place term; `installed` says whether Places is loaded.
private final class FakeViewer: NSObject {
  var installed = false
  @objc(placeTerms:) func placeTerms(_ query: NSString) -> NSNumber {
    var flags: UInt32 = installed ? PlaceTerms.provider : 0
    if (query as String).contains("place:") { flags |= PlaceTerms.present }
    return NSNumber(value: flags)
  }
}

@MainActor
final class SearchModelTests: XCTestCase {
  private static let api: UnsafeMutablePointer<mv_ai_api> = {
    let p = UnsafeMutablePointer<mv_ai_api>.allocate(capacity: 1)
    p.initialize(to: mv_ai_api())
    p.pointee.struct_size = UInt32(MemoryLayout<mv_ai_api>.size)
    p.pointee.search_text = { _, _, dir, _, _, out in
      out?.pointee = pack.searched(dir.map { String(cString: $0) } ?? "")
      return MV_OK
    }
    p.pointee.search_release = { _, _ in MV_OK }
    // "Sa" names Sam; "Sar" names Sarah; anything else, no one.
    p.pointee.suggest_json = { _, query, out, cap, needed in
      let q = query.map { String(cString: $0) } ?? ""
      let json = q == "Sa" ? #"[{"id":1,"name":"Sam","completion":"Sam"}]"#
        : q == "Sar" ? #"[{"id":2,"name":"Sarah","completion":"Sarah"}]"# : "[]"
      let bytes = Array(json.utf8CString)
      needed?.pointee = UInt32(bytes.count)
      guard let out, Int(cap) >= bytes.count else { return MV_ERR_INVALID_ARG }
      bytes.withUnsafeBufferPointer { out.update(from: $0.baseAddress!, count: bytes.count) }
      return MV_OK
    }
    return p
  }()

  private func model() -> SearchModel {
    pack.reset()
    return SearchModel(table: AITable(UnsafePointer(Self.api)))
  }

  /// Lets the model's tasks (a debounce, a names lookup) land.
  private func settle(until done: () -> Bool) async {
    for _ in 0..<200 where !done() { try? await Task.sleep(nanoseconds: 5_000_000) }
  }

  /// #354: the Documents segment and the Text token show only while "Show
  /// documents" is on; turning it off while either is chosen goes back.
  func testDocumentsFollowShowDocuments() {
    let m = model()
    XCTAssertFalse(SearchKinds.shown(audioFiles: false, documentFiles: m.documentFiles).contains(.documents))
    XCTAssertFalse(m.availableFinds.contains(.text))
    XCTAssertEqual(SearchKinds.allHelp(audioFiles: false, documentFiles: false), "Show photos and videos.")
    m.documentFilesChanged(true)
    XCTAssertTrue(SearchKinds.shown(audioFiles: false, documentFiles: m.documentFiles).contains(.documents))
    XCTAssertTrue(m.availableFinds.contains(.text))
    XCTAssertEqual(SearchKinds.allHelp(audioFiles: true, documentFiles: true),
                   "Show photos, videos, documents and audio files.")
    m.kinds = .documents
    m.toggleMatch(.pictures)  // Text alone (and nothing else) on
    XCTAssertTrue(m.matchOn(.text))
    XCTAssertFalse(m.matchOn(.pictures))
    m.documentFilesChanged(false)
    XCTAssertEqual(m.kinds, .all)
    XCTAssertFalse(m.finds.contains(.text))
    // A document result's moment is its page, never a clip time.
    let page = AIResult(index: 0, search: 1, path: "/d/lease.pdf", ptsMs: 2, kind: MV_AI_KIND_DOCUMENTS, more: 0,
                        match: MV_AI_MATCH_TEXT)
    XCTAssertTrue(page.isDocument)
    XCTAssertFalse(page.isClip)
    XCTAssertEqual(page.page, 3)
    let clip = AIResult(index: 0, search: 1, path: "/d/a.mov", ptsMs: 2000, kind: MV_AI_KIND_VIDEOS, more: 0, match: 0)
    XCTAssertTrue(clip.isClip)
    XCTAssertNil(clip.page)
  }

  /// #355: the Audio segment shows only while "Show audio files" is on, and
  /// turning it off while Audio is chosen goes back to All.
  func testAudioSegmentFollowsShowAudioFiles() {
    let m = model()
    XCTAssertFalse(SearchKinds.shown(audioFiles: m.audioFiles).contains(.audio))
    XCTAssertEqual(SearchKinds.all.rawValue, 15)  // MV_AI_KIND_ALL (#354 widened it)
    m.audioFilesChanged(true)
    XCTAssertTrue(SearchKinds.shown(audioFiles: m.audioFiles).contains(.audio))
    m.kinds = .audio
    m.audioFilesChanged(false)
    XCTAssertEqual(m.kinds, .all)
    XCTAssertFalse(SearchKinds.shown(audioFiles: m.audioFiles).contains(.audio))
  }

  func testOpenPanelAsksTheNewFolder() {
    let m = model()
    m.folderChanged("/A")
    m.appeared()
    m.query = "dog"
    m.chipsChanged()
    XCTAssertEqual(pack.asked.last, "/A")
    m.folderChanged("/B")
    XCTAssertEqual(pack.asked.last, "/B")
    m.disappeared()
  }

  func testClosedPanelAsksTheNewFolderWhenItOpens() {
    let m = model()
    m.folderChanged("/A")
    m.appeared()
    m.query = "dog"
    m.chipsChanged()
    m.disappeared()
    let before = pack.asked.count
    m.folderChanged("/B")
    XCTAssertEqual(pack.asked.count, before, "a closed panel does not search")
    m.appeared()
    XCTAssertEqual(pack.asked.last, "/B")
    m.disappeared()
  }

  func testEverywhereIsNotAskedAgain() {
    let m = model()
    m.folderChanged("/A")
    m.scope = .all
    m.appeared()
    m.query = "dog"
    m.chipsChanged()
    let before = pack.asked.count
    m.folderChanged("/B")
    XCTAssertEqual(pack.asked.count, before)
    m.disappeared()
  }

  /// #441: place:Portugal without the Places add-on searches nothing and says
  /// Places is needed (it used to list every geotagged file); with Places it
  /// is an ordinary search, and the help line offers place:.
  func testPlaceTermsNeedPlaces() {
    let viewer = FakeViewer()
    let chrome = MVAIChrome()
    chrome.attach(table: NSValue(pointer: UnsafeRawPointer(Self.api)), host: viewer)
    let m = model()
    m.chrome = chrome
    m.appeared()
    m.query = "place:Portugal"
    m.chipsChanged()
    XCTAssertTrue(m.needsPlaces)
    XCTAssertFalse(m.searching)
    XCTAssertTrue(m.results.isEmpty)
    XCTAssertTrue(pack.asked.isEmpty, "nothing is searched")
    XCTAssertFalse(m.placesAvailable)
    m.query = "beach"
    m.chipsChanged()
    XCTAssertFalse(m.needsPlaces)
    XCTAssertEqual(pack.asked.count, 1)
    viewer.installed = true
    m.refreshPlaces()
    XCTAssertTrue(m.placesAvailable)
    m.query = "beach place:Portugal"
    m.chipsChanged()
    XCTAssertFalse(m.needsPlaces)
    XCTAssertEqual(pack.asked.count, 2, "with Places the pack answers (and intersects)")
    m.disappeared()
  }

  func testNoFolderShowsEverywhere() {
    let m = model()
    XCTAssertEqual(m.scope, .tree)
    XCTAssertEqual(m.shownScope, .all)
    m.folderChanged("/A")
    XCTAssertEqual(m.shownScope, .tree)
  }

  func testTabNeverTakesNamesForOlderWords() async {
    let m = model()
    m.query = "Sa"
    m.queryChanged()
    await settle { !m.suggestions.isEmpty }
    XCTAssertEqual(m.suggestions.map(\.name), ["Sam"])
    // "r" typed: Sam no longer answers the field, and Tab waits for Sarah.
    m.query = "Sar"
    XCTAssertTrue(m.suggestions.isEmpty)
    m.queryChanged()
    XCTAssertTrue(m.completeName())
    XCTAssertEqual(m.query, "Sar")
    await settle { m.query != "Sar" }
    XCTAssertEqual(m.query, "Sarah")
  }

  func testTabWithNoNameComingMovesOn() {
    let m = model()
    m.query = "dog "
    m.queryChanged()
    XCTAssertFalse(m.completeName())
  }

  func testTabWaitingOnOlderWordsIsDropped() async {
    let m = model()
    m.query = "Sar"
    m.queryChanged()
    XCTAssertTrue(m.completeName())
    // Typed on before Sarah came, then back: no Tab for these words now.
    m.query = "Sare"
    m.queryChanged()
    m.query = "Sar"
    m.queryChanged()
    await settle { !m.suggestions.isEmpty }
    XCTAssertEqual(m.suggestions.map(\.name), ["Sarah"])
    XCTAssertEqual(m.query, "Sar")
  }
}
