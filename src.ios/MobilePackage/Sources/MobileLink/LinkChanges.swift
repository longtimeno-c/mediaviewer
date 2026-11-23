// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the files in the server's folders, in file mode (MobileKit/FolderChanges.swift), over
// mv_files_add / _mkdir / _move / _delete. Every call is on the link's queue, never the main thread.
import CMobileLink
import Foundation
import MobileKit
import MobileLinkSupport

public struct FileModeChanges: FolderChanges {
  let files: LinkFiles

  public init(files: LinkFiles) { self.files = files }

  private var connection: LinkConnection { files.connection }

  public func canChange(in folder: Folder) async -> Bool {
    guard !folder.offline, let path = FilePath(id: folder.id), let link = await connection.awaitLink() else { return false }
    return await onLinkQueue {
      var can: Int32 = 0
      guard mv_files_can_change(link.raw, &can) == 0, can == 1 else { return false }
      return Self.writableRoots(link).contains(path.root)
    }
  }

  public func changeableRoots() async -> [Folder] {
    guard let link = await connection.awaitLink(), let roots = try? await files.roots() else { return [] }
    let writable = await onLinkQueue { Self.writableRoots(link) }
    return roots.filter { !$0.offline && FilePath(id: $0.id).map { writable.contains($0.root) } == true }
  }

  public func folders(in folder: Folder) async throws -> [Folder] {
    guard let path = FilePath(id: folder.id) else { return [] }
    return try await files.list(path).folders
  }

  public func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    let path = try place(folder).appending(name)
    let done = try await run { link, out in
      path.root.withCString { r in path.rel.withCString { mv_files_mkdir(link.raw, r, $0, out) } }
    }
    let made = FilePath(root: path.root, rel: done)
    return Folder(id: made.id, name: made.name, itemCount: -1, hasSubfolders: true)
  }

  public func add(_ file: OutgoingFile, to folder: Folder,
                  progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    let path = try place(folder).appending(file.name)
    guard let link = await connection.awaitLink() else { throw ChangeFailure.unreachable }
    final class Progress { let report: @Sendable (Int64, Int64) -> Void; init(_ r: @escaping @Sendable (Int64, Int64) -> Void) { report = r } }
    let box = Progress(progress)
    let (status, out) = await withLinkCancel { cancel in
      await onLinkQueue { () -> (Int32, mv_change_result) in
        var out = mv_change_result()
        let status = path.root.withCString { r in
          path.rel.withCString { rel in
            file.url.path.withCString { local in
              mv_files_add(link.raw, r, rel, local, cancel.raw, { ctx, done, total in
                guard let ctx else { return }
                Unmanaged<Progress>.fromOpaque(ctx).takeUnretainedValue().report(Int64(done), Int64(total))
              }, Unmanaged.passUnretained(box).toOpaque(), &out)
            }
          }
        }
        return (status, out)
      }
    }
    withExtendedLifetime(box) {}
    if status == 11 { throw CancellationError() }  // MV_LINK_CANCELLED
    guard status == 0 else { throw Self.failure(status, out.why) }
    // A place too long to hand back is still a file that arrived, under the name it was sent as.
    let rel = Self.rel(out)
    return rel.isEmpty ? file.name : FilePath(root: path.root, rel: rel).name
  }

  public func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    let source = try place(entry)
    let target = try place(destination).appending(source.name)
    try await move(source, to: target)
  }

  public func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws {
    let source = try place(entry)
    guard let parent = source.parent else { throw ChangeFailure.badName }
    let target = parent.appending(name)
    // Only the case changes: on a server volume that ignores case the new name is "taken" by the
    // file itself, so it goes by way of a name of its own, and back if the second step is refused.
    if name.lowercased() == source.name.lowercased() {
      let between = parent.appending(name + " (renaming)")
      try await move(source, to: between)
      do {
        try await move(between, to: target)
      } catch {
        try? await move(between, to: source)
        throw error
      }
      return
    }
    try await move(source, to: target)
  }

  public func delete(_ entry: FolderEntry, in folder: Folder) async throws {
    let source = try place(entry)
    _ = try await run { link, out in
      source.root.withCString { r in source.rel.withCString { mv_files_delete(link.raw, r, $0, out) } }
    }
  }

  private func move(_ source: FilePath, to target: FilePath) async throws {
    _ = try await run { link, out in
      source.root.withCString { r in
        source.rel.withCString { rel in
          target.root.withCString { tr in target.rel.withCString { mv_files_move(link.raw, r, rel, tr, $0, out) } }
        }
      }
    }
  }

  /// One blocking change on the link's queue: the `rel` it answers with, or its refusal.
  private func run(_ call: @escaping @Sendable (LinkHandle, UnsafeMutablePointer<mv_change_result>) -> Int32) async throws -> String {
    guard let link = await connection.awaitLink() else { throw ChangeFailure.unreachable }
    let (status, out) = await onLinkQueue { () -> (Int32, mv_change_result) in
      var out = mv_change_result()
      let status = call(link, &out)
      return (status, out)
    }
    guard status == 0 else { throw Self.failure(status, out.why) }
    return Self.rel(out)
  }

  private func place(_ folder: Folder) throws -> FilePath {
    guard let path = FilePath(id: folder.id) else { throw ChangeFailure.unsupported }
    return path
  }

  private func place(_ entry: FolderEntry) throws -> FilePath {
    guard let path = FilePath(id: entry.id), !path.rel.isEmpty else { throw ChangeFailure.unsupported }
    return path
  }

  private static func rel(_ out: mv_change_result) -> String {
    withUnsafeBytes(of: out.rel) { raw in
      String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
    }
  }

  private static func writableRoots(_ link: LinkHandle) -> Set<String> {
    final class Ids { var set: Set<String> = [] }
    let ids = Ids()
    let status = mv_files_writable_roots(link.raw, { ctx, id, writable in
      guard let ctx, let id, writable != 0 else { return }
      Unmanaged<Ids>.fromOpaque(ctx).takeUnretainedValue().set.insert(String(cString: id))
    }, Unmanaged.passUnretained(ids).toOpaque())
    return status == 0 ? ids.set : []
  }

  /// A status and mv_change_why as the screens' refusal.
  static func failure(_ status: Int32, _ why: Int32) -> ChangeFailure {
    switch why {
    case 1: return .unsupported
    case 2: return .notAllowed
    case 3: return .badName
    case 4: return .exists
    case 5: return .busy
    case 6: return .nameTooLong
    case 7, 8: return .full
    case 9: return .offline
    case 10: return .notFound
    case 11: return .damaged
    case 14: return .unreadable
    default:
      switch status {
      case 1: return .badName        // MV_LINK_INVALID: a path the link refuses before sending
      case 3: return .unreachable    // MV_LINK_UNREACHABLE
      case 9: return .notFound       // MV_LINK_NOT_FOUND
      case 22: return .unsupported   // MV_LINK_UNSUPPORTED
      default: return .failed
      }
    }
  }
}
