// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changes kept on the phone and laid over a library that cannot take them: the sample library's
// demo of Add, New folder, Move, Rename and Delete (a static copy on GitHub, no server behind it),
// and the fixtures'. What was added, made, moved, renamed and deleted is a small JSON file in
// `directory`, beside the files that were added; nil keeps it in memory.
import Foundation

public final class LocalChanges: @unchecked Sendable {
  private struct State: Codable {
    var stamp = ""
    var folders: [String: [SavedFolder]] = [:]
    var files: [String: [SavedFile]] = [:]
    var items: [String: [SavedItem]] = [:]
    /// Moved away from a folder (by the folder's id).
    var movedFrom: [String: Set<String>] = [:]
    /// Deleted: gone from every folder.
    var deleted: Set<String> = []
    var renamed: [String: String] = [:]
    var next = 0
  }

  private struct SavedFolder: Codable {
    let id: String
    let name: String
    let itemCount: Int
    init(_ f: Folder) { id = f.id; name = f.name; itemCount = f.itemCount }
    var folder: Folder { Folder(id: id, name: name, itemCount: itemCount, hasSubfolders: true) }
  }

  private struct SavedFile: Codable {
    let id: String
    let name: String
    let bytes: Int64
    let modified: Date
    init(_ f: OtherFile) { id = f.id; name = f.name; bytes = f.bytes; modified = f.modified }
    var file: OtherFile { OtherFile(id: id, name: name, bytes: bytes, modified: modified) }
  }

  private struct SavedItem: Codable {
    let id: String
    let clip: Bool
    let captured: Date
    let width: Int
    let height: Int
    let duration: TimeInterval?
    let name: String
    /// The catalogue it came from (MediaID.catalogue), so a moved photo keeps its pictures.
    let catalogue: UInt64
    init(_ i: MediaItem) {
      id = i.id.raw; clip = i.kind == .clip; captured = i.capturedAt; width = i.pixelWidth; height = i.pixelHeight
      duration = i.duration; name = i.fileName; catalogue = i.id.catalogue
    }
    var item: MediaItem {
      MediaItem(id: MediaID(id, catalogue: catalogue), kind: clip ? .clip : .photo, capturedAt: captured, pixelWidth: width,
                pixelHeight: height, duration: duration, fileName: name)
    }
  }

  /// The id prefix of what was added on the phone: its bytes are in `directory`.
  public static let addedPrefix = "phone:"

  private let directory: URL?
  private let lock = NSLock()
  private var state: State

  public init(directory: URL?) {
    self.directory = directory
    if let directory, let data = try? Data(contentsOf: directory.appendingPathComponent("changes.json")),
       let saved = try? JSONDecoder().decode(State.self, from: data) {
      state = saved
    } else {
      state = State()
    }
  }

  /// Forgets every change (and the files added) when `stamp` is not the one they were made under, or
  /// their folder has gone (the library forgotten): the sample opened again starts as published.
  public func begin(stamp: String) {
    lock.withLock {
      let removed = directory.map { !FileManager.default.fileExists(atPath: $0.path) } ?? false
      guard state.stamp != stamp || (removed && state.next + state.deleted.count + state.renamed.count
                                                + state.folders.count + state.movedFrom.count > 0) else { return }
      state = State(stamp: stamp)
      if let directory { try? FileManager.default.removeItem(at: directory) }
      save()
    }
  }

  // MARK: The library as changed

  public func folders(_ base: [Folder], in parent: String) -> [Folder] {
    lock.withLock {
      (base.map(SavedFolder.init) + (state.folders[parent] ?? []))
        .filter { !state.deleted.contains($0.id) && !(state.movedFrom[parent]?.contains($0.id) ?? false) }
        .map { saved in
          let f = saved.folder
          return Folder(id: f.id, name: state.renamed[f.id] ?? f.name, itemCount: f.itemCount, hasSubfolders: true)
        }
    }
  }

  public func files(_ base: [OtherFile], in parent: String) -> [OtherFile] {
    lock.withLock {
      (base.map(SavedFile.init) + (state.files[parent] ?? []))
        .filter { !state.deleted.contains($0.id) && !(state.movedFrom[parent]?.contains($0.id) ?? false) }
        .map { saved in
          let f = saved.file
          return state.renamed[f.id].map { OtherFile(id: f.id, name: $0, bytes: f.bytes, modified: f.modified) } ?? f
        }
    }
  }

  /// Newest first: what was added or moved in comes before what was there.
  public func items(_ base: [MediaItem], in parent: String) -> [MediaItem] {
    lock.withLock {
      let gone = state.movedFrom[parent] ?? []
      return ((state.items[parent] ?? []).reversed().map(\.item) + base)
        .filter { !state.deleted.contains($0.id.raw) && !gone.contains($0.id.raw) }
        .map { i in
          state.renamed[i.id.raw].map {
            MediaItem(id: i.id, kind: i.kind, capturedAt: i.capturedAt, pixelWidth: i.pixelWidth,
                      pixelHeight: i.pixelHeight, duration: i.duration, fileName: $0)
          } ?? i
        }
    }
  }

  /// Deleted here (everywhere it was shown).
  public func isDeleted(_ id: String) -> Bool { lock.withLock { state.deleted.contains(id) } }

  /// Made on the phone (a folder the library behind does not know).
  public func isMade(_ folderID: String) -> Bool {
    lock.withLock { state.folders.values.contains { $0.contains { $0.id == folderID } } && !state.deleted.contains(folderID) }
  }

  /// The bytes of a file or photo added on the phone.
  public func localFile(for id: String) -> URL? {
    guard id.hasPrefix(Self.addedPrefix), let directory else { return nil }
    let parts = id.dropFirst(Self.addedPrefix.count).split(separator: "/", maxSplits: 1)
    guard parts.count == 2 else { return nil }
    let url = directory.appendingPathComponent("files/\(parts[0])/\(parts[1])")
    return FileManager.default.fileExists(atPath: url.path) ? url : nil
  }

  // MARK: Changes. `taken`: the names now in the folder concerned, lower-cased.

  public func makeFolder(_ name: String, in parent: String, taken: Set<String>) throws -> Folder {
    guard !taken.contains(name.lowercased()) else { throw ChangeFailure.exists }
    let folder = Folder(id: parent + "/" + name, name: name, itemCount: 0, hasSubfolders: true)
    change { $0.folders[parent, default: []].append(SavedFolder(folder)) }
    return folder
  }

  /// A photo or clip joins the folder's photos, anything else its files; a taken name gets " (2)".
  /// The bytes are copied into `directory` (as from the phone to a server). Returns the name it got.
  public func add(_ file: OutgoingFile, to parent: String, taken: Set<String>) throws -> String {
    var name = file.name
    let stem = (file.name as NSString).deletingPathExtension, ext = (file.name as NSString).pathExtension
    var n = 2
    while taken.contains(name.lowercased()) {
      name = ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)"
      n += 1
    }
    let number = lock.withLock { () -> Int in
      state.next += 1
      return state.next
    }
    let id = "\(Self.addedPrefix)\(number)/\(name)"
    if let directory {
      let place = directory.appendingPathComponent("files/\(number)", isDirectory: true)
      do {
        try FileManager.default.createDirectory(at: place, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: file.url, to: place.appendingPathComponent(name))
      } catch {
        throw ChangeFailure.unreadable
      }
    }
    let kind = Self.media(of: ext)
    change { state in
      if let kind {
        state.items[parent, default: []].append(SavedItem(
          MediaItem(id: MediaID(id), kind: kind, capturedAt: Date(), pixelWidth: 0, pixelHeight: 0, duration: nil,
                    fileName: name)))
      } else {
        state.files[parent, default: []].append(SavedFile(OtherFile(id: id, name: name, bytes: file.bytes, modified: Date())))
      }
    }
    return name
  }

  public func move(_ entry: FolderEntry, from parent: String, to destination: String, taken: Set<String>) throws {
    guard !taken.contains(entry.name.lowercased()) else { throw ChangeFailure.exists }
    if case .folder(let f) = entry, destination == f.id || destination.hasPrefix(f.id + "/") { throw ChangeFailure.badName }
    change { state in
      state.movedFrom[parent, default: []].insert(entry.id)
      state.movedFrom[destination]?.remove(entry.id)
      switch entry {
      case .folder(let f): state.folders[destination, default: []].append(SavedFolder(f))
      case .file(let f): state.files[destination, default: []].append(SavedFile(f))
      case .item(let i): state.items[destination, default: []].append(SavedItem(i))
      }
    }
  }

  public func rename(_ entry: FolderEntry, to name: String, taken: Set<String>) throws {
    guard !taken.contains(name.lowercased()) || name.lowercased() == entry.name.lowercased() else {
      throw ChangeFailure.exists
    }
    change { $0.renamed[entry.id] = name }
  }

  public func delete(_ entry: FolderEntry) { change { $0.deleted.insert(entry.id) } }

  private func change(_ body: (inout State) -> Void) {
    lock.withLock {
      body(&state)
      save()
    }
  }

  /// Under `lock`.
  private func save() {
    guard let directory else { return }
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    if let data = try? JSONEncoder().encode(state) {
      try? data.write(to: directory.appendingPathComponent("changes.json"), options: .atomic)
    }
  }

  /// A photo or clip by its extension, as a server's listing sorts it; nil for any other file.
  public static func media(of ext: String) -> MediaKind? {
    switch ext.lowercased() {
    case "jpg", "jpeg", "heic", "heif", "png", "dng", "cr3", "nef", "arw", "tif", "tiff": return .photo
    case "mov", "mp4", "m4v": return .clip
    default: return nil
    }
  }
}
