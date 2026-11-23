// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library's demo of changing files (owner: every phone feature can be tried with SAMPLE):
// in the real app, the sample's folder says changes stay on this iPhone, and New Folder, Rename,
// Move, Delete and Add from Files each land on its page. Opens the sample library from the release
// when the app has none, as SearchSmoothnessTests does. Runs with MV_SAMPLE_CHANGES set
// (TEST_RUNNER_MV_SAMPLE_CHANGES=1); Add from Files picks "Trip notes" from the fixtures app's folder
// in Files, so the fixtures app must have been opened once in library.change on this Simulator.
import XCTest

final class SampleChangesTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private let gridTile = NSPredicate(format: "identifier BEGINSWITH 'tile:'")
  private var env: [String: String] { ProcessInfo.processInfo.environment }

  private func launch() throws -> XCUIApplication {
    guard env["MV_SAMPLE_CHANGES"] != nil else { throw XCTSkip("Set TEST_RUNNER_MV_SAMPLE_CHANGES=1") }
    let app = XCUIApplication(bundleIdentifier: bundle)
    if let base = env["MV_SAMPLE_BASE"], !base.isEmpty { app.launchEnvironment["MV_SAMPLE_BASE"] = base }
    app.launch()
    if !app.buttons.matching(gridTile).firstMatch.waitForExistence(timeout: 10) {
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
      XCTAssertTrue(app.buttons.matching(gridTile).firstMatch.waitForExistence(timeout: 600), "No sample library")
    }
    return app
  }

  private func fill(_ app: XCUIApplication, _ text: String) {
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5))
    let old = (field.value as? String).flatMap { $0 == field.placeholderValue ? "" : $0 } ?? ""
    field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: old.count) + text)
  }

  func testTheSampleCanBeChangedOnThisPhone() throws {
    let app = try launch()
    // The sample's own folder (not the offline drive beside it).
    let sample = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'folder:/' AND label BEGINSWITH 'Sample library'")).firstMatch
    XCTAssertTrue(sample.waitForExistence(timeout: 30))
    let sampleID = String(sample.identifier.dropFirst("folder:".count))
    sample.tap()
    XCTAssertTrue(app.navigationBars["Sample library"].waitForExistence(timeout: 10))
    XCTAssertTrue(app.otherElements["sample-changes-demo"].waitForExistence(timeout: 10)
                  || app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH 'Try changing files here.'")).firstMatch.exists,
                  "The page does not say the changes are a demo")

    // New folder.
    app.navigationBars.buttons["add"].tap()
    app.buttons["New Folder"].tap()
    fill(app, "Favourites")
    app.alerts.buttons["new-folder-confirm"].firstMatch.tap()
    let made = app.buttons["folder:\(sampleID)/Favourites"]
    let shelf = app.scrollViews["scrolls-sideways"].firstMatch
    let width = app.windows.firstMatch.frame.width
    for _ in 0..<6 where !made.exists || made.frame.maxX > width { shelf.swipeLeft() }
    XCTAssertTrue(made.waitForExistence(timeout: 10), "The new folder is not on the page")

    // Rename it from its long press: the dialog opens as the menu goes, and takes its own button.
    made.press(forDuration: 1.0)
    XCTAssertTrue(app.buttons["Rename"].waitForExistence(timeout: 5))
    app.buttons["Rename"].tap()
    fill(app, "Best of")
    app.alerts.textFields.firstMatch.typeText("\n")  // (see FolderChangeTests: the button after a long press)
    expectation(for: NSPredicate(format: "label BEGINSWITH 'Best of'"), evaluatedWith: made)
    waitForExpectations(timeout: 10)

    // Move a photo into it.
    let photo = app.buttons.matching(gridTile).firstMatch
    XCTAssertTrue(photo.waitForExistence(timeout: 10))
    let photoID = photo.identifier
    app.navigationBars.buttons["select"].tap()
    app.buttons[photoID].tap()
    app.toolbars.buttons["Move"].tap()
    XCTAssertTrue(app.navigationBars["Move to"].waitForExistence(timeout: 10))
    app.buttons["move-to:\(sampleID)"].firstMatch.tap()
    let into = app.buttons["move-to:\(sampleID)/Favourites"].firstMatch
    XCTAssertTrue(into.waitForExistence(timeout: 10))
    into.tap()
    app.buttons["move-here"].firstMatch.tap()
    expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: app.buttons[photoID])
    waitForExpectations(timeout: 10)

    // Add from Files, into the sample's folder.
    app.navigationBars.buttons["add"].tap()
    app.buttons["Add from Files"].tap()
    let browse = app.buttons["Browse"]
    XCTAssertTrue(browse.waitForExistence(timeout: 20))
    browse.tap()
    let named = { (text: String) in app.descendants(matching: .any).matching(NSPredicate(format: "label == %@", text)).firstMatch }
    let fixtures = named("MV Fixtures")
    if !fixtures.waitForExistence(timeout: 5) { named("On My iPhone").tap() }
    XCTAssertTrue(fixtures.waitForExistence(timeout: 20), "Open the fixtures app once in library.change first")
    fixtures.tap()
    let notes = app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH 'Trip notes'")).firstMatch
    XCTAssertTrue(notes.waitForExistence(timeout: 20))
    notes.tap()
    if app.buttons["Open"].waitForExistence(timeout: 3) { app.buttons["Open"].tap() }
    let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Trip notes.txt'")).firstMatch
    XCTAssertTrue(row.waitForExistence(timeout: 20), "The file was not added")

    // Delete the folder, after saying where it goes.
    made.press(forDuration: 1.0)
    XCTAssertTrue(app.buttons["Delete"].waitForExistence(timeout: 5))
    app.buttons["Delete"].tap()
    XCTAssertTrue(app.staticTexts["It comes back when the sample is opened again."].waitForExistence(timeout: 5))
    app.buttons["Delete"].firstMatch.tap()
    expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: made)
    waitForExpectations(timeout: 10)
  }
}
