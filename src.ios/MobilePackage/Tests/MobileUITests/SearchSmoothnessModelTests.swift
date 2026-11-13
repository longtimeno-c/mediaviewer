// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-12): "when searching the images flicker". A tile follows its item
// from one answer to the next, and the screen keeps the last answer until the next is ready.
import MobileKit
@testable import MobileUI
import XCTest

private func item(_ n: Int) -> MediaItem {
  MediaItem(id: MediaID(String(n), catalogue: 1), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
            pixelWidth: 1, pixelHeight: 1, duration: nil, fileName: "\(n).jpg")
}

/// Answers each query by its length after a set wait, or waits for ever with `hold`.
private final class ScriptedSearch: SearchService, @unchecked Sendable {
  var delay = Duration.milliseconds(10)
  var hold = false
  var fails = false
  func capability() async -> SearchCapability { .ready }
  func recents() async -> [String] { [] }
  func remember(_ query: String) async {}
  func forget(_ query: String) async {}
  func forgetAll() async {}
  func filterOptions() async -> SearchFilterOptions { .none }
  func isWarmingUp() async -> Bool { false }
  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] {
    if hold {
      // A cold model: the core does not stop at once when the search is cancelled.
      await Task.detached { try? await Task.sleep(for: .seconds(3600)) }.value
    }
    if fails { throw MediaFailure.synologyUnreachable }
    try await Task.sleep(for: delay)
    if query.contains("zzz") { return [] }
    return (0..<query.count).map { SearchHit(item: item($0), moment: nil) }
  }
}

final class GridIdentityTests: XCTestCase {
  func testAnAnswerKeysItsTilesByItemNotPosition() {
    let first = GridSource(entries: [3, 1, 2].enumerated().map { GridEntry(item: item($1), position: $0) })
    let next = GridSource(entries: [1, 3].enumerated().map { GridEntry(item: item($1), position: $0) })
    XCTAssertEqual(first.ids, ["3", "1", "2"])
    XCTAssertEqual(next.ids, ["1", "3"])  // item 3 keeps its id at a new place
  }

  func testAnItemTwiceStillHasUniqueIDs() {
    let twice = GridSource(entries: [7, 7, 7].enumerated().map { GridEntry(item: item($1), position: $0) })
    XCTAssertEqual(Set(twice.ids ?? []).count, 3)
  }
}

@MainActor
final class SearchShownTests: XCTestCase {
  private func settle(_ search: SearchModel, until done: () -> Bool) async {
    for _ in 0..<200 where !done() { try? await Task.sleep(for: .milliseconds(10)) }
  }

  func testTheLastAnswerStaysWhileTheNextIsFound() async {
    let service = ScriptedSearch()
    let search = SearchModel(service: service)
    search.setText("lake")
    await settle(search) { search.shown == .results }
    XCTAssertEqual(search.shown, .results)
    let answer = search.answer

    service.hold = true
    search.setText("lake r")
    XCTAssertEqual(search.phase, .searching)
    XCTAssertEqual(search.shown, .results, "typing must not take the answer off screen")
    try? await Task.sleep(for: .milliseconds(300))
    XCTAssertEqual(search.shown, .results)
    XCTAssertEqual(search.answer, answer, "nothing animates until the next answer")
    XCTAssertEqual(search.answered, "lake")
  }

  func testNothingFoundStaysUntilTheNextAnswer() async {
    let service = ScriptedSearch()
    let search = SearchModel(service: service)
    search.setText("zzz")
    await settle(search) { search.shown == .nothing }
    XCTAssertEqual(search.shown, .nothing)
    service.hold = true
    search.setText("zzzz")
    try? await Task.sleep(for: .milliseconds(200))
    XCTAssertEqual(search.shown, .nothing, "no skeleton flash between two answers")
  }

  func testAFirstSlowSearchShowsTheSkeletonOnlyAfterAWait() async {
    let service = ScriptedSearch()
    service.hold = true
    let search = SearchModel(service: service)
    search.setText("dog")
    try? await Task.sleep(for: .milliseconds(300))
    XCTAssertEqual(search.shown, .idle)
    await settle(search) { search.shown == .searching }
    XCTAssertEqual(search.shown, .searching)
  }

  func testClearingGoesBackToTheRecents() async {
    let search = SearchModel(service: ScriptedSearch())
    search.setText("lake")
    await settle(search) { search.shown == .results }
    search.setText("")
    XCTAssertEqual(search.shown, .idle)
  }

  func testACancelledSlowSearchLeavesNoSkeleton() async {
    let service = ScriptedSearch()
    service.hold = true
    let search = SearchModel(service: service)
    search.setText("dog")
    try? await Task.sleep(for: .milliseconds(300))
    search.setText("")
    try? await Task.sleep(for: .milliseconds(1000))
    XCTAssertEqual(search.shown, .idle)
    XCTAssertFalse(search.isWarmingUp)
  }

  func testARetryAfterAFailureShowsTheSkeletonNotABlank() async {
    let service = ScriptedSearch()
    service.fails = true
    let search = SearchModel(service: service)
    search.setText("dog")
    await settle(search) { search.shown == .failed }
    XCTAssertEqual(search.shown, .failed)
    service.fails = false
    service.hold = true
    search.rerun()
    XCTAssertFalse(search.didFail)
    XCTAssertEqual(search.shown, .searching, "the skeleton at once, never a blank page")
  }
}
