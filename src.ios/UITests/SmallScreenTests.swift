// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Every screen on the smallest phone at the largest text size (tools/ios/small-screen.sh sets
// both). Nothing may run off the side: a word cut at the edge is a word no one can read, and a
// button past it is one no one can press. Screenshots go to MV_SMALL_OUT for a person to look at.
import XCTest

final class SmallScreenTests: XCTestCase {
  static let states = [
    "welcome", "scan", "connect.confirm", "connect.confirmGuest", "connect.working", "connect.unreachable", "connect.notReadable",
    "connect.ownNetwork", "firstCopy.running", "firstCopy.waitingForWiFi", "firstCopy.noSpace", "firstCopy.fails",
    "library", "library.unreachable", "library.stale", "library.folder", "library.empty", "library.revoked", "library.accessEnded",
    "library.handedOver", "search.recents", "search.results", "search.nothing", "search.filtered", "search.unsupportedTower",
    "search.failed", "search.warmingUp", "viewer", "viewer.gone", "viewer.cannotDecode", "details", "clip", "clip.tooBig",
    "clip.notPlayable", "people", "people.none", "connection", "connection.unreachable", "settings.light",
  ]

  /// Screens that are words in the middle of the page: at this size they must scroll.
  static let pagesOfWords: Set<String> = ["search.nothing", "library.empty"]

  func testEveryScreenFitsTheSmallestPhoneAtTheLargestText() throws {
    guard let out = ProcessInfo.processInfo.environment["MV_SMALL_OUT"] else { throw XCTSkip("Run through tools/ios/small-screen.sh") }
    var overflow: [String] = []
    for state in Self.states {
      let app = XCUIApplication()
      app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO", "-appearance", state.hasSuffix("light") ? "light" : "dark"]
      app.launch()
      sleep(4)
      try XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: out).appendingPathComponent(state + ".png"))
      let window = app.windows.firstMatch.frame
      let width = window.width
      // Rows that scroll sideways (folder chips, filters, the filmstrip) run past the edge on
      // purpose, showing the next item as a hint. The app marks them.
      let rows = app.descendants(matching: .any).matching(identifier: "scrolls-sideways").allElementsBoundByIndex.map(\.frame)
      for kind in [app.buttons, app.staticTexts] {
        for element in kind.allElementsBoundByIndex where element.exists {
          let f = element.frame
          guard !f.isEmpty, f.height > 1, !element.label.isEmpty else { continue }
          // The grid's tiles, the pager's pages and the filmstrip's pictures sit beside the screen
          // on purpose: they are pictures to scroll to, not words to read.
          if element.identifier.hasPrefix("tile:") || f.width >= width * 0.95 { continue }
          if element.label.hasPrefix("Photo, ") || element.label.hasPrefix("Clip, ") { continue }
          if rows.contains(where: { $0.minY - 1 <= f.minY && f.maxY <= $0.maxY + 1 }) { continue }
          if f.minX < -1 || f.maxX > width + 1 {
            overflow.append("\(state): “\(element.label.prefix(50))” at \(Int(f.minX))–\(Int(f.maxX)) of \(Int(width))")
          }
        }
      }
      // A page of words reaches its last one by scrolling, never ending under the tab bar.
      if Self.pagesOfWords.contains(state), let last = app.staticTexts.allElementsBoundByIndex.last(where: { $0.exists && !$0.label.isEmpty }) {
        for _ in 0..<4 where last.frame.maxY > window.maxY - 90 { app.swipeUp() }
        if last.frame.maxY > window.maxY - 90 { overflow.append("\(state): “\(last.label.prefix(50))” cannot be scrolled into view") }
      }
      app.terminate()
    }
    XCTAssertTrue(overflow.isEmpty, overflow.joined(separator: "\n"))
  }
}
