// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/nas-owner-and-guests.md §8.2: a phone whose time-limited access ended is told so, and
// that is not the "removed" a revoke says: no new code is asked for, only the owner's extension.
import Foundation
import MobileKit
@testable import MobileUI
import XCTest

final class AccessEndedTests: XCTestCase {
  func testEndedIsNotRemoved() {
    let end = Date(timeIntervalSince1970: 1_794_000_000)
    let line = FailureCopy.accessEnded(library: "Family", at: end)
    XCTAssertNotEqual(line, FailureCopy.deviceRevoked)
    XCTAssertTrue(line.contains("“Family”"))
    XCTAssertTrue(line.contains(end.formatted(date: .long, time: .shortened)))
    XCTAssertTrue(line.contains("extend"))
    XCTAssertFalse(line.contains("removed"))
    XCTAssertFalse(line.lowercased().contains("scan"))
  }

  func testWithNoNameOrDateItStillSaysWhat() {
    let line = FailureCopy.accessEnded(library: "", at: nil)
    XCTAssertTrue(line.contains("this library"))
    XCTAssertFalse(line.contains("“"))
  }

  func testAccessUntilNamesTheEnd() {
    let end = Date(timeIntervalSince1970: 1_794_000_000)
    XCTAssertEqual(FailureCopy.accessUntil(end), "Access until \(end.formatted(date: .abbreviated, time: .shortened))")
  }

  func testTheSnapshotCarriesTheEndAndTheState() {
    var s = ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertNil(s.accessUntil)
    s.accessUntil = Date(timeIntervalSince1970: 10)
    XCTAssertNotEqual(s, ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0))
    XCTAssertNotEqual(SynologyState.accessEnded, SynologyState.deviceRevoked)
  }
}
