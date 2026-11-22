// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// FolderNavModel.swift (compiled here through a symlink): the drop verdict as
// the gallery reads it, its words, the click rule and the scroll a Back restores.
// What a drop becomes is shell/file_drop.h's rule, tested in tests/test_file_drop.cpp.
import XCTest

final class FolderNavModelTests: XCTestCase {
  func testFlagsMatchTheBridge() {
    XCTAssertEqual(DropFlags.of(fromApp: true, option: false, command: false).rawValue, 1)
    XCTAssertEqual(DropFlags.of(fromApp: true, option: true, command: false).rawValue, 3)
    XCTAssertEqual(DropFlags.of(fromApp: false, option: false, command: true).rawValue, 4)
    XCTAssertEqual(DropFlags.of(fromApp: false, option: false, command: false).rawValue, 0)
  }

  func testVerdictUnpacksVerbAndRefusal() {
    XCTAssertEqual(DropVerdict(raw: 2), DropVerdict(verb: .move, refusal: .none))
    XCTAssertEqual(DropVerdict(raw: 1), DropVerdict(verb: .copy, refusal: .none))
    XCTAssertEqual(DropVerdict(raw: 3 << 8), DropVerdict(verb: .none, refusal: .remoteTarget))
    XCTAssertEqual(DropVerdict(raw: 4 << 8).refusal, .intoItself)
    XCTAssertTrue(DropVerdict(raw: 3 << 8).refusedVisibly)
    // No files, or nothing under the pointer: refused, but nothing to say.
    XCTAssertFalse(DropVerdict(raw: 1 << 8).refusedVisibly)
    XCTAssertFalse(DropVerdict(raw: 0).allowed)
    XCTAssertFalse(DropVerdict(raw: 0).refusedVisibly)
    // A value from a newer host reads as no drop, never as a copy.
    XCTAssertFalse(DropVerdict(raw: 9).allowed)
  }

  func testLabelSaysWhatHappens() {
    let move = DropVerdict(verb: .move, refusal: .none)
    XCTAssertEqual(move.label(count: 3, target: "Trip", reason: ""), "Move 3 items to Trip")
    XCTAssertEqual(move.label(count: 1, target: "Trip", reason: ""), "Move 1 item to Trip")
    let copy = DropVerdict(verb: .copy, refusal: .none)
    XCTAssertEqual(copy.label(count: nil, target: "Trip", reason: ""), "Copy to Trip")
    let refused = DropVerdict(verb: .none, refusal: .remoteTarget)
    XCTAssertEqual(refused.label(count: 2, target: "NAS", reason: "On another computer: read-only from here"),
                   "On another computer: read-only from here")
  }

  func testOneClickSelectsTwoOpen() {
    XCTAssertEqual(TileClick.of(clickCount: 1), .select)
    XCTAssertEqual(TileClick.of(clickCount: 2), .open)
    XCTAssertEqual(TileClick.of(clickCount: 3), .open)
  }

  func testScrollMemoryRestoresPerFolder() {
    var m = ScrollMemory(capacity: 2)
    m.remember("/a", top: 40)
    m.remember("/b", top: 12)
    XCTAssertEqual(m.top(for: "/a"), 40)
    XCTAssertEqual(m.top(for: "/b"), 12)
    // At the top there is nothing to restore.
    m.remember("/b", top: 0)
    XCTAssertNil(m.top(for: "/b"))
    m.remember("/c", top: 5)
    m.remember("/d", top: 6)
    XCTAssertNil(m.top(for: "/a"))  // the oldest went past the cap
    XCTAssertEqual(m.top(for: "/d"), 6)
    m.remember("", top: 9)
    XCTAssertNil(m.top(for: ""))
  }

  func testVisibleCellsTopIsTheSmallestShown() {
    var v = VisibleCells()
    XCTAssertNil(v.top)
    [30, 31, 32, 33].forEach { v.appear($0) }
    v.disappear(30)
    XCTAssertEqual(v.top, 31)
    v.reset()
    XCTAssertNil(v.top)
  }

  func testSpringLoadIsQuickButNotInstant() {
    XCTAssertGreaterThan(SpringLoad.delay, 0.3)
    XCTAssertLessThan(SpringLoad.delay, 1.2)
  }

  func testARemoteFolderHasTheFolderTrailNotASearchTitle() {
    XCTAssertEqual(PathBarMode.of(listTitle: nil, remoteFolder: ""), .trail)
    XCTAssertEqual(PathBarMode.of(listTitle: "Photos", remoteFolder: "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/"), .trail)
    XCTAssertEqual(PathBarMode.of(listTitle: "beach", remoteFolder: ""), .results("beach"))
  }
}
