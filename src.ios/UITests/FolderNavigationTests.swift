// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Going through folders (the owner, 2026-11-22: "really slick and not clunky"). Several folders deep
// in the fixtures' library and back: each page arrives with its tiles, the edge swipe follows the
// finger and can be let go of, and the page gone back to is where it was left. The hitch figures
// for the same walk are ScrollHitchTests/testFolderNavigationHitches.
import XCTest

final class FolderNavigationTests: XCTestCase {
  private let gridTile = NSPredicate(format: "identifier BEGINSWITH 'tile:'")

  private func launched() -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", "library", "-fixtureChrome", "NO"]
    app.launch()
    XCTAssertTrue(app.navigationBars["All media"].waitForExistence(timeout: 30), "The library did not open")
    return app
  }

  private func folder(_ app: XCUIApplication, _ id: String) -> XCUIElement { app.buttons["folder:\(id)"] }

  /// Opens a folder by its tile and waits for its title. That a page is laid out before it slides in
  /// is the model's (MobileUITests/FolderNavigationTests); here, that each page holds what it should.
  private func open(_ app: XCUIApplication, _ id: String, title: String) {
    let tile = folder(app, id)
    XCTAssertTrue(tile.waitForExistence(timeout: 10), "No tile for \(id)")
    tile.tap()
    XCTAssertTrue(app.navigationBars[title].waitForExistence(timeout: 10), "\(title) did not open")
  }

  /// From the screen's left edge. Slowly, and held before letting go, so whether it goes back is
  /// decided by how far it went, not by how fast it was let go.
  private func edgeSwipe(_ app: XCUIApplication, to x: CGFloat) {
    let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0.5))
    start.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: x, dy: 0.5)),
                withVelocity: .slow, thenHoldForDuration: 0.3)
  }

  func testDeepFoldersAndBack() throws {
    let app = launched()
    // All media scrolled a little, so going back can show it kept its place.
    let iphone = folder(app, "iphone")
    XCTAssertTrue(iphone.waitForExistence(timeout: 10))
    let top = iphone.frame.minY
    // Slowly and held at the end: no fling, and the tile stays below the bar (a tap on a tile out of
    // sight would scroll it back into view itself).
    let grid = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.7))
    grid.press(forDuration: 0.1, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.64)),
               withVelocity: .slow, thenHoldForDuration: 0.3)
    let left = iphone.frame
    XCTAssertLessThan(left.minY, top - 20, "All media did not scroll")
    XCTAssertGreaterThan(left.midY, app.navigationBars["All media"].frame.maxY, "Scrolled the tile out of sight")

    open(app, "iphone", title: "iphone")
    // Its own folders: tiles only this page has.
    let year = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'folder:iphone/'")).firstMatch
    XCTAssertTrue(year.waitForExistence(timeout: 10), "iphone arrived without its folders")
    let yearName = year.label.components(separatedBy: ",")[0]
    let yearID = String(year.identifier.dropFirst("folder:".count))
    open(app, yearID, title: yearName)
    let month = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@", "folder:\(yearID)/")).firstMatch
    XCTAssertTrue(month.waitForExistence(timeout: 10), "\(yearName) arrived without its folders")
    let monthName = month.label.components(separatedBy: ",")[0]
    open(app, String(month.identifier.dropFirst("folder:".count)), title: monthName)
    XCTAssertTrue(app.buttons.matching(gridTile).firstMatch.waitForExistence(timeout: 10), "\(monthName) has no photos")

    // Let go early: the swipe follows the finger and the page stays.
    edgeSwipe(app, to: 0.25)
    XCTAssertTrue(app.navigationBars[monthName].waitForExistence(timeout: 5), "A short swipe went back")
    XCTAssertFalse(app.navigationBars[yearName].exists, "A short swipe went back")
    // All the way: back a folder.
    edgeSwipe(app, to: 0.9)
    XCTAssertTrue(app.navigationBars[yearName].waitForExistence(timeout: 5), "The edge swipe did not go back")
    // The back button, then its menu's way to All media is the system's own; the button twice here.
    app.navigationBars[yearName].buttons.firstMatch.tap()
    XCTAssertTrue(app.navigationBars["iphone"].waitForExistence(timeout: 5))
    app.navigationBars["iphone"].buttons.firstMatch.tap()
    XCTAssertTrue(app.navigationBars["All media"].waitForExistence(timeout: 5))
    XCTAssertEqual(iphone.frame.minY, left.minY, accuracy: 1, "All media lost its place")

    // Opened again: at once, from what it held.
    open(app, "iphone", title: "iphone")
  }

  /// A folder of documents: its files are the page.
  func testAFolderOfDocuments() throws {
    let app = launched()
    let docs = folder(app, "Docs")
    // The shelf scrolls sideways; the documents' folder is near its end.
    let width = app.windows.firstMatch.frame.width
    for _ in 0..<4 where !docs.exists || docs.frame.maxX > width {
      app.scrollViews["scrolls-sideways"].firstMatch.swipeLeft()
    }
    docs.tap()
    XCTAssertTrue(app.navigationBars["Docs"].waitForExistence(timeout: 10))
    XCTAssertTrue(app.staticTexts["8 files"].waitForExistence(timeout: 10), "Docs has no files")
    edgeSwipe(app, to: 0.9)
    XCTAssertTrue(app.navigationBars["All media"].waitForExistence(timeout: 5))
  }
}
