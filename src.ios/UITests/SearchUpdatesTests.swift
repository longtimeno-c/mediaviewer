// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-14): search on the phone "doesn't seem to be updating anymore". Types
// queries into the real app on the sample library the way people do — a letter at a time, quick and
// slow, backspacing, changing a filter — and checks that what is on screen always ends up answering
// the words in the field. Runs only with MV_SEARCH_UPDATES set (and MV_SAMPLE_BASE for a Debug build).
import XCTest

final class SearchUpdatesTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private let gridTile = NSPredicate(format: "label BEGINSWITH 'Photo,' OR label BEGINSWITH 'Clip,'")
  private let tilePredicate = NSPredicate(format: "identifier BEGINSWITH 'tile:'")
  private var env: [String: String] { ProcessInfo.processInfo.environment }

  private func launch() throws -> XCUIApplication {
    guard env["MV_SEARCH_UPDATES"] != nil else { throw XCTSkip("Set MV_SEARCH_UPDATES to run") }
    let app = XCUIApplication(bundleIdentifier: bundle)
    if let base = env["MV_SAMPLE_BASE"], !base.isEmpty { app.launchEnvironment["MV_SAMPLE_BASE"] = base }
    app.launch()
    return app
  }

  private func ensureLibrary(_ app: XCUIApplication) {
    let tiles = app.buttons.matching(gridTile)
    if tiles.firstMatch.waitForExistence(timeout: 8) { return }
    if app.buttons["Scan code"].waitForExistence(timeout: 5) { app.buttons["Scan code"].tap() }
    let enter = app.buttons["Enter a code"]
    XCTAssertTrue(enter.waitForExistence(timeout: 10))
    enter.tap()
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    field.typeText("sample")
    app.alerts.buttons["Open"].tap()
    let open = app.buttons["Open sample library"]
    XCTAssertTrue(open.waitForExistence(timeout: 30))
    open.tap()
    XCTAssertTrue(tiles.firstMatch.waitForExistence(timeout: 600))
    sleep(3)
  }

  /// What the screen answers: the words in the first visible tile's identifier, or the words a
  /// "Nothing found" names.
  private func answered(_ app: XCUIApplication) -> String? {
    if let tile = app.buttons.matching(tilePredicate).allElementsBoundByIndex.first(where: { $0.exists }) {
      let parts = tile.identifier.dropFirst(5).split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false)
      if parts.count > 2 { return String(parts[2]) }
    }
    let nothing = app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH 'Nothing found for'")).firstMatch
    if nothing.exists {
      return nothing.label.replacingOccurrences(of: "Nothing found for “", with: "").replacingOccurrences(of: "”.", with: "")
    }
    return nil
  }

  private func waitFor(_ app: XCUIApplication, _ query: String, timeout: TimeInterval = 15) -> TimeInterval? {
    let start = Date()
    while Date().timeIntervalSince(start) < timeout {
      if answered(app) == query { return Date().timeIntervalSince(start) }
      usleep(50_000)
    }
    return nil
  }

  func testTheScreenFollowsTheField() throws {
    let app = try launch()
    ensureLibrary(app)
    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    field.tap()
    var log: [String] = []
    var stale = 0
    func check(_ query: String, _ how: String) {
      let t = waitFor(app, query)
      let line = "\(how)\t\(query)\t" + (t.map { "\(Int($0 * 1000)) ms" } ?? "STALE: screen answers “\(answered(app) ?? "nothing")”")
      log.append(line)
      print("SEARCH-UPDATES " + line)
      if t == nil { stale += 1 }
    }
    func clear() { if field.buttons["Clear text"].exists { field.buttons["Clear text"].tap() } }
    // Each round: quick, slow, a pause mid-word, backspacing into another query.
    for round in 0..<3 {
      for (query, gap) in [("dog on a beach", 60_000), ("waterfall", 250_000), ("bridge at night", 30_000),
                           ("breakfast", 120_000), ("snowy mountain", 0), ("lake reflection", 90_000)] as [(String, useconds_t)] {
        clear()
        if gap == 0 { field.typeText(query) } else { for c in query { field.typeText(String(c)); usleep(gap) } }
        check(query, "r\(round) typed")
      }
      // Backspace "lake reflection" to "lake", then add " sunset".
      for _ in 0..<" reflection".count { field.typeText(XCUIKeyboardKey.delete.rawValue); usleep(40_000) }
      check("lake", "r\(round) backspaced")
      field.typeText(" sunset")
      check("lake sunset", "r\(round) extended")
      // A result opened and closed, then a new query.
      clear()
      field.typeText("dog")
      check("dog", "r\(round) before viewer")
      app.buttons.matching(tilePredicate).firstMatch.tap()
      sleep(2)
      if app.buttons["Close"].firstMatch.exists { app.buttons["Close"].firstMatch.tap() } else { app.swipeDown(velocity: .fast) }
      sleep(1)
      field.tap()
      clear()
      field.typeText("beach")
      check("beach", "r\(round) after viewer")
      // Leave the tab and come back.
      app.buttons["Library"].firstMatch.tap()
      sleep(1)
      app.buttons["Search"].firstMatch.tap()
      field.tap()
      clear()
      field.typeText("mountain")
      check("mountain", "r\(round) after tab switch")
    }
    if let dir = env["MV_SEARCH_OUT"] {
      try? log.joined(separator: "\n").write(toFile: dir + "/search-updates.txt", atomically: true, encoding: .utf8)
    }
    XCTAssertEqual(stale, 0, "The screen did not follow the field:\n" + log.joined(separator: "\n"))
  }
}
