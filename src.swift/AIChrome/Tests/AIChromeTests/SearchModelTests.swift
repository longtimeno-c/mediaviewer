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
}
