// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-14): search on the phone "doesn't seem to be updating anymore". The
// real app pairs with `driftwire serve` on loopback and copies a first catalogue that has the
// folder's photos only; a search for clips finds nothing. tools/ios/sync-live.sh then publishes a
// second generation with the clips while the app stays open; the app comes back to the foreground,
// syncs it, and the search on screen must answer from it without anyone typing again, as must the
// library's grid. Runs only through that script (MV_SYNC_PAYLOAD).
import XCTest

final class LiveSyncTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private var env: [String: String] { ProcessInfo.processInfo.environment }
  private let tilePredicate = NSPredicate(format: "identifier BEGINSWITH 'tile:'")
  private let clipTile = NSPredicate(format: "identifier BEGINSWITH 'tile:' AND label BEGINSWITH 'Clip,'")

  private func save(_ name: String) {
    guard let dir = env["MV_SYNC_OUT"] else { return }
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
  }

  private func mark(_ name: String, _ text: String = "") {
    guard let dir = env["MV_SYNC_OUT"] else { return }
    try? text.write(toFile: dir + "/" + name, atomically: true, encoding: .utf8)
  }

  private func waitForFile(_ name: String, timeout: TimeInterval) -> Bool {
    guard let dir = env["MV_SYNC_OUT"] else { return false }
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
      if FileManager.default.fileExists(atPath: dir + "/" + name) { return true }
      usleep(500_000)
    }
    return false
  }

  func testANewCatalogueReachesTheSearchOnScreen() throws {
    guard let code = env["MV_SYNC_PAYLOAD"], env["MV_SYNC_OUT"] != nil else {
      throw XCTSkip("Run through tools/ios/sync-live.sh")
    }
    let query = env["MV_SYNC_QUERY"].flatMap { $0.isEmpty ? nil : $0 } ?? "mountain is:video"
    let app = XCUIApplication(bundleIdentifier: bundle)
    app.launchEnvironment["MV_TEST_SCAN_CODE"] = code
    app.launch()
    let match = app.buttons["They match"]
    XCTAssertTrue(match.waitForExistence(timeout: 60), "The code did not reach the check screen")
    match.tap()
    let carryOn = app.buttons["Continue"]
    let tiles = app.buttons.matching(tilePredicate)
    let deadline = Date().addingTimeInterval(600)
    while Date() < deadline, tiles.count == 0 {
      if carryOn.exists { carryOn.tap() }
      usleep(250_000)
    }
    XCTAssertGreaterThan(tiles.count, 0, "The grid did not appear after the copy")
    sleep(3)
    let firstGrid = app.buttons.matching(clipTile).count
    save("01-grid-gen1")

    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    field.tap()
    field.typeText(query + "\n")
    // Clips only (`is:video`, the words the Kind filter adds): the first catalogue's clips have no
    // pictures to match, so it says "Nothing found"; the second answers.
    let nothing = app.staticTexts["Nothing found for “\(query)”."]
    XCTAssertTrue(nothing.waitForExistence(timeout: 60), "The first catalogue did not answer")
    sleep(1)
    let before = app.buttons.matching(clipTile).count
    let beforeTotal = tiles.firstMatch.identifier.split(separator: "|").dropFirst().first.map(String.init) ?? "?"
    save("02-search-gen1")
    print("LIVE-SYNC gen1: \(before) clip tiles for \(query) (total \(beforeTotal)), \(firstGrid) clips in the grid")
    XCTAssertEqual(before, 0, "The first catalogue was meant to have no clips")

    // The script publishes the second generation now; the app syncs it on coming back.
    mark("phone-ready")
    XCTAssertTrue(waitForFile("gen2-published", timeout: 1800), "The second generation was not published")
    XCUIDevice.shared.press(.home)
    sleep(2)
    app.activate()

    // The search on screen, never typed again: the clips arrive in it once the sync lands.
    var after = 0
    var afterTotal = beforeTotal
    let wait = Date()
    while Date().timeIntervalSince(wait) < 180 {
      after = app.buttons.matching(clipTile).count
      afterTotal = tiles.firstMatch.identifier.split(separator: "|").dropFirst().first.map(String.init) ?? "?"
      if after > 0 { break }
      usleep(500_000)
    }
    let took = Date().timeIntervalSince(wait)
    save("03-search-gen2")
    print("LIVE-SYNC gen2: \(after) clip tiles for \(query) (total \(afterTotal)) after \(Int(took)) s")

    // The library's grid follows too.
    app.buttons["Library"].firstMatch.tap()
    sleep(2)
    let grid = app.buttons.matching(clipTile).count
    save("04-grid-gen2")
    // And a search typed afresh: what the catalogue answers now, whatever the screen kept.
    app.buttons["Search"].firstMatch.tap()
    field.tap()
    if field.buttons["Clear text"].exists { field.buttons["Clear text"].tap() }
    field.typeText(query + "\n")
    sleep(4)
    save("05-search-fresh")
    let fresh = app.buttons.matching(clipTile).count
    print("LIVE-SYNC grid: \(grid) clips; fresh search: \(fresh) clip tiles")
    mark("result.txt", "search_kept=\(after) total_before=\(beforeTotal) total_after=\(afterTotal) seconds=\(Int(took)) grid=\(grid) fresh=\(fresh)\n")
    XCTAssertGreaterThan(after, 0, "The search on screen still answers from the first catalogue")
  }
}
