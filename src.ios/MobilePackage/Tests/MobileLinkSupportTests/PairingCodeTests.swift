// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileLinkSupport
import XCTest

final class PairingCodeTests: XCTestCase {
  func testAGoodCodeIsUsable() {
    XCTAssertEqual(PairingCode.reading(status: 0, hasDeviceKey: false), .usable)
    XCTAssertEqual(PairingCode.reading(status: 0, hasDeviceKey: true), .usable)
  }

  func testAnExpiredCodeIsRefusedOnAPhoneThatNeverPaired() {
    XCTAssertEqual(PairingCode.reading(status: 2, hasDeviceKey: false), .expired)
  }

  /// The phone paired, its first copy stopped, and the code ran out: scanning it again must reach
  /// the server, which knows the phone.
  func testAnExpiredCodeIsStillTriedByAPhoneThatHasPaired() {
    XCTAssertEqual(PairingCode.reading(status: 2, hasDeviceKey: true), .usable)
  }

  func testAnythingElseIsNotACode() {
    XCTAssertEqual(PairingCode.reading(status: 1, hasDeviceKey: true), .unreadable)
    XCTAssertEqual(PairingCode.reading(status: 9, hasDeviceKey: false), .unreadable)
  }
}
