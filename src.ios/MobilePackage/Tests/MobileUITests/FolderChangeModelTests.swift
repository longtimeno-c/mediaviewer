// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing files from a folder's page (LibraryChanges.swift): what is chosen, a change landing on the
// page in place (and on every page and tile showing the folders it touched), a refusal of some of a
// batch said with what did change, files sent in order, held while their folder is changed, stopped,
// tried again, and kept across a change of mode.
import MobileKit
@testable import MobileUI
import XCTest

private let trip = Folder(id: "file:r/Trip", name: "Trip", itemCount: -1, hasSubfolders: true)
private let day = Folder(id: "file:r/Trip/Day 1", name: "Day 1", itemCount: -1, hasSubfolders: true)
private let day2 = Folder(id: "file:r/Trip/Day 2", name: "Day 2", itemCount: -1, hasSubfolders: true)
private let other = Folder(id: "file:r/Other", name: "Other", itemCount: -1, hasSubfolders: true)

private func photo(_ name: String, in folder: Folder = trip) -> MediaItem {
  MediaItem(id: MediaID(folder.id + "/" + name), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
            pixelWidth: 0, pixelHeight: 0, duration: nil, fileName: name)
}

private func file(_ name: String) -> OutgoingFile {
  OutgoingFile(url: URL(fileURLWithPath: "/nonexistent/\(name)"), name: name, bytes: 10)
}

/// A server folder tree in memory, changed by the stub below.
private final class Server: @unchecked Sendable {
  let lock = NSLock()
  var folders: [String: [Folder]] = [trip.id: [day, day2]]
  var items: [String: [MediaItem]] = [trip.id: [photo("a.jpg"), photo("b.jpg")]]
  var files: [String: [OtherFile]] = [trip.id: [OtherFile(id: "file:r/Trip/notes.txt", name: "notes.txt", bytes: 5, modified: Date())]]
  var refuse: ChangeFailure?
  /// Refused for these names only.
  var refuseNames: Set<String> = []
  var mayChange = true
  var asked = 0
  /// Sends wait here until let go, when set; `sending` is told when one starts waiting.
  var holdSends = false
  private var held: [CheckedContinuation<Void, Never>] = []
  private var whenSending: [CheckedContinuation<Void, Never>] = []

  func remove(_ id: String, from folder: String) {
    lock.withLock {
      folders[folder]?.removeAll { $0.id == id }
      items[folder]?.removeAll { $0.id.raw == id }
      files[folder]?.removeAll { $0.id == id }
    }
  }

  func waitHeld() async {
    await withCheckedContinuation { c in
      let ready = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
        held.append(c)
        let tell = whenSending
        whenSending = []
        return tell
      }
      ready.forEach { $0.resume() }
    }
  }

  /// Returns once a send is waiting.
  func sending() async {
    await withCheckedContinuation { c in
      let now = lock.withLock { () -> Bool in
        if !held.isEmpty { return true }
        whenSending.append(c)
        return false
      }
      if now { c.resume() }
    }
  }

  func letSendsGo() {
    let go = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
      holdSends = false
      let all = held
      held = []
      return all
    }
    go.forEach { $0.resume() }
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
  private func refused(_ name: String) throws {
    if let refuse = server.refuse { throw refuse }
    if server.lock.withLock({ server.refuseNames.contains(name) }) { throw ChangeFailure.exists }
  }
  func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    try refused(name)
    let made = Folder(id: folder.id + "/" + name, name: name, itemCount: -1, hasSubfolders: true)
    server.lock.withLock { server.folders[folder.id, default: []].append(made) }
    return made
  }
  func add(_ file: OutgoingFile, to folder: Folder, progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    if server.lock.withLock({ server.holdSends }) { await server.waitHeld() }
    try Task.checkCancellation()
    try refused(file.name)
    progress(file.bytes, file.bytes)
    server.lock.withLock {
      server.files[folder.id, default: []].append(OtherFile(id: folder.id + "/" + file.name, name: file.name, bytes: file.bytes, modified: Date()))
    }
    return file.name
  }
  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    try refused(entry.name)
    server.remove(entry.id, from: folder.id)
    server.lock.withLock {
      switch entry {
      case .item(let i): server.items[destination.id, default: []].append(photo(i.fileName, in: destination))
      case .folder(let f): server.folders[destination.id, default: []].append(f)
      case .file(let f): server.files[destination.id, default: []].append(f)
      }
    }
  }
  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws { try refused(name) }
  func delete(_ entry: FolderEntry, in folder: Folder) async throws {
    try refused(entry.name)
    server.remove(entry.id, from: folder.id)
  }
}

@MainActor
final class FolderChangeModelTests: XCTestCase {
  private func opened(_ server: Server) async -> (LibraryModel, FolderPage) {
    let library = LibraryModel(store: Store(server: server), changes: Changes(server: server))
    library.enter(trip)
    let page = library.stack.last!
    await page.loading?.value
    await settled { page.changeable }
    return (library, page)
  }

  /// The ask for writability runs in a task of its own: yields until it has (bounded, no clock).
  private func settled(_ done: () -> Bool) async {
    for _ in 0..<1000 where !done() { await Task.yield() }
  }

  func testAPageThisPhoneMayChangeSaysSo() async {
    let server = Server()
    let (_, page) = await opened(server)
    XCTAssertTrue(page.changeable)
    server.mayChange = false
    let library = LibraryModel(store: Store(server: server), changes: Changes(server: server))
    let before = server.lock.withLock { server.asked }
    library.enter(trip)
    await library.stack.last!.loading?.value
    await settled { server.lock.withLock { server.asked } > before }
    XCTAssertGreaterThan(server.lock.withLock { server.asked }, before, "The page never asked")
    XCTAssertFalse(library.stack.last!.changeable, "A view device's page offers changes")
  }

  /// A folder inside one this phone may change has its commands from the first frame.
  func testCommandsAreThereAtPush() async {
    let (library, page) = await opened(Server())
    XCTAssertTrue(page.changeable)
    library.enter(day)
    XCTAssertTrue(library.stack.last!.changeable, "Add and Select would appear after the push")
  }

  /// Holding a tile starts Select with it chosen (as Photos does); the tap its lift brings does not
  /// undo that, and taps after it choose as Select's do.
  func testHoldingATileStartsSelectWithItChosen() async {
    let (library, page) = await opened(Server())
    library.startSelecting(holding: day.id, on: page)
    XCTAssertTrue(page.selecting)
    XCTAssertEqual(page.selected, [day.id])
    library.tapped(day.id, on: page)  // the finger lifting
    XCTAssertEqual(page.selected, [day.id], "The hold's own lift unchose it")
    library.tapped(day2.id, on: page)
    XCTAssertEqual(page.selected, [day.id, day2.id])
    library.tapped(day.id, on: page)
    XCTAssertEqual(page.selected, [day2.id])
    // A hold while choosing changes nothing; the tap that follows chooses as any tap does.
    library.startSelecting(holding: day.id, on: page)
    XCTAssertEqual(page.selected, [day2.id])
    library.tapped(day.id, on: page)
    XCTAssertEqual(page.selected, [day.id, day2.id])
  }

  /// No lift is coming after VoiceOver's Select, nor after a hold that went on into a scroll: the
  /// next tap on that tile unchooses it.
  func testAHoldWithNoLiftLosesNoTap() async {
    let (library, page) = await opened(Server())
    library.startSelecting(holding: day.id, on: page, lifting: false)
    XCTAssertEqual(page.selected, [day.id])
    library.tapped(day.id, on: page)
    XCTAssertTrue(page.selected.isEmpty, "VoiceOver's next tap was passed over")

    library.setSelecting(false, on: page)
    library.startSelecting(holding: day.id, on: page)
    library.dropHold(on: page)  // the finger went on to scroll
    library.tapped(day.id, on: page)
    XCTAssertTrue(page.selected.isEmpty, "The tap after a scroll was passed over")
  }

  /// A page this phone may only view has no Select: a hold there chooses nothing.
  func testHoldingATileOnAViewPageChoosesNothing() async {
    let server = Server()
    server.mayChange = false
    let library = LibraryModel(store: Store(server: server), changes: Changes(server: server))
    library.enter(trip)
    let page = library.stack.last!
    await page.loading?.value
    library.startSelecting(holding: day.id, on: page)
    XCTAssertFalse(page.selecting)
    XCTAssertTrue(page.selected.isEmpty)
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
    XCTAssertEqual(page.subfolders, [day2])
    XCTAssertEqual(page.entries.count, 1)
    XCTAssertFalse(page.selecting)
    XCTAssertNil(page.changeFailure)
  }

  /// Some of a batch refused: the rest still go, the page says how many and why, only what did not
  /// change comes back, and only it stays chosen.
  func testAPartlyRefusedMoveSaysWhatMoved() async {
    let server = Server()
    let (library, page) = await opened(server)
    server.refuseNames = ["Day 2"]
    library.setSelecting(true, on: page)
    library.toggle(day.id, on: page)
    library.toggle(day2.id, on: page)
    let worked = await library.move(page.chosen, from: page, to: other)
    XCTAssertFalse(worked)
    XCTAssertEqual(page.subfolders, [day2])
    XCTAssertEqual(page.changeFailure,
                   "1 of 2 moved to “Other”. " + ChangeCopy.line(for: .exists, name: "Day 2"))
    XCTAssertEqual(page.selected, [day2.id])
    XCTAssertTrue(page.selecting)
  }

  /// Files waiting to go into folders being moved: held while the moves are sent; those for the
  /// folder that moved are dropped, those for the one refused go on.
  func testFilesForAPartlyMovedBatchAreHeldThenDroppedOrSent() async {
    let server = Server()
    server.holdSends = true
    let (library, page) = await opened(server)
    library.enter(day)
    let dayPage = library.stack.last!
    library.add([file("1.jpg")], to: dayPage)
    library.stack.removeLast()
    library.enter(day2)
    library.add([file("2.jpg")], to: library.stack.last!)
    library.stack.removeLast()
    await server.sending()  // 1.jpg is being sent into Day 1
    server.refuseNames = ["Day 2"]
    let sending = library.uploading!
    let moved = await library.move([.folder(day), .folder(day2)], from: page, to: other)
    XCTAssertFalse(moved)
    XCTAssertTrue(sending.isCancelled, "A send into a folder being moved was not stopped first")
    XCTAssertEqual(library.uploads.map(\.name), ["2.jpg"])
    server.refuseNames = []
    server.letSendsGo()
    await library.uploading?.value
    XCTAssertTrue(library.uploads.isEmpty)
    XCTAssertNil(server.lock.withLock { server.files[day.id] }, "1.jpg went into a folder that had moved")
    XCTAssertEqual(server.lock.withLock { server.files[day2.id]?.map(\.name) }, ["2.jpg"])
  }

  /// Photos moved from a folder up into the one under it: the page underneath shows them on return.
  func testThePageUnderneathListsAgainAfterAMoveIntoIt() async {
    let server = Server()
    server.lock.withLock { server.items[day.id] = [photo("c.jpg", in: day)] }
    let (library, tripPage) = await opened(server)
    library.enter(day)
    let dayPage = library.stack.last!
    await dayPage.loading?.value
    let moved = await library.move([.item(photo("c.jpg", in: day))], from: dayPage, to: trip)
    XCTAssertTrue(moved)
    library.stack.removeLast()
    XCTAssertEqual(tripPage.entries.count, 3, "The page underneath still shows it without the photo")
  }

  /// A name the server would not take is said before anything is sent.
  func testANameTheServerWouldNotTakeIsSaidBeforeSending() async {
    let server = Server()
    let (library, page) = await opened(server)
    let made = await library.makeFolder(named: "2024/Lisbon", in: page)
    XCTAssertFalse(made)
    XCTAssertEqual(page.changeFailure, ChangeCopy.line(for: .badName, name: "2024/Lisbon"))
    XCTAssertEqual(server.lock.withLock { server.folders[trip.id]!.map(\.name) }, ["Day 1", "Day 2"])
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
    XCTAssertEqual(page.subfolders, [day, day2])
    XCTAssertEqual(page.others.map(\.name), ["notes.txt"])
  }

  func testNewFolderAppearsOnThePage() async {
    let (library, page) = await opened(Server())
    let made = await library.makeFolder(named: "  Lisbon ", in: page)
    XCTAssertTrue(made)
    XCTAssertEqual(page.subfolders.map(\.name), ["Day 1", "Day 2", "Lisbon"])
    let blank = await library.makeFolder(named: "   ", in: page)
    XCTAssertFalse(blank)
  }

  /// Sent: on the page once it arrives, and off the list. A refusal stays with Try again; a stop
  /// takes it off at once.
  func testUploadsArriveFailRetryAndStop() async {
    let server = Server()
    let (library, page) = await opened(server)
    server.refuse = .full
    library.add([file("IMG_1.jpg")], to: page)
    await library.uploading?.value
    XCTAssertEqual(library.uploads.first?.state, .failed(ChangeCopy.line(for: .full, name: "IMG_1.jpg")))

    server.refuse = nil
    library.retry(library.uploads[0])
    await library.uploading?.value
    XCTAssertTrue(library.uploads.isEmpty)
    XCTAssertTrue(page.others.contains { $0.name == "IMG_1.jpg" }, "The sent file is not on the page")

    server.holdSends = true
    library.add([file("IMG_2.jpg")], to: page)
    let sending = library.uploading!
    await server.sending()
    library.cancel(library.uploads[0])
    XCTAssertTrue(library.uploads.isEmpty)
    server.letSendsGo()
    await sending.value
    XCTAssertFalse(server.lock.withLock { server.files[trip.id]!.contains { $0.name == "IMG_2.jpg" } })
  }

  /// Leaving the folder does not stop a send: it arrives, and the folder lists it when opened again.
  func testAnUploadSurvivesLeavingTheFolder() async {
    let server = Server()
    server.holdSends = true
    let (library, page) = await opened(server)
    library.add([file("c.txt")], to: page)
    let sending = library.uploading!
    library.stack.removeLast()
    await server.sending()
    server.letSendsGo()
    await sending.value
    XCTAssertTrue(library.uploads.isEmpty)
    library.enter(trip)
    await library.stack.last!.loading?.value
    XCTAssertTrue(library.stack.last!.others.contains { $0.name == "c.txt" })
  }

  /// A picked file shows at once while it is copied; one that could not be read says so.
  func testPickedFilesShowWhileTheyAreReady() async {
    let server = Server()
    let (library, page) = await opened(server)
    let rows = library.preparing(["big.mov", "gone.pdf"], for: page)
    XCTAssertEqual(library.uploads.map(\.state), [.preparing, .preparing])
    XCTAssertNil(library.uploading, "Sent before it was copied")
    library.prepared(rows[1], nil)
    XCTAssertEqual(library.uploads[1].state, .failed(ChangeCopy.line(for: .unreadable, name: "gone.pdf")))
    library.prepared(rows[0], file("big.mov"))
    await library.uploading?.value
    XCTAssertEqual(library.uploads.map(\.name), ["gone.pdf"])
  }

  /// The library seen another way (its folders, then its catalogue): files being sent go on. Another
  /// library: they go.
  func testAChangeOfModeKeepsUploads() async {
    let server = Server()
    server.holdSends = true
    let (library, page) = await opened(server)
    library.add([file("d.jpg")], to: page)
    library.reset(keepingUploads: true)
    XCTAssertEqual(library.uploads.map(\.name), ["d.jpg"])
    library.reset()
    XCTAssertTrue(library.uploads.isEmpty)
    server.letSendsGo()
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
