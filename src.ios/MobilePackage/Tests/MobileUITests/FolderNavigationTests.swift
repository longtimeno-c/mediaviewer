// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The library as a navigation stack (docs/plans/mobile.md §11.1): a tap slides the folder's page in
// at once, laid out when its tile had it listed ahead; one opened before slides in from what it
// held and is not listed again under the transition; the system's back (a swipe, the button, its
// menu) shortens the stack and the pages under it are as they were.
import MobileKit
import Observation
@testable import MobileUI
import XCTest

private func item(_ n: Int) -> MediaItem {
  MediaItem(id: MediaID("\(n)"), kind: .photo, capturedAt: Date(timeIntervalSince1970: TimeInterval(1_700_000_000 - n * 3600)),
            pixelWidth: 4, pixelHeight: 3, duration: nil, fileName: "\(n).jpg")
}

private let trip = Folder(id: "Trip", name: "Trip", itemCount: 3, hasSubfolders: true)
private let day = Folder(id: "Trip/Day 1", name: "Day 1", itemCount: 2, hasSubfolders: false)
private let nasFolder = Folder(id: "nas/Docs", name: "Docs", itemCount: -1, hasSubfolders: false)

/// Holds every listing until it is let go, when `held`; counts what was asked.
private actor Lister {
  var held = false
  private(set) var lists = 0
  private var waiting: [CheckedContinuation<Void, Never>] = []

  func hold() { held = true }

  func letGo() {
    held = false
    waiting.forEach { $0.resume() }
    waiting = []
  }

  func list() async {
    lists += 1
    guard held else { return }
    await withCheckedContinuation { waiting.append($0) }
  }
}

private struct TreeStore: LibraryStore {
  let lister = Lister()

  func list(in scope: LibraryScope) async throws -> any MediaList {
    await lister.list()
    try Task.checkCancellation()
    switch scope {
    case .folder(let folder) where folder == day: return ArrayMediaList([item(1), item(2)])
    case .folder: return ArrayMediaList([item(1), item(2), item(3)])
    default: return ArrayMediaList((1...5).map(item))
    }
  }

  func folders(under parent: Folder?) async throws -> [Folder] {
    switch parent {
    case nil: return [trip, nasFolder]
    case trip?: return [day]
    default: return []
    }
  }

  func people() async throws -> [Person] { [] }
  func details(for item: MediaItem) async throws -> MediaDetails { throw MediaFailure.originalGone }
  func preview(of folder: Folder, count: Int) async -> FolderPreview { .empty }
}

@MainActor
final class FolderNavigationTests: XCTestCase {
  /// The tap answers at once: the page is on the stack in the same moment, its listing to come.
  func testATapPushesAtOnce() async {
    let store = TreeStore()
    await store.lister.hold()
    let library = LibraryModel(store: store)
    library.enter(trip)
    let page = try! XCTUnwrap(library.stack.last, "Not pushed at once")
    XCTAssertEqual(page.path, [trip])
    XCTAssertTrue(page.isLoading)
    XCTAssertEqual(library.trail, [trip])
    await store.lister.letGo()
    await page.loading?.value
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.subfolders, [day])
    XCTAssertEqual(page.entries.count, 3)
  }

  /// Its tile on screen had it listed: the tap pushes it laid out, and nothing is asked again.
  func testAFolderListedAheadSlidesInLaidOut() async {
    let store = TreeStore()
    let library = LibraryModel(store: store)
    await library.load()
    await library.prefetch(trip, in: library.root)?.value
    let asked = await store.lister.lists
    library.enter(trip)
    let page = try! XCTUnwrap(library.stack.last)
    XCTAssertFalse(page.isLoading, "Pushed with its skeleton")
    XCTAssertEqual(page.entries.count, 3)
    XCTAssertEqual(page.subfolders, [day])
    let askedAgain = await store.lister.lists
    XCTAssertEqual(askedAgain, asked)
  }

  /// Gone back to and opened again: laid out at once, and not listed again (a catalogue's folders
  /// change only with a new catalogue).
  func testAFolderOpenedBeforeSlidesInAtOnce() async {
    let store = TreeStore()
    let library = LibraryModel(store: store)
    library.enter(trip)
    await library.stack.last?.loading?.value
    library.stack.removeLast()  // the system's back
    let asked = await store.lister.lists
    library.enter(trip)
    let page = try! XCTUnwrap(library.stack.last)
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.entries.count, 3)
    let askedAgain = await store.lister.lists
    XCTAssertEqual(askedAgain, asked)
  }

  /// A folder on the server listed moments ago is not listed again under the transition.
  func testAServerFolderListedJustNowIsNotAskedAgain() async {
    let store = TreeStore()
    let library = LibraryModel(store: store)
    library.enter(nasFolder)
    await library.stack.last?.loading?.value
    library.stack.removeLast()
    let asked = await store.lister.lists
    library.enter(nasFolder)
    await library.stack.last?.loading?.value  // (the first one's, long done: no second listing started)
    let askedAgain = await store.lister.lists
    XCTAssertEqual(askedAgain, asked)
  }

  /// Listed again and holding the same items: the grid is not handed a new list (no tile redrawn).
  func testAnUnchangedListingRedrawsNoTile() async {
    let library = LibraryModel(store: TreeStore())
    library.enter(trip)
    let page = try! XCTUnwrap(library.stack.last)
    await page.loading?.value
    let redrawn = Redrawn()
    withObservationTracking { _ = page.entries } onChange: { redrawn.yes = true }
    await library.load(page).value
    XCTAssertFalse(redrawn.yes, "An identical listing redrew the grid")
  }

  /// A new catalogue forgets what folders held: the next open lists again.
  func testANewCatalogueForgetsListings() async {
    let library = LibraryModel(store: TreeStore())
    library.enter(trip)
    await library.stack.last?.loading?.value
    library.stack.removeLast()
    library.forgetPreviews()
    library.enter(trip)
    XCTAssertTrue(library.stack.last!.isLoading)
  }

  /// Several folders deep and back: each page under the top keeps what it held, and the trail (what
  /// Search searches) follows the stack the system shortens.
  func testBackKeepsThePagesUnderIt() async {
    let library = LibraryModel(store: TreeStore())
    await library.load()
    library.enter(trip)
    await library.stack.last?.loading?.value
    library.enter(day)
    await library.stack.last?.loading?.value
    XCTAssertEqual(library.trail, [trip, day])
    XCTAssertEqual(library.current.entries.count, 2)
    let tripPage = library.stack[0]
    library.stack.removeLast()
    XCTAssertTrue(library.current === tripPage)
    XCTAssertEqual(library.trail, [trip])
    XCTAssertEqual(tripPage.entries.count, 3)
    library.stack.removeAll()  // the back button's menu, to All media
    XCTAssertEqual(library.trail, [])
    XCTAssertEqual(library.root.entries.count, 5)
    XCTAssertEqual(library.root.subfolders.map(\.id), [trip.id, nasFolder.id])
  }

  /// A page taken off the stack while it is still listing stops listing.
  func testAPageGoneBackFromStopsLoading() async {
    let store = TreeStore()
    await store.lister.hold()
    let library = LibraryModel(store: store)
    library.enter(trip)
    let loading = try! XCTUnwrap(library.stack.last?.loading)
    library.stack.removeLast()
    XCTAssertTrue(loading.isCancelled)
    await store.lister.letGo()
  }
}

private final class Redrawn: @unchecked Sendable { var yes = false }
