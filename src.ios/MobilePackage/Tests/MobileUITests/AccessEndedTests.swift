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

  /// The confirm screen's line for a guest code says how long, as the computer's menu offered it.
  func testAGuestCodeSaysHowLong() {
    XCTAssertEqual(FailureCopy.guestAccess(for: 86_400), "Guest access for 1 day, from when you pair.")
    XCTAssertEqual(FailureCopy.duration(7 * 86_400), "7 days")
    XCTAssertEqual(FailureCopy.duration(3_600), "1 hour")
    XCTAssertEqual(FailureCopy.duration(2 * 3_600), "2 hours")
    XCTAssertEqual(FailureCopy.duration(5_400), "90 minutes")
    XCTAssertNil(LibraryIdentity(name: "Family", fingerprint: "7F3A 92C1").guestFor)
    XCTAssertEqual(LibraryIdentity(name: "Family", fingerprint: "7F3A 92C1", guestFor: 3_600).guestFor, 3_600)
  }

  /// An ended access with its copy gone shows the library and the card, never a copy that would be refused.
  @MainActor
  func testAnEndedLibraryIsNotCopiedAgain() {
    let ended = ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertEqual(AppModel.stage(for: ended), .main)
    let neverCopied = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertEqual(AppModel.stage(for: neverCopied), .firstCopy)
    XCTAssertEqual(AppModel.stage(for: .noLibrary), .welcome)
  }

  func testTheSnapshotCarriesTheEndAndTheState() {
    var s = ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertNil(s.accessUntil)
    s.accessUntil = Date(timeIntervalSince1970: 10)
    XCTAssertNotEqual(s, ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0))
    XCTAssertNotEqual(SynologyState.accessEnded, SynologyState.deviceRevoked)
  }
}
