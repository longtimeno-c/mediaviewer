// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode (docs/plans/phone-in-the-app.md §4): a library with no catalogue opens as its folders, not
// at the copy; File search's answer says why a tag matched, and "N of M" in which folder.
import MobileKit
@testable import MobileUI
import XCTest

private func fileItem(_ name: String) -> MediaItem {
  MediaItem(id: MediaID("file:r/Trip/\(name)"), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
            pixelWidth: 0, pixelHeight: 0, duration: nil, fileName: name)
}

private final class FolderSearch: SearchService, @unchecked Sendable {
  var reading: SearchAnswer.Progress?
  private(set) var asked = 0
  func capability() async -> SearchCapability { .filesOnly }
  func recents() async -> [String] { [] }
  func remember(_ query: String) async {}
  func forget(_ query: String) async {}
  func forgetAll() async {}
  func filterOptions() async -> SearchFilterOptions { .none }
  func isWarmingUp() async -> Bool { false }
  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] { try await answer(query, filters: filters).hits }
  func answer(_ query: String, filters: SearchFilters) async throws -> SearchAnswer {
    asked += 1
    let hits = [SearchHit(item: fileItem("r5.jpg"), moment: nil),
                SearchHit(item: fileItem("IMG_1.JPG"), moment: nil, why: "Canon EOS R5")]
    return SearchAnswer(hits: hits, outOf: 40, folderName: "Trip", readingTags: reading)
  }
}

@MainActor
final class FileSearchModelTests: XCTestCase {
  private func settle(until done: () -> Bool) async {
    for _ in 0..<200 where !done() { try? await Task.sleep(for: .milliseconds(10)) }
  }

  func testATagMatchCarriesItsWhyAndTheCountItsFolder() async {
    let search = SearchModel(service: FolderSearch())
    await search.prepare()
    XCTAssertEqual(search.capability, .filesOnly)
    search.setText("r5")
    await settle { search.shown == .results }
    XCTAssertEqual(search.entries.count, 2)
    XCTAssertNil(search.entries[0].caption, "a name match says nothing more")
    XCTAssertEqual(search.entries[1].caption, "Canon EOS R5")
    XCTAssertEqual(search.fileScope, SearchModel.FileScope(folderName: "Trip", found: 2, outOf: 40, readingTags: nil))
  }

  func testWhileTagsAreStillReadItAsksAgain() async {
    let service = FolderSearch()
    service.reading = SearchAnswer.Progress(done: 3, total: 40)
    let search = SearchModel(service: service)
    search.setText("r5")
    await settle { service.asked >= 2 }
    XCTAssertGreaterThanOrEqual(service.asked, 2)
    XCTAssertEqual(search.fileScope?.readingTags, SearchAnswer.Progress(done: 3, total: 40))
    service.reading = nil
    let before = service.asked
    try? await Task.sleep(for: .milliseconds(1600))
    XCTAssertLessThanOrEqual(service.asked, before + 1, "once every tag is read it stops asking")
    search.setText("")
  }
}

final class FileModeStageTests: XCTestCase {
  @MainActor
  func testALibraryInFileModeOpensItsFoldersNotTheCopy() {
    var snapshot = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    XCTAssertEqual(AppModel.firstStage(for: snapshot), .firstCopy)
    snapshot.fileMode = true
    XCTAssertEqual(AppModel.firstStage(for: snapshot), .main)
    XCTAssertEqual(AppModel.firstStage(for: .noLibrary), .welcome)
  }
}
