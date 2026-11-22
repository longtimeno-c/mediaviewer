// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing files from a folder's page (LibraryChanges.swift): what is chosen, a change landing on the
// page in place, a refusal said on the page with the choice kept, and files sent in order, stopped,
// and tried again.
import MobileKit
@testable import MobileUI
import XCTest

private let trip = Folder(id: "file:r/Trip", name: "Trip", itemCount: -1, hasSubfolders: true)
private let day = Folder(id: "file:r/Trip/Day 1", name: "Day 1", itemCount: -1, hasSubfolders: true)
private let other = Folder(id: "file:r/Other", name: "Other", itemCount: -1, hasSubfolders: true)

private func photo(_ name: String) -> MediaItem {
  MediaItem(id: MediaID("file:r/Trip/\(name)"), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
            pixelWidth: 0, pixelHeight: 0, duration: nil, fileName: name)
}

/// A server folder tree in memory, changed by the stub below.
private final class Server: @unchecked Sendable {
  let lock = NSLock()
  var folders: [String: [Folder]] = [trip.id: [day]]
  var items: [String: [MediaItem]] = [trip.id: [photo("a.jpg"), photo("b.jpg")]]
  var files: [String: [OtherFile]] = [trip.id: [OtherFile(id: "file:r/Trip/notes.txt", name: "notes.txt", bytes: 5, modified: Date())]]
  var refuse: ChangeFailure?
  var mayChange = true
  var asked = 0
  /// A send waits here until let go, when set.
  var held: CheckedContinuation<Void, Never>?
  var holdSends = false

  func remove(_ id: String, from folder: String) {
    lock.withLock {
      folders[folder]?.removeAll { $0.id == id }
      items[folder]?.removeAll { $0.id.raw == id }
      files[folder]?.removeAll { $0.id == id }
    }
  }
}

private struct Store: LibraryStore {
  let server: Server
  func list(in scope: LibraryScope) async throws -> any MediaList {
    guard case .folder(let f) = scope else { return ArrayMediaList([]) }
    return ArrayMediaList(server.lock.withLock { server.items[f.id] ?? [] })
  }
  func folders(under parent: Folder?) async throws -> [Folder] {
    server.lock.withLock { parent.map { server.folders[$0.id] ?? [] } ?? [trip, other] }
  }
  func otherFiles(in folder: Folder) async throws -> [OtherFile] { server.lock.withLock { server.files[folder.id] ?? [] } }
  func people() async throws -> [Person] { [] }
  func details(for item: MediaItem) async throws -> MediaDetails { throw MediaFailure.originalGone }
  func preview(of folder: Folder, count: Int) async -> FolderPreview { .empty }
}

private struct Changes: FolderChanges {
  let server: Server
  func canChange(in folder: Folder) async -> Bool {
    server.lock.withLock { server.asked += 1 }
    return server.mayChange
  }
  func changeableRoots() async -> [Folder] { [trip, other] }
  func folders(in folder: Folder) async throws -> [Folder] { server.lock.withLock { server.folders[folder.id] ?? [] } }
  func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    if let refuse = server.refuse { throw refuse }
    let made = Folder(id: folder.id + "/" + name, name: name, itemCount: -1, hasSubfolders: true)
    server.lock.withLock { server.folders[folder.id, default: []].append(made) }
    return made
  }
  func add(_ file: OutgoingFile, to folder: Folder, progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    if server.holdSends { await withCheckedContinuation { c in server.lock.withLock { server.held = c } } }
    try Task.checkCancellation()
    if let refuse = server.refuse { throw refuse }
    progress(file.bytes, file.bytes)
    server.lock.withLock {
      server.files[folder.id, default: []].append(OtherFile(id: folder.id + "/" + file.name, name: file.name, bytes: file.bytes, modified: Date()))
    }
    return file.name
  }
  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    if let refuse = server.refuse { throw refuse }
    server.remove(entry.id, from: folder.id)
    if case .item(let i) = entry { server.lock.withLock { server.items[destination.id, default: []].append(i) } }
  }
  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws {
    if let refuse = server.refuse { throw refuse }
  }
  func delete(_ entry: FolderEntry, in folder: Folder) async throws {
    if let refuse = server.refuse { throw refuse }
    server.remove(entry.id, from: folder.id)
  }
}

@MainActor
final class FolderChangeModelTests: XCTestCase {
  private func opened(_ server: Server) async -> (LibraryModel, FolderPage) {
    let library = LibraryModel(store: Store(server: server), changes: Changes(server: server))
    library.pushWait = .seconds(600)
    library.enter(trip)
    await library.entering?.value
    let page = library.stack.last!
    await settled(page) { $0.changeable }
    return (library, page)
  }

  /// Waits for the main actor's work on `page` (no clock): yields until `done`, at most a few hundred times.
  private func settled(_ page: FolderPage, _ done: (FolderPage) -> Bool) async {
    for _ in 0..<500 where !done(page) { await Task.yield() }
  }

  func testAPageThisPhoneMayChangeSaysSo() async {
    let server = Server()
    let (_, page) = await opened(server)
    XCTAssertTrue(page.changeable)
    server.mayChange = false
    let library = LibraryModel(store: Store(server: server), changes: Changes(server: server))
    library.pushWait = .seconds(600)
    let before = server.lock.withLock { server.asked }
    library.enter(trip)
    await library.entering?.value
    for _ in 0..<500 where server.lock.withLock({ server.asked }) == before { await Task.yield() }
    for _ in 0..<50 { await Task.yield() }
    XCTAssertGreaterThan(server.lock.withLock { server.asked }, before, "The page never asked")
    XCTAssertFalse(library.stack.last!.changeable, "A view device's page offers changes")
  }

  func testChosenFollowsThePageOrder() async {
    let (library, page) = await opened(Server())
    library.setSelecting(true, on: page)
    library.toggle("file:r/Trip/b.jpg", on: page)
    library.toggle(day.id, on: page)
    library.toggle("file:r/Trip/notes.txt", on: page)
    XCTAssertEqual(page.chosen.map(\.name), ["Day 1", "notes.txt", "b.jpg"])
    library.toggle(day.id, on: page)
    XCTAssertEqual(page.chosen.map(\.name), ["notes.txt", "b.jpg"])
    library.setSelecting(false, on: page)
    XCTAssertTrue(page.chosen.isEmpty)
  }

  /// Deleted: gone from the page (a folder at once, a photo with the new listing), and Select ends.
  func testDeleteLandsInPlace() async {
    let (library, page) = await opened(Server())
    library.setSelecting(true, on: page)
    library.toggle(day.id, on: page)
    library.toggle("file:r/Trip/a.jpg", on: page)
    let worked = await library.delete(page.chosen, in: page)
    XCTAssertTrue(worked)
    XCTAssertEqual(page.subfolders, [])
    XCTAssertEqual(page.entries.count, 1)
    XCTAssertFalse(page.selecting)
    XCTAssertNil(page.changeFailure)
  }

  /// A taken name: said on the page, naming what was refused; the choice stays for another try.
  func testARefusalIsSaidAndTheChoiceKept() async {
    let server = Server()
    let (library, page) = await opened(server)
    library.setSelecting(true, on: page)
    library.toggle("file:r/Trip/a.jpg", on: page)
    server.refuse = .exists
    let worked = await library.move(page.chosen, from: page, to: other)
    XCTAssertFalse(worked)
    XCTAssertEqual(page.changeFailure, ChangeCopy.line(for: .exists, name: "a.jpg"))
    XCTAssertTrue(page.selecting)
    XCTAssertEqual(page.selected, ["file:r/Trip/a.jpg"])
    XCTAssertEqual(page.entries.count, 2, "A refused move changed the page")
  }

  func testMoveLeavesThePageAndTheDestinationListsAfresh() async {
    let server = Server()
    let (library, page) = await opened(server)
    library.enter(day)
    await library.entering?.value
    library.stack.removeLast()  // Day 1's listing is now remembered
    let moved = await library.move([.item(photo("b.jpg"))], from: page, to: day)
    XCTAssertTrue(moved)
    XCTAssertEqual(page.entries.count, 1)
    library.enter(day)
    XCTAssertTrue(library.stack.count == 1, "Day 1 was shown from before the move")
    await library.entering?.value
    XCTAssertEqual(library.stack.last?.entries.count, 1)
  }

  func testNewFolderAppearsOnThePage() async {
    let (library, page) = await opened(Server())
    let made = await library.makeFolder(named: "  Lisbon ", in: page)
    XCTAssertTrue(made)
    XCTAssertEqual(page.subfolders.map(\.name), ["Day 1", "Lisbon"])
    let blank = await library.makeFolder(named: "   ", in: page)
    XCTAssertFalse(blank)
  }

  /// Sent: on the page once it arrives, and off the list. A refusal stays with Try again; a stop
  /// takes it off at once.
  func testUploadsArriveFailRetryAndStop() async throws {
    let server = Server()
    let (library, page) = await opened(server)
    let file = OutgoingFile(url: URL(fileURLWithPath: "/nonexistent/IMG_1.jpg"), name: "IMG_1.jpg", bytes: 10)

    server.refuse = .full
    library.add([file], to: page)
    await library.uploading?.value
    XCTAssertEqual(library.uploads.first?.state, .failed(ChangeCopy.line(for: .full, name: "IMG_1.jpg")))

    server.refuse = nil
    library.retry(library.uploads[0])
    await library.uploading?.value
    await page.loading?.value
    XCTAssertTrue(library.uploads.isEmpty)
    XCTAssertTrue(page.others.contains { $0.name == "IMG_1.jpg" }, "The sent file is not on the page")

    // Stopped while it is being sent: off the list at once, and nothing arrives.
    server.holdSends = true
    library.add([OutgoingFile(url: file.url, name: "IMG_2.jpg", bytes: 10)], to: page)
    let sending = try XCTUnwrap(library.uploading)
    for _ in 0..<200 where server.lock.withLock({ server.held == nil }) { await Task.yield() }
    library.cancel(library.uploads[0])
    XCTAssertTrue(library.uploads.isEmpty)
    server.lock.withLock { server.held?.resume() }
    await sending.value
    XCTAssertFalse(server.lock.withLock { server.files[trip.id]!.contains { $0.name == "IMG_2.jpg" } })
  }

  /// Leaving the folder does not stop a send: it arrives, and the folder lists it when opened again.
  func testAnUploadSurvivesLeavingTheFolder() async {
    let server = Server()
    server.holdSends = true
    let (library, page) = await opened(server)
    library.add([OutgoingFile(url: URL(fileURLWithPath: "/nonexistent/c.txt"), name: "c.txt", bytes: 3)], to: page)
    let sending = library.uploading!
    library.stack.removeLast()
    for _ in 0..<200 where server.lock.withLock({ server.held == nil }) { await Task.yield() }
    server.lock.withLock { server.held?.resume() }
    await sending.value
    XCTAssertTrue(library.uploads.isEmpty)
    library.enter(trip)
    await library.entering?.value
    XCTAssertTrue(library.stack.last!.others.contains { $0.name == "c.txt" })
  }

  /// A slash would make folders on the server, a dot first would hide it: said, nothing sent.
  func testANameTheServerWouldNotTakeIsSaidBeforeSending() async {
    let server = Server()
    let (library, page) = await opened(server)
    let made = await library.makeFolder(named: "2024/Lisbon", in: page)
    XCTAssertFalse(made)
    XCTAssertEqual(page.changeFailure, ChangeCopy.line(for: .badName, name: "2024/Lisbon"))
    XCTAssertEqual(server.lock.withLock { server.folders[trip.id]!.map(\.name) }, ["Day 1"])
    let renamed = await library.rename(.item(photo("a.jpg")), to: ".hidden", in: page)
    XCTAssertFalse(renamed)
    XCTAssertNil(LibraryModel.nameProblem("Trip notes (2).txt"))
  }

  /// A refused delete puts back what left the page at once.
  func testARefusedDeleteRestoresThePage() async {
    let server = Server()
    let (library, page) = await opened(server)
    server.refuse = .notAllowed
    let worked = await library.delete([.folder(day), .file(page.others[0])], in: page)
    XCTAssertFalse(worked)
    XCTAssertEqual(page.subfolders, [day])
    XCTAssertEqual(page.others.map(\.name), ["notes.txt"])
  }

  /// A folder deleted while files wait to go into it: they are not sent (that would make it again).
  func testFilesWaitingForADeletedFolderAreNotSent() async {
    let server = Server()
    server.holdSends = true
    let (library, page) = await opened(server)
    library.enter(day)
    await library.entering?.value
    let dayPage = library.stack.last!
    let file = { (n: String) in OutgoingFile(url: URL(fileURLWithPath: "/nonexistent/\(n)"), name: n, bytes: 1) }
    library.add([file("1.jpg"), file("2.jpg")], to: dayPage)
    library.stack.removeLast()
    let sending = library.uploading!
    let worked = await library.delete([.folder(day)], in: page)
    XCTAssertTrue(worked)
    XCTAssertTrue(library.uploads.isEmpty)
    XCTAssertEqual(page.changeFailure, ChangeCopy.uploadsDropped(2, folder: "Day 1"))
    server.lock.withLock { server.held?.resume() }
    await sending.value
    XCTAssertNil(server.lock.withLock { server.files[day.id] })
  }

  /// What was chosen and is no longer listed is no longer chosen.
  func testAChoiceGoneFromTheListingIsDropped() async {
    let server = Server()
    let (library, page) = await opened(server)
    library.setSelecting(true, on: page)
    library.toggle("file:r/Trip/a.jpg", on: page)
    library.toggle(day.id, on: page)
    server.remove("file:r/Trip/a.jpg", from: trip.id)  // gone elsewhere
    await library.load(page).value
    XCTAssertEqual(page.selected, [day.id])
  }
}
