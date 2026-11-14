// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/self-host-drives.md §4: a server whose combined drive is not healthy says so on the
// phone, calmly, in the library's top slot; the sample library stands in for one (owner 2026-11-12).
import Foundation
import MobileKit
@testable import MobileUI
import XCTest

final class ServerStorageTests: XCTestCase {
  private let lost = "Your server's mirror lost a drive. Your files are safe until another one fails: replace it soon."

  func testAHealthyServerSaysNothing() {
    let s = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertEqual(s.serverStorage, 0)
    XCTAssertNil(AppModel.storageNotice(for: s))
  }

  func testALostDriveIsACalmCard() throws {
    var s = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    s.serverStorage = 2
    s.serverStorageWords = lost
    let notice = try XCTUnwrap(AppModel.storageNotice(for: s))
    XCTAssertEqual(notice.message, lost)
    XCTAssertTrue(notice.message.hasPrefix("Your server's mirror lost a drive"))
    XCTAssertTrue(notice.canDismiss)
    // A new state is said again, even after the last one was dismissed.
    s.serverStorage = 1
    XCTAssertNotEqual(AppModel.storageNotice(for: s)?.id, notice.id)
    // No library: nothing to say.
    XCTAssertNil(AppModel.storageNotice(for: ConnectionSnapshot.noLibrary))
  }

  func testTheSampleOffersTheDemo() {
    XCTAssertEqual(SettingsCopy.sampleLoseDrive, "Show a lost drive")
    XCTAssertEqual(SettingsCopy.sampleMendDrive, "Put the drive back")
  }
}
