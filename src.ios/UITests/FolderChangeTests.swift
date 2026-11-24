// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing files from the phone (docs/plans/mobile.md §11.1 "Changing files"), in the fixtures'
// library with a device whose role is "change" (library.change): New folder, Rename, Move, Delete
// and Add from Files, each seen to land on the page; and a device that may only view sees none of
// it (library).
import XCTest

final class FolderChangeTests: XCTestCase {
  private let gridTile = NSPredicate(format: "identifier BEGINSWITH 'tile:'")

  private func launched(_ state: String = "library.change") -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO"]
    app.launch()
    XCTAssertTrue(app.navigationBars["All media"].waitForExistence(timeout: 30), "The library did not open")
    return app
  }

  private func open(_ app: XCUIApplication, _ id: String, title: String) {
    let tile = app.buttons["folder:\(id)"]
    XCTAssertTrue(tile.waitForExistence(timeout: 10), "No tile for \(id)")
    tile.tap()
    XCTAssertTrue(app.navigationBars[title].waitForExistence(timeout: 10), "\(title) did not open")
  }

  private func addMenu(_ app: XCUIApplication, _ item: String) {
    let add = app.navigationBars.buttons["add"]
    XCTAssertTrue(add.waitForExistence(timeout: 10), "No Add")
    add.tap()
    let entry = app.buttons[item]
    XCTAssertTrue(entry.waitForExistence(timeout: 5), "No \(item) in Add")
    entry.tap()
  }

  /// Types `text` into the alert's field, over whatever it held.
  private func fill(_ app: XCUIApplication, _ text: String) {
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    // The alert's field has the keyboard already; what it holds is selected over (Rename) or empty.
    XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5), "No keyboard for the name")
    let old = (field.value as? String).flatMap { $0 == field.placeholderValue ? "" : $0 } ?? ""
    field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: old.count) + text)
    XCTAssertEqual(field.value as? String, text, "The name was not typed")
  }

  func testNewFolderRenameMoveAndDelete() throws {
    let app = launched()
    open(app, "iphone", title: "iphone")
    // Its commands are there with the page, not after it.
    XCTAssertTrue(app.navigationBars.buttons["select"].exists, "Select appeared after the page")

    // New folder: on the page at once.
    addMenu(app, "New Folder")
    fill(app, "Lisbon")
    app.alerts.buttons["new-folder-confirm"].firstMatch.tap()
    // The page lists it in place; the shelf of folders runs sideways, so it may be past the edge.
    let made = app.buttons["folder:iphone/Lisbon"]
    let shelf = app.scrollViews["scrolls-sideways"].firstMatch
    let width = app.windows.firstMatch.frame.width
    for _ in 0..<5 where !made.exists || made.frame.maxX > width { shelf.swipeLeft() }
    XCTAssertTrue(made.waitForExistence(timeout: 10), "The new folder is not on the page")

    // Rename, from its long press: the dialog opens once the menu has gone. Return confirms it:
    // XCUITest's tap on the Rename dialog's own button does not register (a tap by hand does, and
    // Create's and Delete's do here), so the test confirms with the keyboard, as a person can.
    made.press(forDuration: 1.0)
    XCTAssertTrue(app.buttons["Rename"].waitForExistence(timeout: 5), "No Rename on a long press")
    app.buttons["Rename"].tap()
    fill(app, "Porto")
    app.alerts.textFields.firstMatch.typeText("\n")
    let renamed = NSPredicate(format: "label BEGINSWITH 'Porto'")
    expectation(for: renamed, evaluatedWith: made)
    waitForExpectations(timeout: 10)

    // Rename again from Select's bar.
    app.navigationBars.buttons["select"].tap()
    made.tap()
    app.toolbars.buttons["Rename"].tap()
    fill(app, "Porto 2026")
    app.alerts.textFields.firstMatch.typeText("\n")
    expectation(for: NSPredicate(format: "label BEGINSWITH 'Porto 2026'"), evaluatedWith: made)
    waitForExpectations(timeout: 10)

    // Select a photo, Move it into Porto: it leaves this page.
    let photo = app.buttons.matching(gridTile).firstMatch
    XCTAssertTrue(photo.waitForExistence(timeout: 10))
    let photoID = photo.identifier
    app.navigationBars.buttons["select"].tap()
    app.buttons[photoID].tap()
    XCTAssertTrue(app.navigationBars["1 selected"].waitForExistence(timeout: 5), "The photo was not chosen")
    app.toolbars.buttons["Move"].tap()
    XCTAssertTrue(app.navigationBars["Move to"].waitForExistence(timeout: 10), "Move did not open")
    let iphoneRow = app.buttons["move-to:iphone"].firstMatch
    XCTAssertTrue(iphoneRow.waitForExistence(timeout: 10), "Move does not list the folders")
    iphoneRow.tap()
    let porto = app.buttons["move-to:iphone/Lisbon"].firstMatch
    XCTAssertTrue(porto.waitForExistence(timeout: 10), "Move does not list the new folder")
    XCTAssertEqual(porto.label, "Porto 2026")
    porto.tap()
    XCTAssertTrue(app.navigationBars["Porto 2026"].waitForExistence(timeout: 10))
    app.buttons["move-here"].firstMatch.tap()
    XCTAssertTrue(app.navigationBars["Move to"].waitForNonExistence(timeout: 10), "Move Here did not close the picker")
    XCTAssertTrue(app.navigationBars["iphone"].waitForExistence(timeout: 10))
    let gone = NSPredicate(format: "exists == false")
    expectation(for: gone, evaluatedWith: app.buttons[photoID])
    waitForExpectations(timeout: 10)

    // In Porto it is there; Delete it, after saying where it goes.
    open(app, "iphone/Lisbon", title: "Porto 2026")
    XCTAssertTrue(app.buttons[photoID].waitForExistence(timeout: 10), "The photo did not arrive in Porto")
    app.navigationBars.buttons["select"].tap()
    app.buttons[photoID].tap()
    app.toolbars.buttons["Delete"].tap()
    XCTAssertTrue(app.staticTexts["It goes to the trash on your server, where it can be restored."].waitForExistence(timeout: 5),
                  "Delete did not say where it goes")
    app.buttons["Delete"].firstMatch.tap()
    XCTAssertTrue(app.staticTexts["This folder has no photos or clips."].waitForExistence(timeout: 10), "The photo is still there")
  }

  /// The owner's report (2026-11-24): in the sample library, "Sample library", Add and Select
  /// crowded the bar and the folder's name read "…". The name has the bar's middle, at a width
  /// that shows it, with the chip under it and clear of the buttons.
  func testAFolderWhoseFilesCanBeChangedStillShowsItsName() throws {
    let app = launched("library.changeSample")
    XCTAssertTrue(app.buttons["Sample library"].waitForExistence(timeout: 10), "Not the sample library")
    let tile = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'folder:'")).firstMatch
    XCTAssertTrue(tile.waitForExistence(timeout: 10))
    let name = tile.label.components(separatedBy: ",").first ?? tile.label
    tile.tap()
    let select = app.buttons["select"].firstMatch
    XCTAssertTrue(select.waitForExistence(timeout: 10), "This folder cannot be changed: nothing crowds its bar")
    let title = app.staticTexts["folder-title"].firstMatch
    XCTAssertTrue(title.waitForExistence(timeout: 10), "The folder's name is not in the bar")
    XCTAssertEqual(title.label.lowercased(), name.lowercased())
    let chip = app.buttons["Sample library"].firstMatch
    XCTAssertTrue(chip.exists)
    XCTAssertGreaterThanOrEqual(chip.frame.minY, title.frame.maxY - 1, "The chip is not under the name")
    XCTAssertLessThanOrEqual(title.frame.maxX, select.frame.minX, "The name runs under Select")
    XCTAssertLessThanOrEqual(chip.frame.maxX, select.frame.minX, "The chip runs under Select")
    // Wide enough to read, not a sliver or "…".
    XCTAssertGreaterThan(title.frame.width, 40)
    if let dir = ProcessInfo.processInfo.environment["MV_SHOT_DIR"] {
      try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent("folder-title.png"))
    }
  }

  func testAddFromFiles() throws {
    let app = launched()
    open(app, "iphone", title: "iphone")
    addMenu(app, "Add from Files")
    // The system's picker: the fixtures app's own folder holds "Trip notes".
    let browse = app.buttons["Browse"]
    XCTAssertTrue(browse.waitForExistence(timeout: 20), "The Files picker did not open")
    browse.tap()
    // Browse opens on its list of places, or on the last one used ("On My iPhone").
    let named = { (text: String) in app.descendants(matching: .any).matching(NSPredicate(format: "label == %@", text)).firstMatch }
    let folder = named("MV Fixtures")
    if !folder.waitForExistence(timeout: 5) {
      let onPhone = named("On My iPhone")
      XCTAssertTrue(onPhone.waitForExistence(timeout: 10), "Files lists no On My iPhone")
      onPhone.tap()
    }
    XCTAssertTrue(folder.waitForExistence(timeout: 20), "The fixtures app's folder is not in Files")
    folder.tap()
    let notes = app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH 'Trip notes'")).firstMatch
    XCTAssertTrue(notes.waitForExistence(timeout: 20), "No Trip notes in the fixtures app's folder")
    notes.tap()
    if app.buttons["Open"].waitForExistence(timeout: 3) { app.buttons["Open"].tap() }
    // Sent, with its progress, then on the page among the folder's files.
    XCTAssertTrue(app.staticTexts["Adding “Trip notes.txt”"].waitForExistence(timeout: 20), "No progress while it is sent")
    let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Trip notes.txt'")).firstMatch
    XCTAssertTrue(row.waitForExistence(timeout: 20), "The file did not arrive in the folder")
  }

  func testAViewDeviceSeesNoChanges() throws {
    let app = launched("library")
    open(app, "iphone", title: "iphone")
    let photo = app.buttons.matching(gridTile).firstMatch
    XCTAssertTrue(photo.waitForExistence(timeout: 10))
    XCTAssertFalse(app.navigationBars.buttons["add"].exists)
    XCTAssertFalse(app.navigationBars.buttons["select"].exists)
    photo.press(forDuration: 1.0)
    XCTAssertFalse(app.buttons["Delete"].waitForExistence(timeout: 2), "A view device has Delete")
  }
}
