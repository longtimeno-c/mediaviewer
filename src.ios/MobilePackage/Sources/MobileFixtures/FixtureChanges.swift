// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing files, in the fixtures (MobileKit/FolderChanges.swift): the phone's own record of changes
// (MobileKit/LocalChanges.swift, in memory) laid over the generated library, so the whole flow can
// be shown and tested with no server. It refuses what a server would: a taken name, and any change
// from a device that may only view.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import Foundation
import MobileKit

/// A document in the fixtures app's own folder ("On My iPhone > MV Fixtures" in Files), for Add
/// from Files to pick.
enum FixtureDocuments {
  static func seed() {
    guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
    let file = docs.appendingPathComponent("Trip notes.txt")
    guard !FileManager.default.fileExists(atPath: file.path) else { return }
    try? Data("Lisbon, three days. Tram 28 early, before the crowds.\n".utf8).write(to: file)
  }
}

struct FixtureFolderChanges: FolderChanges {
  let world: FixtureWorld

  private var book: LocalChanges { world.changeBook }
  private var store: FixtureStore { FixtureStore(world: world) }

  func canChange(in folder: Folder) async -> Bool { world.mayChange && !folder.offline }

  func changeableRoots() async -> [Folder] {
    guard world.mayChange else { return [] }
    return (FixtureLibrary.folders[""] ?? []).filter { !$0.offline }
  }

  func folders(in folder: Folder) async throws -> [Folder] { try await store.folders(under: folder) }

  /// The names now in `folder`, lower-cased.
  private func taken(in folder: Folder) async -> Set<String> {
    let folders = (try? await store.folders(under: folder)) ?? []
    let files = (try? await store.otherFiles(in: folder)) ?? []
    let items = (try? await store.list(in: .folder(folder))).map { list in (0..<list.count).map { list.item(at: $0).fileName } } ?? []
    return Set((folders.map(\.name) + files.map(\.name) + items).map { $0.lowercased() })
  }

  func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    try allowed(name)
    return try book.makeFolder(name, in: folder.id, taken: await taken(in: folder))
  }

  /// Sent over a pretend link: a few steps a second, and a stop leaves nothing behind.
  func add(_ file: OutgoingFile, to folder: Folder,
           progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    try allowed(file.name)
    let total = max(file.bytes, 1)
    for step in 1...world.uploadSteps {
      try await Task.sleep(for: world.uploadStep)
      progress(total * Int64(step) / Int64(world.uploadSteps), total)
    }
    try Task.checkCancellation()
    return try book.add(file, to: folder.id, taken: await taken(in: folder))
  }

  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    try allowed(entry.name)
    try book.move(entry, from: folder.id, to: destination.id, taken: await taken(in: destination))
  }

  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws {
    try allowed(name)
    try book.rename(entry, to: name, taken: await taken(in: folder))
  }

  func delete(_ entry: FolderEntry, in folder: Folder) async throws {
    try allowed(entry.name)
    book.delete(entry)
  }

  private func allowed(_ name: String) throws {
    guard world.mayChange else { throw ChangeFailure.notAllowed }
    guard !name.hasPrefix("."), !name.contains("/") else { throw ChangeFailure.badName }
  }
}
