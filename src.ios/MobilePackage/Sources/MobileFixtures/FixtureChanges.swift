// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing files, in the fixtures (MobileKit/FolderChanges.swift): an in-memory record of what was
// added, made, moved, renamed and deleted, laid over the generated library, so the whole flow can be
// shown and tested with no server. It refuses what a server would: a taken name, and any change from
// a device that may only view.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import Foundation
import MobileKit

final class FixtureChangeBook: @unchecked Sendable {
  private let lock = NSLock()
  private var addedFolders: [String: [Folder]] = [:]
  private var addedFiles: [String: [OtherFile]] = [:]
  private var addedItems: [String: [MediaItem]] = [:]
  /// Moved away from a folder (by the folder's id).
  private var movedFrom: [String: Set<String>] = [:]
  /// Deleted: gone from every folder.
  private var deleted: Set<String> = []
  private var renamed: [String: String] = [:]
  private var nextItem = 900_000

  func folders(_ base: [Folder], in parent: String) -> [Folder] {
    lock.withLock {
      (base + (addedFolders[parent] ?? []))
        .filter { !deleted.contains($0.id) && !(movedFrom[parent]?.contains($0.id) ?? false) }
        // Any folder may now hold folders made in it.
        .map { Folder(id: $0.id, name: renamed[$0.id] ?? $0.name, itemCount: $0.itemCount, hasSubfolders: true) }
    }
  }

  func files(_ base: [OtherFile], in parent: String) -> [OtherFile] {
    lock.withLock {
      (base + (addedFiles[parent] ?? []))
        .filter { !deleted.contains($0.id) && !(movedFrom[parent]?.contains($0.id) ?? false) }
        .map { f in renamed[f.id].map { OtherFile(id: f.id, name: $0, bytes: f.bytes, modified: f.modified) } ?? f }
    }
  }

  /// Newest first; added ones are the newest.
  func items(_ base: [MediaItem], in parent: String) -> [MediaItem] {
    lock.withLock {
      let gone = movedFrom[parent] ?? []
      return ((addedItems[parent] ?? []).reversed() + base)
        .filter { !deleted.contains($0.id.raw) && !gone.contains($0.id.raw) }
        .map { i in
          renamed[i.id.raw].map {
            MediaItem(id: i.id, kind: i.kind, capturedAt: i.capturedAt, pixelWidth: i.pixelWidth,
                      pixelHeight: i.pixelHeight, duration: i.duration, fileName: $0)
          } ?? i
        }
    }
  }

  /// Every name now in `parent`, for a refusal of a taken one.
  private func names(in parent: String) -> Set<String> {
    let folders = folders(FixtureLibrary.folders[parent] ?? [], in: parent).map(\.name)
    let files = files(FixtureStore.others(in: parent), in: parent).map(\.name)
    let items = parent.isEmpty ? [] : items(FixtureLibrary.items(under: parent), in: parent).map(\.fileName)
    return Set((folders + files + items).map { $0.lowercased() })
  }

  func makeFolder(_ name: String, in parent: String) throws -> Folder {
    guard !names(in: parent).contains(name.lowercased()) else { throw ChangeFailure.exists }
    let folder = Folder(id: parent + "/" + name, name: name, itemCount: 0, hasSubfolders: false)
    lock.withLock { addedFolders[parent, default: []].append(folder) }
    return folder
  }

  /// A sent file: a photo or clip joins the grid, anything else the folder's files. A taken name gets " (2)".
  func add(_ file: OutgoingFile, to parent: String) -> String {
    let taken = names(in: parent)
    var name = file.name
    let stem = (file.name as NSString).deletingPathExtension, ext = (file.name as NSString).pathExtension
    var n = 2
    while taken.contains(name.lowercased()) {
      name = ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)"
      n += 1
    }
    let kind = OtherFile.Family.media(of: ext)
    lock.withLock {
      if let kind {
        nextItem += 1
        addedItems[parent, default: []].append(
          MediaItem(id: MediaID("item-\(nextItem)"), kind: kind, capturedAt: Date(), pixelWidth: 4032, pixelHeight: 3024,
                    duration: kind == .clip ? 12 : nil, fileName: name))
      } else {
        addedFiles[parent, default: []].append(OtherFile(id: parent + "/" + name, name: name, bytes: file.bytes, modified: Date()))
      }
    }
    return name
  }

  func move(_ entry: FolderEntry, from parent: String, to destination: String) throws {
    guard !names(in: destination).contains(entry.name.lowercased()) else { throw ChangeFailure.exists }
    if case .folder(let f) = entry, destination == f.id || destination.hasPrefix(f.id + "/") { throw ChangeFailure.badName }
    lock.withLock {
      movedFrom[parent, default: []].insert(entry.id)
      movedFrom[destination]?.remove(entry.id)
      switch entry {
      case .folder(let f): addedFolders[destination, default: []].append(f)
      case .file(let f): addedFiles[destination, default: []].append(f)
      case .item(let i): addedItems[destination, default: []].append(i)
      }
    }
  }

  func rename(_ entry: FolderEntry, in parent: String, to name: String) throws {
    guard !names(in: parent).contains(name.lowercased()) else { throw ChangeFailure.exists }
    lock.withLock { renamed[entry.id] = name }
  }

  func delete(_ entry: FolderEntry) { lock.withLock { _ = deleted.insert(entry.id) } }

  /// A document in the fixtures app's own folder ("On My iPhone > MV Fixtures" in Files), for
  /// Add from Files to pick.
  static func seedDocuments() {
    guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
    let file = docs.appendingPathComponent("Trip notes.txt")
    guard !FileManager.default.fileExists(atPath: file.path) else { return }
    try? Data("Lisbon, three days. Tram 28 early, before the crowds.\n".utf8).write(to: file)
  }
}

extension OtherFile.Family {
  /// A photo or clip by its extension, as the server's listing would sort it; nil for any other file.
  static func media(of ext: String) -> MediaKind? {
    switch ext.lowercased() {
    case "jpg", "jpeg", "heic", "heif", "png", "dng", "cr3", "nef", "arw", "tif", "tiff": return .photo
    case "mov", "mp4", "m4v": return .clip
    default: return nil
    }
  }
}

struct FixtureFolderChanges: FolderChanges {
  let world: FixtureWorld

  private var book: FixtureChangeBook { world.changeBook }

  func canChange(in folder: Folder) async -> Bool { world.mayChange && !folder.offline }

  func changeableRoots() async -> [Folder] {
    guard world.mayChange else { return [] }
    return book.folders(FixtureLibrary.folders[""] ?? [], in: "")
  }

  func folders(in folder: Folder) async throws -> [Folder] {
    book.folders(FixtureLibrary.folders[folder.id] ?? [], in: folder.id)
  }

  func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    try allowed(name)
    return try book.makeFolder(name, in: folder.id)
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
    return book.add(file, to: folder.id)
  }

  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    try allowed(entry.name)
    try book.move(entry, from: folder.id, to: destination.id)
  }

  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws {
    try allowed(name)
    try book.rename(entry, in: folder.id, to: name)
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
