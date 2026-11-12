// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/nas-owner-and-guests.md §8.2, call C6: the copy goes only when the NAS says the time
// is up, only for the library still paired, and the pairing itself stays.
import Foundation
import MobileLinkSupport
import XCTest

final class AccessEndTests: XCTestCase {
  func testOnlyTheServiceDeletes() {
    let now = 1_000_000.0
    XCTAssertEqual(AccessEnd.decide(serviceSaidEnded: true, noRouteAnswered: false, storedEnd: nil, now: now),
                   .init(showEnded: true, dropLocalCopy: true))
    // Offline and past the end the NAS gave: said, not acted on.
    XCTAssertEqual(AccessEnd.decide(serviceSaidEnded: false, noRouteAnswered: true, storedEnd: now - 1, now: now),
                   .init(showEnded: true, dropLocalCopy: false))
    // Offline, still inside it: just unreachable.
    XCTAssertEqual(AccessEnd.decide(serviceSaidEnded: false, noRouteAnswered: true, storedEnd: now + 60, now: now),
                   .init(showEnded: false, dropLocalCopy: false))
    // Answered with something else (a revoke, a wrong service): never "ended", whatever the clock.
    XCTAssertEqual(AccessEnd.decide(serviceSaidEnded: false, noRouteAnswered: false, storedEnd: now - 1, now: now),
                   .init(showEnded: false, dropLocalCopy: false))
    // An unlimited phone has no end to pass.
    XCTAssertEqual(AccessEnd.decide(serviceSaidEnded: false, noRouteAnswered: true, storedEnd: nil, now: now),
                   .init(showEnded: false, dropLocalCopy: false))
  }

  func testTheCopyGoesAndThePairingStays() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("access-end-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: root) }
    let copy = ["library", "thumbs", "originals", "clips"].map { root.appendingPathComponent($0) }
    let pairing = root.appendingPathComponent("pairing.json")
    for folder in copy {
      try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
      try Data("bytes".utf8).write(to: folder.appendingPathComponent("a.bin"))
    }
    try Data("{}".utf8).write(to: pairing)

    let ran = AccessEnd.dropIfStill("family", stored: { "family" }) { AccessEnd.removeFolders(copy) }
    XCTAssertTrue(ran)
    for folder in copy { XCTAssertFalse(FileManager.default.fileExists(atPath: folder.path), folder.lastPathComponent) }
    XCTAssertTrue(FileManager.default.fileExists(atPath: pairing.path))
  }

  func testAnotherLibraryPairedMeanwhileIsNotTouched() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("access-end-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: root) }
    let other = root.appendingPathComponent("thumbs")
    try FileManager.default.createDirectory(at: other, withIntermediateDirectories: true)
    // Paired over (or forgotten) between the refusal and the deletion.
    XCTAssertFalse(AccessEnd.dropIfStill("family", stored: { "office" }) { AccessEnd.removeFolders([other]) })
    XCTAssertFalse(AccessEnd.dropIfStill("family", stored: { nil }) { AccessEnd.removeFolders([other]) })
    XCTAssertTrue(FileManager.default.fileExists(atPath: other.path))
  }
}
