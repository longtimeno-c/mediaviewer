// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import XCTest

final class ViewerBehaviourTests: XCTestCase {
  /// A zoomed photo that bounces at the first page's edge is still zoomed, so it cannot be
  /// dismissed by accident.
  func testZoomSurvivesEdgeBounce() throws {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", "viewer", "-fixtureChrome", "NO"]
    app.launch()
    XCTAssertTrue(app.buttons["viewer-close"].waitForExistence(timeout: 30))
    sleep(2)

    let pager = app.collectionViews.firstMatch
    pager.pinch(withScale: 3, velocity: 3)
    XCTAssertTrue(app.buttons["viewer-close-zoomed"].waitForExistence(timeout: 5), "The pinch did not zoom.")

    // The first photo has no page before it, so these only pan and then bounce the pager.
    for _ in 0..<4 { pager.swipeRight(velocity: .fast) }
    sleep(1)

    XCTAssertTrue(app.buttons["viewer-close-zoomed"].exists, "The viewer forgot the photo was zoomed.")
  }
}
