// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileLinkSupport
import XCTest

final class LibraryWatchTests: XCTestCase {
  /// Pairing B over A, with no Forget between, is a change: A's catalogue must close.
  func testPairingAnotherLibraryOverThisOneIsAChange() {
    var watch = LibraryWatch(current: "A")
    XCTAssertTrue(watch.changed(to: "B"))
    XCTAssertFalse(watch.changed(to: "B"))
  }

  func testForgetIsAChangeAndTheNextFirstPairingIsNot() {
    var watch = LibraryWatch(current: "A")
    XCTAssertTrue(watch.changed(to: ""))
    XCTAssertFalse(watch.changed(to: "B"))
  }

  func testNothingPairedThenAFirstLibraryIsNot() {
    var watch = LibraryWatch(current: "")
    XCTAssertFalse(watch.changed(to: "A"))
  }

  /// Forget before the first connect: launch already knew the library, so going to none is seen.
  func testForgetBeforeTheFirstConnectIsAChange() {
    var watch = LibraryWatch(current: "A")  // stamped at launch from the stored id
    XCTAssertTrue(watch.changed(to: ""))
  }
}
