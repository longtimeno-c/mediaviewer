// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → People follows the folder the viewer has open, and shows everyone
// once none is (owner report, 2026-10-26: back on the empty window, the grid
// still showed the last folder's people). The host says "" on its way back.
import XCTest
@testable import AIChrome
import CAiApi

@MainActor
final class PeopleScopeTests: XCTestCase {
  /// A table with every entry absent: each call fails, no pack is needed.
  private static let api: UnsafeMutablePointer<mv_ai_api> = {
    let p = UnsafeMutablePointer<mv_ai_api>.allocate(capacity: 1)
    p.initialize(to: mv_ai_api())
    p.pointee.struct_size = UInt32(MemoryLayout<mv_ai_api>.size)
    return p
  }()

  private func model() -> ManagementModel { ManagementModel(table: AITable(UnsafePointer(Self.api))) }

  func testNoFolderShowsEveryone() {
    let m = model()
    XCTAssertNil(m.peopleScopeDir)
    XCTAssertEqual(m.peopleScopeValue, SearchScope.all.rawValue)
  }

  func testOpenFolderScopesTheGrid() {
    let m = model()
    m.folderChanged("/A")
    XCTAssertEqual(m.peopleScopeDir, "/A")
    XCTAssertEqual(m.peopleScopeValue, SearchScope.tree.rawValue)
  }

  func testLeavingTheFolderShowsEveryoneAgain() {
    let m = model()
    m.folderChanged("/A")
    m.peopleScope = .folder
    m.folderChanged("")
    XCTAssertNil(m.peopleScopeDir)
    XCTAssertEqual(m.peopleScopeValue, SearchScope.all.rawValue)
    // The next folder keeps the choice made in the last one.
    m.folderChanged("/B")
    XCTAssertEqual(m.peopleScopeDir, "/B")
    XCTAssertEqual(m.peopleScopeValue, SearchScope.folder.rawValue)
  }
}
