// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the files in a server's shared folders (docs/plans/mobile.md §11.1 "Changing files"):
// Add, New folder, Move, Rename and Delete, for a device the server lets change files there. Only a
// library listed live from the server (file mode) has them; a catalogue's folders are what the
// computer last published, so a change would not show there until the next one.
import Foundation

/// One thing in a folder that a change acts on.
public enum FolderEntry: Hashable, Sendable, Identifiable {
  case folder(Folder)
  case item(MediaItem)
  case file(OtherFile)

  public var id: String {
    switch self {
    case .folder(let f): return f.id
    case .item(let i): return i.id.raw
    case .file(let f): return f.id
    }
  }

  public var name: String {
    switch self {
    case .folder(let f): return f.name
    case .item(let i): return i.fileName
    case .file(let f): return f.name
    }
  }

  public var isFolder: Bool { if case .folder = self { return true } else { return false } }
}

/// Why the server did not make a change. Nothing is ever replaced or destroyed on a refusal.
public enum ChangeFailure: Error, Equatable, Sendable {
  /// The name is taken where it was going.
  case exists
  /// This phone may not change files there now (its role, or the folder).
  case notAllowed
  /// A name the server will not take (hidden, unsafe, a folder into itself).
  case badName
  /// The name is too long.
  case nameTooLong
  /// Someone else is changing that file now.
  case busy
  /// The server's disk, or a backup folder's limit, is full.
  case full
  /// The folder's drive is not connected to the server.
  case offline
  /// It is not there any more (moved or deleted elsewhere).
  case notFound
  /// The server is older than these changes.
  case unsupported
  /// The server could not be reached, or the link dropped.
  case unreachable
  /// What arrived was not what was sent.
  case damaged
  case failed
}

/// A file the phone sends: where it is on the phone, and the name it should have.
public struct OutgoingFile: Sendable, Equatable {
  public let url: URL
  public let name: String
  public let bytes: Int64

  public init(url: URL, name: String, bytes: Int64) {
    self.url = url
    self.name = name
    self.bytes = bytes
  }
}

public protocol FolderChanges: Sendable {
  /// Whether this phone may change files in `folder` now: the server offers changes and lets this
  /// device change files there. false for the top of the library (it is the list of folders).
  func canChange(in folder: Folder) async -> Bool
  /// The folders this phone may change, at the top: where Move starts.
  func changeableRoots() async -> [Folder]
  /// The folders inside `folder`, for Move's picker.
  func folders(in folder: Folder) async throws -> [Folder]
  /// Makes a folder named `name` inside `folder`.
  func makeFolder(named name: String, in folder: Folder) async throws -> Folder
  /// Sends `file` into `folder`. Returns the name it got (a taken name gets " (2)"). Cancellable;
  /// sent again, it carries on from where the server's unfinished copy ends.
  func add(_ file: OutgoingFile, to folder: Folder,
           progress: @escaping @Sendable (_ done: Int64, _ total: Int64) -> Void) async throws -> String
  /// Moves `entry` into `folder`, keeping its name.
  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws
  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws
  /// Moves `entry` to the trash on the server, where its owner can restore it.
  func delete(_ entry: FolderEntry, in folder: Folder) async throws
}

/// A library with no changes: a catalogue, the sample library, a server older than them.
public struct NoFolderChanges: FolderChanges {
  public init() {}
  public func canChange(in folder: Folder) async -> Bool { false }
  public func changeableRoots() async -> [Folder] { [] }
  public func folders(in folder: Folder) async throws -> [Folder] { [] }
  public func makeFolder(named name: String, in folder: Folder) async throws -> Folder { throw ChangeFailure.unsupported }
  public func add(_ file: OutgoingFile, to folder: Folder,
                  progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    throw ChangeFailure.unsupported
  }
  public func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    throw ChangeFailure.unsupported
  }
  public func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws { throw ChangeFailure.unsupported }
  public func delete(_ entry: FolderEntry, in folder: Folder) async throws { throw ChangeFailure.unsupported }
}
