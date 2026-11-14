// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import MobileLinkSupport
import XCTest

final class ScopedCacheTests: XCTestCase {
  private final class Picture {}

  /// A new generation can reuse an asset id for another picture.
  func testAnotherGenerationDoesNotShareAnId() {
    let cache = ScopedCache<Picture>()
    _ = cache.object(library: "home", generation: 1, id: "31")  // a lookup first, as the app does
    cache.set(Picture(), library: "home", generation: 1, id: "31")
    XCTAssertNotNil(cache.object(library: "home", generation: 1, id: "31"))
    XCTAssertNil(cache.object(library: "home", generation: 2, id: "31"))
  }

  /// After Forget and pairing another library, none of the first library's pictures remain.
  func testAnotherLibraryEmptiesIt() {
    let cache = ScopedCache<Picture>()
    _ = cache.object(library: "home", generation: 1, id: "31")  // a lookup first, as the app does
    cache.set(Picture(), library: "home", generation: 1, id: "31")
    XCTAssertNil(cache.object(library: "office", generation: 1, id: "31"))
    XCTAssertNil(cache.object(library: "home", generation: 1, id: "31"), "the first library's entries are gone, not set aside")
  }

  func testRemoveAllEmptiesIt() {
    let cache = ScopedCache<Picture>()
    _ = cache.object(library: "home", generation: 1, id: "31")  // a lookup first, as the app does
    cache.set(Picture(), library: "home", generation: 1, id: "31")
    cache.removeAll()
    XCTAssertNil(cache.object(library: "home", generation: 1, id: "31"))
  }

  /// A picture that finishes loading after Forget does not bring the forgotten library back.
  func testALateSetAfterForgetIsDropped() {
    let cache = ScopedCache<Picture>()
    _ = cache.object(library: "home", generation: 1, id: "31")
    cache.removeAll()
    cache.set(Picture(), library: "home", generation: 1, id: "32")
    XCTAssertNil(cache.object(library: "home", generation: 1, id: "32"))
  }
}
