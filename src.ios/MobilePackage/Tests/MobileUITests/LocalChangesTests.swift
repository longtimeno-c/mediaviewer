// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library's changes, kept on the phone (MobileKit/LocalChanges.swift): laid over what the
// library lists, kept across a relaunch, and gone when the sample is opened again or forgotten.
import MobileKit
import XCTest

private let dogs = Folder(id: "/root/Dogs", name: "Dogs", itemCount: 2, hasSubfolders: false)
private let beaches = Folder(id: "/root/Beaches", name: "Beaches", itemCount: 1, hasSubfolders: false)

private func photo(_ n: Int, catalogue: UInt64 = 7) -> MediaItem {
  MediaItem(id: MediaID("\(n)", catalogue: catalogue), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
            pixelWidth: 4, pixelHeight: 3, duration: nil, fileName: "\(n).jpg")
}

final class LocalChangesTests: XCTestCase {
  private var dir: URL!

  override func setUp() {
    dir = FileManager.default.temporaryDirectory.appendingPathComponent("local-changes-\(UUID().uuidString)")
  }

  override func tearDown() { try? FileManager.default.removeItem(at: dir) }

  func testChangesAreLaidOverWhatTheLibraryLists() throws {
    let book = LocalChanges(directory: dir)
    book.begin(stamp: "1")
    let made = try book.makeFolder("Best", in: dogs.id, taken: [])
    XCTAssertEqual(book.folders([], in: dogs.id).map(\.name), ["Best"])
    XCTAssertThrowsError(try book.makeFolder("best", in: dogs.id, taken: ["best"])) {
      XCTAssertEqual($0 as? ChangeFailure, .exists)
    }
    try book.move(.item(photo(1)), from: dogs.id, to: made.id, taken: [])
    XCTAssertEqual(book.items([photo(1), photo(2)], in: dogs.id).map(\.id.raw), ["2"])
    // A moved photo keeps the catalogue it came from, so it keeps its pictures.
    XCTAssertEqual(book.items([], in: made.id).first?.id, photo(1).id)
    try book.rename(.item(photo(2)), to: "Rex.jpg", taken: [])
    XCTAssertEqual(book.items([photo(2)], in: dogs.id).map(\.fileName), ["Rex.jpg"])
    book.delete(.folder(beaches))
    XCTAssertTrue(book.isDeleted(beaches.id))
    XCTAssertEqual(book.folders([beaches], in: "/root"), [])
  }

  func testAnAddedFileIsKeptOnThePhoneWithTheNameItGot() throws {
    let book = LocalChanges(directory: dir)
    book.begin(stamp: "1")
    let source = FileManager.default.temporaryDirectory.appendingPathComponent("pick-\(UUID().uuidString).jpg")
    try Data("jpeg".utf8).write(to: source)
    defer { try? FileManager.default.removeItem(at: source) }
    let file = OutgoingFile(url: source, name: "1.jpg", bytes: 4)
    let name = try book.add(file, to: dogs.id, taken: ["1.jpg"])
    XCTAssertEqual(name, "1 (2).jpg")
    let added = try XCTUnwrap(book.items([], in: dogs.id).first)
    XCTAssertEqual(added.fileName, "1 (2).jpg")
    XCTAssertEqual(try Data(contentsOf: XCTUnwrap(book.localFile(for: added.id.raw))), Data("jpeg".utf8))
    XCTAssertTrue(FileManager.default.fileExists(atPath: source.path), "The picked file was moved, not copied")
  }

  /// Kept across a relaunch; gone for another sample, and when the library is forgotten.
  func testKeptUntilTheSampleIsOpenedAgainOrForgotten() throws {
    var book = LocalChanges(directory: dir)
    book.begin(stamp: "1")
    _ = try book.makeFolder("Best", in: dogs.id, taken: [])
    book = LocalChanges(directory: dir)
    book.begin(stamp: "1")
    XCTAssertEqual(book.folders([], in: dogs.id).map(\.name), ["Best"], "Not kept across a relaunch")
    book.begin(stamp: "2")
    XCTAssertEqual(book.folders([], in: dogs.id), [], "Kept for another sample")
    _ = try book.makeFolder("Best", in: dogs.id, taken: [])
    try FileManager.default.removeItem(at: dir)  // Forget removes the library's folder
    book.begin(stamp: "2")
    XCTAssertEqual(book.folders([], in: dogs.id), [], "Kept after the library was forgotten")
  }
}
