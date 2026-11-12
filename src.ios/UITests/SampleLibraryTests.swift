// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §11.1: the sample library, as App Review meets it. A wrong code is "not
// readable"; SAMPLE, typed on the Scan screen, opens the sample library; the real sync copies its
// signed catalogue; the grid, search, the viewer and a clip answer; Forget removes it. Runs only
// through tools/ios/sample-library-sim.sh, which serves the folder on loopback (MV_SAMPLE_BASE) and
// checks the answers against the pictures' tags afterwards.
import XCTest

final class SampleLibraryTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private var env: [String: String] { ProcessInfo.processInfo.environment }
  private let tilePredicate = NSPredicate(format: "identifier BEGINSWITH 'tile:'")
  private let gridTile = NSPredicate(format: "label BEGINSWITH 'Photo,' OR label BEGINSWITH 'Clip,'")

  private func save(_ name: String) {
    guard let dir = env["MV_SAMPLE_OUT"] else { return }
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
  }

  private func launch() throws -> XCUIApplication {
    guard let base = env["MV_SAMPLE_BASE"], env["MV_SAMPLE_OUT"] != nil else {
      throw XCTSkip("Run through tools/ios/sample-library-sim.sh")
    }
    let app = XCUIApplication(bundleIdentifier: bundle)
    app.launchEnvironment["MV_SAMPLE_BASE"] = base
    app.launch()
    return app
  }

  private func enterCode(_ app: XCUIApplication, _ code: String) {
    if app.buttons["Scan code"].waitForExistence(timeout: 5) { app.buttons["Scan code"].tap() }
    let enter = app.buttons["Enter a code"]
    XCTAssertTrue(enter.waitForExistence(timeout: 10), "The Scan screen has no Enter a code")
    enter.tap()
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    field.typeText(code)
    app.alerts.buttons["Open"].tap()
  }

  func test1_AWrongCodeIsNotReadable() throws {
    let app = try launch()
    enterCode(app, "SAMPLE-LIBRARY")
    let words = app.staticTexts["That isn't a code MediaViewer can read. Scan the one shown by MediaViewer on your Mac or Windows PC."]
    XCTAssertTrue(words.waitForExistence(timeout: 15), "A wrong code did not say it was not readable")
    save("01-wrong-code")
  }

  func test2_TheCodeOpensTheSampleLibrary() throws {
    let app = try launch()
    enterCode(app, "sample")
    let open = app.buttons["Open sample library"]
    XCTAssertTrue(open.waitForExistence(timeout: 15), "SAMPLE did not reach the sample library's page")
    XCTAssertTrue(app.staticTexts["Sample library"].exists, "The page does not name the sample library")
    save("02-open-sample")
    open.tap()

    let tiles = app.buttons.matching(gridTile)
    let deadline = Date().addingTimeInterval(600)
    var sawCopy = false
    while Date() < deadline, tiles.count == 0 {
      if !sawCopy, app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH 'Copying library'")).count > 0 {
        sawCopy = true
        save("03-copying")
      }
      usleep(250_000)
    }
    XCTAssertGreaterThan(tiles.count, 0, "The grid did not appear after the copy")
    // Labelled on every visit, as the source of what is shown.
    XCTAssertTrue(app.buttons["Sample library"].waitForExistence(timeout: 5), "The library is not labelled as the sample")
    sleep(3)  // JPEG-512s arrive
    save("04-grid")
  }

  func test3_SearchAnswersFromTheRealEngine() throws {
    let app = try launch()
    XCTAssertTrue(app.buttons.matching(gridTile).firstMatch.waitForExistence(timeout: 30), "No sample library after relaunch")
    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    var answers: [[String: Any]] = []
    for (n, query) in ["dog on a beach", "waterfall", "bridge at night", "breakfast", "snowy mountain", "lake reflection"].enumerated() {
      field.tap()
      if field.buttons["Clear text"].exists { field.buttons["Clear text"].tap() }
      field.typeText(query + "\n")
      var shown: [String] = []
      let start = Date()
      while Date().timeIntervalSince(start) < 60 {
        usleep(300_000)
        let tiles = app.buttons.matching(tilePredicate).allElementsBoundByIndex.filter { $0.exists && $0.isHittable }
        let parsed = tiles.map { $0.identifier.dropFirst(5).split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false).map(String.init) }
        if !parsed.isEmpty, parsed.allSatisfy({ $0.count > 2 && $0[2] == query }) {
          shown = tiles.sorted { abs($0.frame.minY - $1.frame.minY) > 4 ? $0.frame.minY < $1.frame.minY : $0.frame.minX < $1.frame.minX }
            .map { String($0.identifier.dropFirst(5).split(separator: "|").first ?? "") }
          break
        }
      }
      XCTAssertFalse(shown.isEmpty, "No results for \(query)")
      XCTAssertFalse(app.staticTexts["Search isn't working right now."].exists)
      answers.append(["query": query, "tiles": shown])
      save(String(format: "05-search-%d", n))
    }
    let out = URL(fileURLWithPath: env["MV_SAMPLE_OUT"]!).appendingPathComponent("answers.json")
    try JSONSerialization.data(withJSONObject: answers, options: [.prettyPrinted]).write(to: out)
  }

  func test4_ThePhotoAndTheClipOpen() throws {
    let app = try launch()
    let photo = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Photo,'")).firstMatch
    XCTAssertTrue(photo.waitForExistence(timeout: 30))
    photo.tap()
    XCTAssertTrue(app.buttons["viewer-close"].waitForExistence(timeout: 15), "The photo did not open")
    sleep(5)  // the original from the static folder
    XCTAssertFalse(app.staticTexts[ "Can't reach the sample library. You can keep searching what's on this phone."].exists)
    save("06-photo")
    app.buttons["viewer-close"].tap()
    sleep(1)
    let clip = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Clip,'")).firstMatch
    if !clip.exists { app.swipeUp() }
    XCTAssertTrue(clip.waitForExistence(timeout: 10), "No clip in the grid")
    clip.tap()
    XCTAssertTrue(app.buttons["Pause"].waitForExistence(timeout: 30), "The clip did not start")
    sleep(2)
    save("07-clip")
  }

  func test5_ForgetRemovesIt() throws {
    let app = try launch()
    XCTAssertTrue(app.buttons.matching(gridTile).firstMatch.waitForExistence(timeout: 30))
    app.buttons["Sample library"].tap()  // the chip opens Settings
    XCTAssertTrue(app.staticTexts["Sample library"].waitForExistence(timeout: 10))
    save("08-settings")
    let forget = app.buttons["Forget the sample library"]
    if !forget.isHittable { app.swipeUp() }
    forget.tap()
    app.buttons["Forget"].firstMatch.tap()
    XCTAssertTrue(app.buttons["Scan code"].waitForExistence(timeout: 15), "Forget did not return to Welcome")
    save("09-forgotten")
  }
}
