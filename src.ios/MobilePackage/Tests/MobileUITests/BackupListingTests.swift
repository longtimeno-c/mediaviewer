// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileKit
import XCTest

final class BackupListingTests: XCTestCase {
  func testCountsThePhotosLookedAt() {
    let listing = BackupListing(looked: 1_500, of: 18_320)
    XCTAssertEqual(listing.line, "Looked at \(1_500.formatted()) of \(18_320.formatted()) photos")
    XCTAssertEqual(listing.fraction, 1_500.0 / 18_320.0, accuracy: 1e-9)
  }

  func testAnEmptyLibraryIsNoFraction() {
    XCTAssertEqual(BackupListing(looked: 0, of: 0).fraction, 0)
  }
}
