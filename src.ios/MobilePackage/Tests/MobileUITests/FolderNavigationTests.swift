// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The library as a navigation stack (docs/plans/mobile.md §11.1): a folder's page slides in laid
// out, never filling in under the transition; one opened before slides in at once; the system's
// back (a swipe, the button, its menu) shortens the stack and the pages under it are as they were.
import MobileKit
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
  private func model(_ store: TreeStore, wait: Duration = .seconds(600)) -> LibraryModel {
    let model = LibraryModel(store: store)
    model.pushWait = wait
    return model
  }

  /// The tap waits for the listing (here as long as it takes: the wait is ten minutes), so the page
  /// is pushed already holding its folders and photos.
  func testAFolderSlidesInLaidOut() async {
    let library = model(TreeStore())
    library.enter(trip)
    XCTAssertTrue(library.stack.isEmpty, "Pushed before its listing came")
    await library.entering?.value
    let page = try! XCTUnwrap(library.stack.last)
    XCTAssertEqual(page.path, [trip])
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.subfolders, [day])
    XCTAssertEqual(page.entries.count, 3)
    XCTAssertEqual(library.trail, [trip])
  }

  /// A slow listing does not hold the tap: past the wait the page slides in with its skeleton, and
  /// the listing fills it in place.
  func testASlowFolderSlidesInAfterTheWait() async {
    let store = TreeStore()
    await store.lister.hold()
    let library = model(store, wait: .milliseconds(1))
    library.enter(trip)
    await library.entering?.value
    let page = try! XCTUnwrap(library.stack.last)
    XCTAssertTrue(page.isLoading)
    await store.lister.letGo()
    await page.loading?.value
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.entries.count, 3)
  }

  /// Gone back to and opened again: pushed in the same moment, laid out, and not listed again (a
  /// catalogue's folders change only with a new catalogue).
  func testAFolderOpenedBeforeSlidesInAtOnce() async {
    let store = TreeStore()
    let library = model(store)
    library.enter(trip)
    await library.entering?.value
    library.stack.removeLast()  // the system's back
    let asked = await store.lister.lists
    library.enter(trip)
    let page = try! XCTUnwrap(library.stack.last, "Not pushed at once")
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.entries.count, 3)
    XCTAssertEqual(page.subfolders, [day])
    let askedAgain = await store.lister.lists
    XCTAssertEqual(askedAgain, asked)
  }

  /// A folder on the server, listed live: shown at once from last time, and listed again behind it.
  func testAServerFolderOpenedBeforeIsListedAgainBehindIt() async {
    let store = TreeStore()
    let library = model(store)
    library.enter(nasFolder)
    await library.entering?.value
    library.stack.removeLast()
    let asked = await store.lister.lists
    library.enter(nasFolder)
    let page = try! XCTUnwrap(library.stack.last)
    XCTAssertFalse(page.isLoading)
    await page.loading?.value
    let askedAgain = await store.lister.lists
    XCTAssertEqual(askedAgain, asked + 1)
  }

  /// A new catalogue forgets what folders held: the next open waits for its listing again.
  func testANewCatalogueForgetsListings() async {
    let library = model(TreeStore())
    library.enter(trip)
    await library.entering?.value
    library.stack.removeLast()
    library.forgetPreviews()
    library.enter(trip)
    XCTAssertTrue(library.stack.isEmpty)
    await library.entering?.value
    XCTAssertEqual(library.stack.count, 1)
  }

  /// Several folders deep and back: each page under the top keeps what it held, and the trail (what
  /// Search searches) follows the stack the system shortens.
  func testBackKeepsThePagesUnderIt() async {
    let library = model(TreeStore())
    await library.load()
    library.enter(trip)
    await library.entering?.value
    library.enter(day)
    await library.entering?.value
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
    let library = model(store, wait: .milliseconds(1))
    library.enter(trip)
    await library.entering?.value
    let loading = try! XCTUnwrap(library.stack.last?.loading)
    library.stack.removeLast()
    XCTAssertTrue(loading.isCancelled)
    await store.lister.letGo()
  }

  /// A second tap while the first waits for its listing: only the second folder is pushed.
  func testOnlyTheLastTapIsPushed() async {
    let store = TreeStore()
    await store.lister.hold()
    let library = model(store)
    library.enter(trip)
    library.enter(nasFolder)
    await store.lister.letGo()
    await library.entering?.value
    XCTAssertEqual(library.stack.map(\.path), [[nasFolder]])
  }

  /// Gone back while a tap waits for its listing: the wait ends at once, nothing is pushed, and that
  /// folder stops listing.
  func testGoingBackWhileATapWaitsDropsIt() async {
    let store = TreeStore()
    let library = model(store)
    library.enter(trip)
    await library.entering?.value
    await store.lister.hold()
    library.enter(day)
    let waiting = try! XCTUnwrap(library.entering)
    library.stack.removeLast()
    await waiting.value  // ten minutes if the wait were not ended
    XCTAssertTrue(library.stack.isEmpty)
    await store.lister.letGo()
  }

  /// A new catalogue while a tap waits: that folder is listed again from it, never kept from the old one.
  func testANewCatalogueRelistsTheFolderATapWaitsFor() async {
    let store = TreeStore()
    await store.lister.hold()
    let library = model(store)
    library.enter(trip)
    library.forgetPreviews()
    await store.lister.letGo()
    await library.entering?.value
    let page = try! XCTUnwrap(library.stack.last)
    await page.loading?.value
    let lists = await store.lister.lists
    XCTAssertEqual(lists, 2, "Not listed again")
    XCTAssertFalse(page.isLoading)
    XCTAssertEqual(page.entries.count, 3)
  }
}
