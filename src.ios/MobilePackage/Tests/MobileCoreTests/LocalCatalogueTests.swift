// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// LocalCatalogue's swaps, tiles and warm-up over real generations the publisher builds
// (tests/mobile_generation_fixture.cpp), in the Simulator: tools/ios/run-mobilecore-tests.sh.
import CMobileSearch
import CMobileTestFixture
import MobileKit
import UIKit
import XCTest
@testable import MobileCore

final class LocalCatalogueTests: XCTestCase {
  private var scratch: URL!

  override func setUpWithError() throws {
    scratch = FileManager.default.temporaryDirectory.appendingPathComponent("mobilecore-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: scratch, withIntermediateDirectories: true)
  }

  override func tearDownWithError() throws {
    try? FileManager.default.removeItem(at: scratch)
  }

  /// A library as the sync installs one: gen/1-1 (variant 0) and gen/1-2 (variant 1, the same
  /// asset ids with another file, place and tile), installed.json naming the first.
  override func tearDown() {
    LocalCatalogue.beforeOpen = nil
    super.tearDown()
  }

  private func library(installed: Int = 1) throws -> (LibraryLocation, first: URL, second: URL) {
    let root = scratch.appendingPathComponent("library", isDirectory: true)
    try FileManager.default.createDirectory(at: root.appendingPathComponent("gen"), withIntermediateDirectories: true)
    var dirs: [URL] = []
    for (variant, generation) in [(Int32(0), UInt64(1)), (Int32(1), UInt64(2))] {
      var gen = [CChar](repeating: 0, count: 4096), lib = [CChar](repeating: 0, count: 4096)
      let made = scratch.appendingPathComponent("made-\(generation)").path
      XCTAssertEqual(mv_mobile_test_make_generation(made, variant, 1, generation, nil, nil, &gen, 4096, &lib, 4096), 0)
      let to = root.appendingPathComponent("gen/1-\(generation)", isDirectory: true)
      try FileManager.default.copyItem(at: URL(fileURLWithPath: String(cString: gen)), to: to)
      let models = root.appendingPathComponent("models")
      if !FileManager.default.fileExists(atPath: models.path) {
        try FileManager.default.copyItem(at: URL(fileURLWithPath: String(cString: lib)).appendingPathComponent("models"), to: models)
      }
      dirs.append(to)
    }
    try Data(#"{"library_id":"library","epoch":1,"generation":\#(installed),"dir":"1-\#(installed)"}"#.utf8)
      .write(to: root.appendingPathComponent("installed.json"))
    return (LibraryLocation(root: root), dirs[0], dirs[1])
  }

  private func item(named name: String, in list: any MediaList) throws -> MediaItem {
    for i in 0..<list.count where list.item(at: i).fileName == name { return list.item(at: i) }
    throw XCTSkip("no \(name)")
  }

  /// #7: two swaps finishing out of order: the older one, finishing last, must not win.
  func testAnOlderGenerationFinishingLastDoesNotReplaceANewerOne() async throws {
    let (location, first, second) = try library()
    let catalogue = LocalCatalogue(location: location)
    try await catalogue.swap(to: second)
    try await catalogue.swap(to: first)
    XCTAssertEqual(catalogue.generationInForce()?.number, 2)
    let list = try await catalogue.library.list(in: .everything)
    XCTAssertNoThrow(try item(named: "z.jpg", in: list))  // the newer one's file
  }

  /// #8: an item from a list taken before a swap answers from its own generation.
  func testAPreSwapListKeepsItsOwnTilesAndDetails() async throws {
    let (location, _, second) = try library()
    let catalogue = LocalCatalogue(location: location)
    let before = try await catalogue.library.list(in: .everything)
    let old = try item(named: "a.jpg", in: before)
    try await catalogue.swap(to: second)
    XCTAssertEqual(catalogue.generationInForce()?.number, 2)

    let details = try await catalogue.library.details(for: old)
    XCTAssertEqual(details.place, "Ortisei, South Tyrol, Italy")
    let tile = await catalogue.images.thumbnail(for: old)
    XCTAssertEqual(tile?.size, CGSize(width: 48, height: 32))  // variant 0's tile, not 1's 32 x 48

    let after = try await catalogue.library.list(in: .everything)
    let new = try item(named: "z.jpg", in: after)
    XCTAssertEqual(old.id.raw, new.id.raw)
    XCTAssertNotEqual(old.id, new.id)
    let newTile = await catalogue.images.thumbnail(for: new)
    XCTAssertEqual(newTile?.size, CGSize(width: 32, height: 48))
    withExtendedLifetime(before) {}
  }

  /// #10: opening the grid starts nothing; the first tile does, after it has painted.
  func testWarmUpWaitsForTheFirstTile() async throws {
    let (location, _, _) = try library()
    let catalogue = LocalCatalogue(location: location)
    let list = try await catalogue.library.list(in: .everything)
    let g = try XCTUnwrap(catalogue.generationInForce())
    try await Task.sleep(for: .milliseconds(500))
    XCTAssertEqual(mv_mobile_search_state(g.raw), MV_MOBILE_SEARCH_OFF, "nothing starts before a tile has painted")

    _ = await catalogue.images.thumbnail(for: list.item(at: 0))
    var state = mv_mobile_search_state(g.raw)
    for _ in 0..<250 where state == MV_MOBILE_SEARCH_OFF {
      try await Task.sleep(for: .milliseconds(20))
      state = mv_mobile_search_state(g.raw)
    }
    XCTAssertNotEqual(state, MV_MOBILE_SEARCH_OFF, "the first tile starts the engine")
  }

  /// An older swap landing while the first open (of a newer generation) is still in flight.
  func testAnOlderSwapDuringTheFirstOpenDoesNotWin() async throws {
    let (location, first, second) = try library(installed: 2)
    let slow = second.lastPathComponent
    LocalCatalogue.beforeOpen = { dir in if dir.lastPathComponent == slow { Thread.sleep(forTimeInterval: 0.4) } }
    let catalogue = LocalCatalogue(location: location)
    let opening = Task { try await catalogue.generation() }
    try await Task.sleep(for: .milliseconds(100))  // the first open is inside its slow part
    try await catalogue.swap(to: first)
    XCTAssertEqual(catalogue.generationInForce()?.number, 2, "the newer first open is in force once the swap returns")
    _ = try await opening.value
    XCTAssertEqual(catalogue.generationInForce()?.number, 2)
  }

  /// Prefetch and an old generation's tiles are not a first paint of the generation in force.
  func testOnlyAPaintInTheGenerationInForceWarmsUp() async throws {
    let (location, _, second) = try library()
    let catalogue = LocalCatalogue(location: location)
    let before = try await catalogue.library.list(in: .everything)
    let old = try XCTUnwrap(catalogue.generationInForce())
    catalogue.images.prefetch((0..<before.count).map { before.item(at: $0) }, fittingPixels: CGSize(width: 64, height: 64))
    try await Task.sleep(for: .milliseconds(500))
    XCTAssertEqual(mv_mobile_search_state(old.raw), MV_MOBILE_SEARCH_OFF, "prefetch is not a paint")

    try await catalogue.swap(to: second)
    let new = try XCTUnwrap(catalogue.generationInForce())
    _ = await catalogue.images.thumbnail(for: try item(named: "a.jpg", in: before))
    try await Task.sleep(for: .milliseconds(500))
    XCTAssertEqual(mv_mobile_search_state(old.raw), MV_MOBILE_SEARCH_OFF, "an old generation's tile starts nothing")
    XCTAssertEqual(mv_mobile_search_state(new.raw), MV_MOBILE_SEARCH_OFF)
    withExtendedLifetime(before) {}
  }
}
