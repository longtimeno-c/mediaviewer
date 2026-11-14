// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-12): "when searching the images flicker". Types the same queries a
// letter at a time into the real app on the sample library, so a before and an after build can be
// recorded (simctl io recordVideo) and traced (Instruments' Animation Hitches) doing exactly the
// same thing. Runs only through tools/ios/search-smoothness.sh, which sets MV_SEARCH_SMOOTHNESS;
// it opens the sample library first if the app has none.
import XCTest

final class SearchSmoothnessTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private let gridTile = NSPredicate(format: "label BEGINSWITH 'Photo,' OR label BEGINSWITH 'Clip,'")
  private let tilePredicate = NSPredicate(format: "identifier BEGINSWITH 'tile:'")
  private var env: [String: String] { ProcessInfo.processInfo.environment }

  private func launch() throws -> XCUIApplication {
    guard env["MV_SEARCH_SMOOTHNESS"] != nil else { throw XCTSkip("Run through tools/ios/search-smoothness.sh") }
    let app = XCUIApplication(bundleIdentifier: bundle)
    if let base = env["MV_SAMPLE_BASE"], !base.isEmpty { app.launchEnvironment["MV_SAMPLE_BASE"] = base }
    app.launch()
    return app
  }

  /// Opens the sample library when the app has no library yet.
  private func ensureLibrary(_ app: XCUIApplication) {
    let tiles = app.buttons.matching(gridTile)
    if tiles.firstMatch.waitForExistence(timeout: 8) { return }
    if app.buttons["Scan code"].waitForExistence(timeout: 5) { app.buttons["Scan code"].tap() }
    let enter = app.buttons["Enter a code"]
    XCTAssertTrue(enter.waitForExistence(timeout: 10), "The Scan screen has no Enter a code")
    enter.tap()
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    field.typeText("sample")
    app.alerts.buttons["Open"].tap()
    let open = app.buttons["Open sample library"]
    XCTAssertTrue(open.waitForExistence(timeout: 30), "SAMPLE did not reach the sample library's page")
    open.tap()
    XCTAssertTrue(tiles.firstMatch.waitForExistence(timeout: 600), "The grid did not appear after the copy")
    sleep(3)
  }

  /// Waits until every visible tile answers `query` (their identifiers carry the words).
  private func waitForAnswer(_ app: XCUIApplication, _ query: String) -> TimeInterval? {
    let start = Date()
    while Date().timeIntervalSince(start) < 20 {
      let tiles = app.buttons.matching(tilePredicate).allElementsBoundByIndex.prefix(6)
      if !tiles.isEmpty, tiles.allSatisfy({ $0.identifier.hasSuffix("|" + query) }) { return Date().timeIntervalSince(start) }
      // Named by the words it answers: an earlier query's stays on screen while this one is typed.
      if app.staticTexts["Nothing found for “\(query)”."].exists { return Date().timeIntervalSince(start) }
      usleep(50_000)
    }
    return nil
  }

  func testTypingQueries() throws {
    let app = try launch()
    ensureLibrary(app)
    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    // Warm the engine first, so the recording measures typing and not the model's first load.
    field.tap()
    field.typeText("dog")
    _ = waitForAnswer(app, "dog")
    field.buttons["Clear text"].tap()
    sleep(1)
    // The mark the recording script waits for before it starts.
    try? "ready".write(toFile: (env["MV_SEARCH_OUT"] ?? NSTemporaryDirectory()) + "/ready", atomically: true, encoding: .utf8)
    sleep(UInt32(env["MV_SEARCH_LEAD"] ?? "3") ?? 3)
    // From the last key to every visible tile answering it, as the test sees it (its polling adds
    // the same overhead to both builds being compared).
    var latencies: [String] = []
    for query in ["snowy mountain", "lake reflection", "bridge at night"] {
      if field.buttons["Clear text"].exists { field.buttons["Clear text"].tap() }
      // About 6 keys a second, a quick typist on a phone.
      for letter in query { field.typeText(String(letter)); usleep(160_000) }
      let latency = waitForAnswer(app, query)
      XCTAssertNotNil(latency, "No answer for \(query)")
      latencies.append("\(query)\t\(Int((latency ?? -1) * 1000))")
      sleep(1)
      // Refine: take two letters back, then put them back.
      field.typeText(XCUIKeyboardKey.delete.rawValue + XCUIKeyboardKey.delete.rawValue)
      usleep(400_000)
      field.typeText(String(query.suffix(2)))
      XCTAssertNotNil(waitForAnswer(app, query), "No answer for \(query) after refining")
      sleep(1)
    }
    try? latencies.joined(separator: "\n").write(toFile: (env["MV_SEARCH_OUT"] ?? NSTemporaryDirectory()) + "/latency.txt",
                                                 atomically: true, encoding: .utf8)
    try? "done".write(toFile: (env["MV_SEARCH_OUT"] ?? NSTemporaryDirectory()) + "/done", atomically: true, encoding: .utf8)
  }
}
