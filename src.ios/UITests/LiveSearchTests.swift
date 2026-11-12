// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §9's parity test, live: the real app pairs with `driftwire serve` on
// loopback, copies the catalogue the Mac's own publisher made, and answers each query on its search
// screen. Writes the tiles it shows, in order, for tools/ios/search-live.sh to compare with the
// Mac's answers. Runs only when MV_LIVE_PAYLOAD is set. With MV_LIVE_SOUND_BASE (a Debug app), sound
// search's model comes from that folder ("release": the app's own, from the project's release), and
// the queries wait until Settings says it is on.
import XCTest

final class LiveSearchTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private var env: [String: String] { ProcessInfo.processInfo.environment }

  private func save(_ name: String) {
    guard let dir = env["MV_LIVE_OUT"] else { return }
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
  }

  private let tilePredicate = NSPredicate(format: "identifier BEGINSWITH 'tile:'")

  func testPairCopyThenSearch() throws {
    guard let code = env["MV_LIVE_PAYLOAD"], let queriesFile = env["MV_LIVE_QUERIES"], let outDir = env["MV_LIVE_OUT"] else {
      throw XCTSkip("Run through tools/ios/search-live.sh")
    }
    let queries = try String(contentsOfFile: queriesFile, encoding: .utf8)
      .split(separator: "\n").map(String.init).filter { !$0.isEmpty && !$0.hasPrefix("#") }
    let app = XCUIApplication(bundleIdentifier: bundle)
    app.launchEnvironment["MV_TEST_SCAN_CODE"] = code
    let sound = env["MV_LIVE_SOUND_BASE"].flatMap { $0.isEmpty ? nil : $0 }
    if let sound, sound != "release" { app.launchEnvironment["MV_SOUND_BASE"] = sound }
    app.launch()

    let match = app.buttons["They match"]
    XCTAssertTrue(match.waitForExistence(timeout: 60), "The code did not reach the check screen")
    save("01-check")
    match.tap()
    let carryOn = app.buttons["Continue"]
    let tiles = app.buttons.matching(tilePredicate)
    let deadline = Date().addingTimeInterval(600)
    while Date() < deadline, tiles.count == 0 {
      if carryOn.exists { save("02-outside-skipped"); carryOn.tap() }
      usleep(250_000)
    }
    XCTAssertGreaterThan(tiles.count, 0, "The grid did not appear after the copy")
    sleep(3)
    save("03-grid")

    if sound != nil {
      app.buttons["Settings"].firstMatch.tap()
      let on = app.descendants(matching: .any).matching(NSPredicate(format: "label == 'Sound search, On'")).firstMatch
      XCTAssertTrue(on.waitForExistence(timeout: 600), "Sound search did not come on")
      save("04-sound-on")
      sleep(5)  // the catalogue opens again with it, beside the one without
      app.buttons["Library"].firstMatch.tap()
    }

    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    var answers: [[String: Any]] = []
    for (n, query) in queries.enumerated() {
      field.tap()
      if let clear = field.buttons["Clear text"].firstMatch as XCUIElement?, clear.exists { clear.tap() }
      field.typeText(query + "\n")
      // Answered: every tile on screen says it answers these words, or the "Nothing" page names them.
      var last: [String] = []
      var total = 0
      var answered = false
      let start = Date()
      while Date().timeIntervalSince(start) < 60 {
        usleep(300_000)
        let tiles = visibleTiles(app)
        let mine = !tiles.isEmpty && tiles.allSatisfy { $0.answering == query }
        let nothing = app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH 'Nothing' AND label CONTAINS %@", query)).count > 0
        if mine || (nothing && tiles.isEmpty) {
          last = tiles.map(\.id)
          total = tiles.first?.total ?? 0
          answered = true
          break
        }
      }
      XCTAssertTrue(answered, "No answer on screen for \(query)")
      answers.append(["query": query, "tiles": last, "total": total])
      if n < 16 { save(String(format: "q%02d", n)) }
    }
    let data = try JSONSerialization.data(withJSONObject: answers, options: [.prettyPrinted])
    try data.write(to: URL(fileURLWithPath: outDir).appendingPathComponent("phone-answers.json"))
  }

  /// The tiles on screen, in reading order: asset id, how many results there are in all, and the
  /// words they answer.
  private func visibleTiles(_ app: XCUIApplication) -> [(id: String, total: Int, answering: String)] {
    let window = app.windows.firstMatch.frame
    return app.buttons.matching(tilePredicate).allElementsBoundByIndex
      .filter { $0.exists && $0.isHittable && window.intersects($0.frame) }
      .sorted { abs($0.frame.minY - $1.frame.minY) > 4 ? $0.frame.minY < $1.frame.minY : $0.frame.minX < $1.frame.minX }
      .map { element in
        // "tile:<asset>|<total>|<words>"
        let parts = element.identifier.dropFirst(5).split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false).map(String.init)
        return (parts.first ?? "", parts.count > 1 ? Int(parts[1]) ?? 0 : 0, parts.count > 2 ? parts[2] : "")
      }
  }
}
