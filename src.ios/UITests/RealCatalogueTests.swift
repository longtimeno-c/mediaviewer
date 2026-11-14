// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md M5 and M6's local half, through the real app and a real installed
// generation. Skipped unless TEST_RUNNER_MV_REAL_CATALOGUE names a folder for its screenshots:
// the real app must be installed with a generation in Application Support/library first.
import XCTest

final class RealCatalogueTests: XCTestCase {
  private var shots: String { ProcessInfo.processInfo.environment["MV_REAL_CATALOGUE"] ?? "" }

  private func save(_ name: String) throws {
    try XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: shots).appendingPathComponent(name))
  }

  func testGridAndSearchAnswerFromTheCatalogue() throws {
    try XCTSkipIf(shots.isEmpty, "needs a real installed generation")
    let app = XCUIApplication(bundleIdentifier: "io.github.longtimeno-c.mediaviewer.mobile")
    app.terminate()
    app.launch()

    let tile = NSPredicate(format: "label BEGINSWITH 'Clip,' OR label BEGINSWITH 'Photo,'")
    XCTAssertTrue(app.buttons.matching(tile).firstMatch.waitForExistence(timeout: 20), "the library grid has rows")
    try save("grid.png")

    app.buttons["Search"].firstMatch.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    field.tap()
    field.typeText("mountains\n")
    XCTAssertTrue(app.buttons.matching(tile).firstMatch.waitForExistence(timeout: 30), "a search answers")
    XCTAssertFalse(app.staticTexts["Search isn't working right now."].exists)
    try save("search.png")
  }
}
