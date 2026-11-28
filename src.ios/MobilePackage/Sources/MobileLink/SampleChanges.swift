// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library's demo of changing files (owner: every phone feature can be tried with SAMPLE).
// The sample is a static copy on GitHub with no server behind it, so its changes are kept on this
// phone (MobileKit/LocalChanges.swift, in the library's own folder, which Forget removes) and laid
// over its folders: Add, New Folder, Move, Rename and Delete work as on a server, and the pages say
// it is a demo. Opening the sample again (another generation) starts it as published. Nothing is
// sent anywhere, and the published sample is unchanged.
import Foundation
import ImageIO
import MobileKit
import UIKit

public enum SampleChanges {
  /// The services, with the sample's folders changeable on the phone while the library is the sample.
  public static func wrap(_ services: Services, isSample: @escaping @Sendable () -> Bool,
                          stamp: @escaping @Sendable () -> String) -> Services {
    let book = LocalChanges(directory: LinkFolders.library.appendingPathComponent("sample-changes", isDirectory: true))
    let library = SampleChangesLibrary(base: services.library, book: book, isSample: isSample, stamp: stamp)
    var wrapped = Services(library: library, images: SampleChangesImages(base: services.images, book: book),
                           search: services.search, connection: services.connection, sync: services.sync,
                           clips: services.clips, saver: services.saver, backup: services.backup)
    wrapped.changes = SampleFolderChanges(library: library, other: services.changes)
    return wrapped.with(vpn: services.vpn)
  }
}

/// The library with the sample's changes laid over it (while it is the sample).
struct SampleChangesLibrary: LibraryStore {
  let base: LibraryStore
  let book: LocalChanges
  let isSample: @Sendable () -> Bool
  let stamp: @Sendable () -> String

  /// The changes apply (and are started afresh for a new sample).
  var active: Bool {
    guard isSample() else { return false }
    book.begin(stamp: stamp())
    return true
  }

  func list(in scope: LibraryScope) async throws -> any MediaList {
    guard active else { return try await base.list(in: scope) }
    switch scope {
    case .folder(let folder):
      let listed = book.isMade(folder.id) ? [] : Self.items(of: try await base.list(in: scope))
      return ArrayMediaList(book.items(listed, in: folder.id))
    case .everything:
      let all = try await base.list(in: scope)
      return ArrayMediaList(Self.items(of: all).filter { !book.isDeleted($0.id.raw) })
    case .person:
      return try await base.list(in: scope)
    }
  }

  func folders(under parent: Folder?) async throws -> [Folder] {
    guard active, let parent else { return try await base.folders(under: parent) }
    let listed = book.isMade(parent.id) ? [] : try await base.folders(under: parent)
    return book.folders(listed, in: parent.id)
  }

  func otherFiles(in folder: Folder) async throws -> [OtherFile] {
    guard active else { return try await base.otherFiles(in: folder) }
    let listed = book.isMade(folder.id) ? [] : try await base.otherFiles(in: folder)
    return book.files(listed, in: folder.id)
  }

  func preview(of folder: Folder, count: Int) async -> FolderPreview {
    guard active, !folder.offline else { return await base.preview(of: folder, count: count) }
    guard let list = try? await list(in: .folder(folder)) else { return .empty }
    let others = (try? await otherFiles(in: folder))?.count ?? 0
    return FolderPreview(items: (0..<min(count, list.count)).map { list.item(at: $0) }, files: list.count, folders: nil,
                         others: others)
  }

  func download(_ file: OtherFile, progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> URL {
    if let local = book.localFile(for: file.id) {
      progress(file.bytes, file.bytes)
      return local
    }
    return try await base.download(file, progress: progress)
  }

  func details(for item: MediaItem) async throws -> MediaDetails {
    guard let local = book.localFile(for: item.id.raw) else { return try await base.details(for: item) }
    let bytes = (try? FileManager.default.attributesOfItem(atPath: local.path)[.size] as? NSNumber)?.int64Value ?? 0
    return MediaDetails(place: nil, camera: nil, lens: nil, exposure: nil, folderPath: "",
                        formatName: local.pathExtension.uppercased(), fileBytes: bytes)
  }

  func people() async throws -> [Person] { try await base.people() }
  func offlineFolders() async -> [Folder] { await base.offlineFolders() }

  static func items(of list: any MediaList) -> [MediaItem] { (0..<list.count).map { list.item(at: $0) } }
}

/// Add, New Folder, Move, Rename and Delete against the phone's copy, for the sample's own folders;
/// the server's (`other`) for anything else.
struct SampleFolderChanges: FolderChanges {
  let library: SampleChangesLibrary
  let other: FolderChanges

  private var book: LocalChanges { library.book }

  /// The sample's folders: a catalogue's ("/<root>/<path>"), never the offline one beside them.
  private func sample(_ folder: Folder) -> Bool { library.active && !folder.offline && folder.id.hasPrefix("/") }

  func canChange(in folder: Folder) async -> Bool {
    sample(folder) ? true : await other.canChange(in: folder)
  }

  func changeableRoots() async -> [Folder] {
    guard library.active else { return await other.changeableRoots() }
    return ((try? await library.folders(under: nil)) ?? []).filter { sample($0) }
  }

  func folders(in folder: Folder) async throws -> [Folder] {
    sample(folder) ? try await library.folders(under: folder) : try await other.folders(in: folder)
  }

  /// The names now in `folder`, lower-cased.
  private func taken(in folder: Folder) async -> Set<String> {
    let folders = (try? await library.folders(under: folder)) ?? []
    let files = (try? await library.otherFiles(in: folder)) ?? []
    let items = (try? await library.list(in: .folder(folder))).map(SampleChangesLibrary.items(of:)) ?? []
    return Set((folders.map(\.name) + files.map(\.name) + items.map(\.fileName)).map { $0.lowercased() })
  }

  func makeFolder(named name: String, in folder: Folder) async throws -> Folder {
    guard sample(folder) else { return try await other.makeFolder(named: name, in: folder) }
    return try book.makeFolder(name, in: folder.id, taken: await taken(in: folder))
  }

  /// Copied onto the phone, with its progress, as it would go to a server.
  func add(_ file: OutgoingFile, to folder: Folder,
           progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> String {
    guard sample(folder) else { return try await other.add(file, to: folder, progress: progress) }
    let taken = await taken(in: folder)
    let book = book
    let name = try await Task.detached(priority: .userInitiated) { try book.add(file, to: folder.id, taken: taken) }.value
    try Task.checkCancellation()
    progress(file.bytes, file.bytes)
    return name
  }

  func move(_ entry: FolderEntry, from folder: Folder, to destination: Folder) async throws {
    guard sample(folder) else { return try await other.move(entry, from: folder, to: destination) }
    guard sample(destination) else { throw ChangeFailure.notAllowed }
    try book.move(entry, from: folder.id, to: destination.id, taken: await taken(in: destination))
  }

  func rename(_ entry: FolderEntry, in folder: Folder, to name: String) async throws {
    guard sample(folder) else { return try await other.rename(entry, in: folder, to: name) }
    try book.rename(entry, to: name, taken: await taken(in: folder))
  }

  func delete(_ entry: FolderEntry, in folder: Folder) async throws {
    guard sample(folder) else { return try await other.delete(entry, in: folder) }
    book.delete(entry)
  }
}

/// Pictures of what was added on the phone, from its copy; everything else from the library.
struct SampleChangesImages: ImageLadder {
  let base: ImageLadder
  let book: LocalChanges
  private static let memory = NSCache<NSString, UIImage>()

  func cachedImage(for id: MediaID) -> UIImage? {
    guard id.raw.hasPrefix(LocalChanges.addedPrefix) else { return base.cachedImage(for: id) }
    return Self.memory.object(forKey: id.raw as NSString)
  }

  func placeholder(for item: MediaItem) async -> UIImage? {
    added(item) ? nil : await base.placeholder(for: item)
  }

  func face(for person: Person) async -> UIImage? { await base.face(for: person) }
  func cachedFace(for person: Person) -> UIImage? { base.cachedFace(for: person) }

  func thumbnail(for item: MediaItem) async -> UIImage? {
    guard let local = local(item) else { return await base.thumbnail(for: item) }
    let image = await Self.picture(of: local, longest: 384)
    if let image { Self.memory.setObject(image, forKey: item.id.raw as NSString) }
    return image
  }

  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    guard let local = local(item) else { return base.ladder(for: item, fittingPixels: fittingPixels) }
    return AsyncThrowingStream { continuation in
      Task {
        if let image = await Self.picture(of: local, longest: max(fittingPixels.width, fittingPixels.height, 1024)) {
          continuation.yield(.image(image, isOriginal: true))
        }
        continuation.finish()
      }
    }
  }

  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    base.prefetch(items.filter { !added($0) }, fittingPixels: fittingPixels)
  }

  func length(for item: MediaItem) async -> TimeInterval? { added(item) ? nil : await base.length(for: item) }

  private func added(_ item: MediaItem) -> Bool { item.id.raw.hasPrefix(LocalChanges.addedPrefix) }
  private func local(_ item: MediaItem) -> URL? { added(item) ? book.localFile(for: item.id.raw) : nil }

  /// Off the main thread: the file's picture, at most `longest` pixels on its long side.
  private static func picture(of url: URL, longest: CGFloat) async -> UIImage? {
    await Task.detached(priority: .userInitiated) {
      guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
      let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                      kCGImageSourceCreateThumbnailWithTransform: true,
                                      kCGImageSourceThumbnailMaxPixelSize: longest]
      return CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary).map(UIImage.init(cgImage:))
    }.value
  }
}
