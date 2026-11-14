// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode (docs/plans/phone-in-the-app.md §4): a paired library with no catalogue yet is its NAS's
// named folders, listed through the service (mv_files_*), and searched the way the desktop's File
// search searches (docs/design/16 "File search"): over the folder on screen only, by name and then
// by tag. Tags come from each file's head (the first 256 KiB), read as the folder is shown, parsed by
// ImageIO and kept by path, size and mtime. The grid's pictures come from the same head (the
// embedded preview), else a small downscale. Nothing here runs on the main thread except the
// in-memory lookups, and nothing logs a path or a name (CLAUDE.md rule 6).
import AVFoundation
import CMobileLink
import CryptoKit
import Foundation
import ImageIO
import MobileKit
import MobileLinkSupport
import UIKit

/// One file a listing showed.
struct FileEntry: Sendable {
  let path: FilePath
  let size: UInt64
  let mtime: Int64
  /// mv_file_entry.kind: 0 photo, 1 RAW, 2 clip.
  let kind: Int32
  let item: MediaItem

  var stamp: String { path.stamp(size: size, mtime: mtime) }
}

public final class LinkFiles: @unchecked Sendable {
  private let connection: LinkConnection
  private let originals: OriginalStore
  private let lock = NSLock()
  /// The files of the folders listed lately, by item id: what opening one needs. Bounded to the
  /// last `recent.limit` folders (the one on screen always kept).
  private var known: [String: FileEntry] = [:]
  private var knownByFolder: [String: [String]] = [:]
  private var recent = RecentFolders(limit: 32)
  /// Listings in flight, by folder id: the library asks for a folder's files and its subfolders at once.
  private var listing: [String: Task<FolderListing, Error>] = [:]
  private var showing: Shown?
  private var generation = 0
  private var factsLibrary = ""
  private var facts: FileFactCache?
  private let memory = ScopedCache<UIImage>(totalCostLimit: 96 << 20)
  private let heads = HeadReads()
  private let posters = OneAtATimeGate(limit: 1)
  private let wholeReads = OneAtATimeGate(limit: 2)
  /// The viewer's ±2 and the like: two at a time, and a fast scroll drops what it cannot start.
  private let prefetches = OneAtATimeGate(limit: 2)
  private var prefetchesWaiting = 0
  static let prefetchQueueLimit = 6

  /// The grid's picture: on screen at 120 pt, three pixels a point.
  static let thumbPixels = 384
  /// A file with no picture in its head is read whole for its grid picture only up to this size.
  static let wholeReadLimit: UInt64 = 8 << 20

  init(connection: LinkConnection, originals: OriginalStore) {
    self.connection = connection
    self.originals = originals
    Task.detached(priority: .utility) { [weak self] in
      for await snapshot in connection.updates() where !snapshot.hasLibrary {
        self?.forgetAll()
      }
    }
    // The tag cache is written in batches; what is left goes when the app is left.
    NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil,
                                           queue: nil) { [weak self] _ in
      guard let self else { return }
      Task.detached(priority: .utility) { await self.saveFacts(force: true) }
    }
  }

  private func saveFacts(force: Bool) async {
    guard let cache = lock.withLock({ facts }), let link = await connection.awaitLink() else { return }
    let connection = self.connection
    await onLinkQueue {
      let write: (() throws -> Void) throws -> Bool = { body in try connection.whileLive(link) { try body() } != nil }
      if force { cache.save(write: write) } else { cache.saveIfDue(write: write) }
    }
  }

  public var library: LibraryStore { FileModeLibrary(files: self) }
  public var search: SearchService { FileModeSearch(files: self) }

  /// Whether an item or a folder is file mode's (rather than a catalogue's).
  public static func isFileID(_ raw: String) -> Bool { FilePath.isFile(raw) }

  private func forgetAll() {
    lock.withLock {
      known.removeAll()
      knownByFolder.removeAll()
      recent.removeAll()
      showing = nil
      generation += 1
      facts?.removeAll()
      facts = nil
      factsLibrary = ""
    }
    memory.removeAll()
  }

  func entry(for id: MediaID) -> FileEntry? { lock.withLock { known[id.raw] } }

  // MARK: Listing

  struct FolderListing: Sendable {
    let folders: [Folder]
    /// Newest first, as every list the screens show is.
    let files: [FileEntry]
  }

  func roots() async throws -> [Folder] {
    guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }
    let (status, rows) = await onLinkQueue { () -> (Int32, [(String, String)]) in
      final class Rows { var list: [(String, String)] = [] }
      let rows = Rows()
      let status = mv_files_roots(link.raw, { ctx, id, label in
        guard let ctx, let id, let label else { return }
        Unmanaged<Rows>.fromOpaque(ctx).takeUnretainedValue().list.append((String(cString: id), String(cString: label)))
      }, Unmanaged.passUnretained(rows).toOpaque())
      return (status, rows.list)
    }
    guard status == 0 else { throw Self.failure(status) }
    return rows.map { Folder(id: FilePath(root: $0.0, rel: "").id, name: $0.1, itemCount: -1, hasSubfolders: true) }
      .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
  }

  func list(_ folder: FilePath) async throws -> FolderListing {
    let key = folder.id
    let task: Task<FolderListing, Error> = lock.withLock {
      if let running = listing[key] { return running }
      let task = Task.detached(priority: .userInitiated) { [self] in try await readListing(folder) }
      listing[key] = task
      return task
    }
    defer { lock.withLock { if listing[key] == task { listing[key] = nil } } }
    return try await task.value
  }

  private func readListing(_ folder: FilePath) async throws -> FolderListing {
    guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }
    struct Row { let name: String; let size: UInt64; let mtime: Int64; let dir: Bool; let kind: Int32 }
    let (status, rows) = await onLinkQueue { () -> (Int32, [Row]) in
      final class Rows { var list: [Row] = [] }
      let rows = Rows()
      let status = folder.root.withCString { root in
        folder.rel.withCString { rel in
          mv_files_list(link.raw, root, rel, { ctx, e in
            guard let ctx, let e, let name = e.pointee.name else { return }
            Unmanaged<Rows>.fromOpaque(ctx).takeUnretainedValue().list.append(
              Row(name: String(cString: name), size: e.pointee.size, mtime: e.pointee.mtime, dir: e.pointee.dir != 0, kind: e.pointee.kind))
          }, Unmanaged.passUnretained(rows).toOpaque())
        }
      }
      return (status, rows.list)
    }
    guard status == 0 else { throw Self.failure(status) }
    var folders: [Folder] = []
    var files: [FileEntry] = []
    for row in rows {
      let path = folder.appending(row.name)
      if row.dir {
        folders.append(Folder(id: path.id, name: row.name, itemCount: -1, hasSubfolders: true))
        continue
      }
      let item = MediaItem(id: MediaID(path.id), kind: row.kind == 2 ? .clip : .photo,
                           capturedAt: Date(timeIntervalSince1970: TimeInterval(row.mtime)), pixelWidth: 0, pixelHeight: 0,
                           duration: nil, fileName: row.name)
      files.append(FileEntry(path: path, size: row.size, mtime: row.mtime, kind: row.kind, item: item))
    }
    folders.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    files.sort { $0.mtime != $1.mtime ? $0.mtime > $1.mtime : $0.path.rel < $1.path.rel }
    remember(files, in: folder)
    return FolderListing(folders: folders, files: files)
  }

  /// Keeps a listing's files for opening, and forgets the folders listed longest ago.
  private func remember(_ files: [FileEntry], in folder: FilePath) {
    let key = folder.id
    lock.withLock {
      for id in knownByFolder[key] ?? [] { known[id] = nil }
      for f in files { known[f.item.id.raw] = f }
      knownByFolder[key] = files.map(\.item.id.raw)
      for gone in recent.touch(key, keep: showing?.folder?.id) {
        for id in knownByFolder.removeValue(forKey: gone) ?? [] { known[id] = nil }  // a file is in one folder
      }
    }
  }

  static func failure(_ status: Int32) -> MediaFailure {
    switch status {
    case 9: return .originalGone
    case 10, 18: return .folderNotOnNAS
    case 5: return .synologyUnreachable
    default: return .synologyUnreachable
    }
  }

  // MARK: The folder on screen

  /// The folder the library shows, its file index for search, and how far its tag pass has got.
  final class Shown: @unchecked Sendable {
    let folder: FilePath?
    let name: String
    let files: [FileEntry]
    let index: FileIndex
    /// Photos the pass reads (clips carry no tags it can read from a head).
    let toRead: Int
    /// Each file's place in `files`, by stamp: where a head read anywhere puts its fields.
    let positions: [String: Int]
    private let lock = NSLock()
    private var read = 0
    private var ended = false

    init(folder: FilePath?, name: String, files: [FileEntry]) {
      self.folder = folder
      self.name = name
      self.files = files
      index = FileIndex(names: files.map(\.path.name))
      toRead = files.filter { $0.kind != 2 }.count
      positions = Dictionary(files.enumerated().map { ($1.stamp, $0) }, uniquingKeysWith: { first, _ in first })
    }

    func noteRead() { lock.withLock { read += 1 } }
    /// The pass is over, however it ended (done, stopped, or never able to start): nothing is
    /// still being read, so search stops saying so.
    func endPass() { lock.withLock { ended = true } }
    var progress: SearchAnswer.Progress? {
      lock.withLock { !ended && read < toRead ? SearchAnswer.Progress(done: read, total: toRead) : nil }
    }
  }

  var shownFolder: Shown? { lock.withLock { showing } }

  /// The library has put this folder on screen (nil: the named folders, which hold no files).
  /// Search looks here, and its tags start to be read; a folder left stops its pass.
  func show(_ folder: FilePath?, name: String, files: [FileEntry]) {
    let shown = Shown(folder: folder, name: name, files: files)
    let mine = lock.withLock { () -> Int in
      showing = shown
      generation += 1
      return generation
    }
    guard folder != nil, shown.toRead > 0 else {
      shown.endPass()
      return
    }
    Task.detached(priority: .utility) { [self] in await readTags(of: shown, generation: mine) }
  }

  private func isCurrent(_ generation: Int) -> Bool { lock.withLock { self.generation == generation } }

  private func factCache() -> FileFactCache? {
    let library = connection.libraryID
    guard !library.isEmpty else { return nil }
    let cache: FileFactCache = lock.withLock {
      if let facts, factsLibrary == library { return facts }
      let made = FileFactCache(file: LinkFolders.fileFacts(for: library))
      facts = made
      factsLibrary = library
      return made
    }
    cache.load()
    return cache
  }

  /// The background tag pass: what the cache knows at once, then each photo's head, two at a time.
  /// Each head read also makes the file's grid picture when the head holds one, so a file's head
  /// is read once for both.
  private func readTags(of shown: Shown, generation: Int) async {
    defer { shown.endPass() }
    guard let cache = factCache(), let link = await connection.awaitLink() else { return }
    var unread: [(Int, FileEntry)] = []
    for (i, file) in shown.files.enumerated() where file.kind != 2 {
      if let known = cache.facts(for: file.stamp) {
        shown.index.setFields(i, known.fields)
        shown.noteRead()
      } else {
        unread.append((i, file))
      }
    }
    let work = { @Sendable [self] (i: Int, file: FileEntry) async -> Void in
      guard isCurrent(generation), !Task.isCancelled else { return }
      // A tile may have read this head meanwhile, and learnt its tags with it.
      if cache.facts(for: file.stamp) == nil, let head = await heads.head(of: file, link: link) {
        await use(head, of: file, link: link, cache: cache, thumbnail: true)
      }
      if let known = cache.facts(for: file.stamp) { shown.index.setFields(i, known.fields) }
      shown.noteRead()
    }
    var queue = unread[...]
    await withTaskGroup(of: Void.self) { group in
      for _ in 0..<2 {
        guard let (i, file) = queue.popFirst() else { break }
        group.addTask(priority: .utility) { await work(i, file) }
      }
      while await group.next() != nil {
        guard isCurrent(generation) else {
          group.cancelAll()
          break
        }
        if let (i, file) = queue.popFirst() { group.addTask(priority: .utility) { await work(i, file) } }
      }
    }
    await saveFacts(force: false)
  }

  /// Everything a head gives: the file's tags (into the cache, and into the search index if its
  /// folder is on screen) and, with `thumbnail`, its grid picture when the head holds one.
  private func use(_ head: Data, of file: FileEntry, link: LinkHandle, cache: FileFactCache?, thumbnail: Bool) async {
    if let cache, file.kind != 2, cache.facts(for: file.stamp) == nil {
      let values = FileTags.read(head: head)
      let facts = values.map { FileFacts(fields: Self.fields(of: $0), camera: Self.cameraLine($0), lens: $0.lens) }
        ?? FileFacts(fields: [], camera: "", lens: "")
      cache.set(facts, for: file.stamp)
      if let shown = shownFolder, let i = shown.positions[file.stamp] { shown.index.setFields(i, facts.fields) }
    }
    if thumbnail, memoryImage(of: file) == nil, !thumbOnDisk(file),
       let image = await headPicture(head, of: file) {
      await keep(image, of: file, link: link)
    }
  }

  static func fields(of values: FileTagValues) -> [String] {
    final class Out { var list: [String] = [] }
    let out = Out()
    var keep: [UnsafeMutablePointer<CChar>] = []
    defer { keep.forEach { free($0) } }
    func c(_ s: String) -> UnsafePointer<CChar> {
      let p = strdup(s)!
      keep.append(p)
      return UnsafePointer(p)
    }
    let tags = values.tags.map { mv_file_tag(raw_tag: c($0.rawTag), value: c($0.value)) }
    _ = tags.withUnsafeBufferPointer { buffer in
      mv_files_fields(c(values.make), c(values.model), c(values.lens), c(values.comment), buffer.baseAddress, buffer.count, { ctx, text in
        guard let ctx, let text else { return }
        Unmanaged<Out>.fromOpaque(ctx).takeUnretainedValue().list.append(String(cString: text))
      }, Unmanaged.passUnretained(out).toOpaque())
    }
    return out.list
  }

  /// The camera line the details show, as the desktop makes it: the fields' first when it is the camera.
  static func cameraLine(_ values: FileTagValues) -> String {
    let make = values.make, model = values.model
    if make.isEmpty || model.lowercased().hasPrefix(make.lowercased()) { return model }
    if model.isEmpty { return make }
    return make + " " + model
  }

  func facts(for item: MediaItem) -> FileFacts? {
    guard let entry = entry(for: item.id) else { return nil }
    return lock.withLock { facts }?.facts(for: entry.stamp)
  }

  // MARK: Pictures

  func memoryImage(for id: MediaID) -> UIImage? {
    guard let file = entry(for: id) else { return nil }
    return memoryImage(of: file)
  }

  /// Keyed by where the file is, its size and its time, as the disk copy is: a file changed in
  /// place is never shown with its old picture.
  private func memoryImage(of file: FileEntry) -> UIImage? {
    memory.object(library: connection.libraryID, generation: 0, id: file.stamp)
  }

  private func diskThumb(_ file: FileEntry) -> URL {
    LinkFolders.fileThumbs(for: connection.libraryID).appendingPathComponent(Self.hashName(file.stamp) + ".jpg")
  }

  private func thumbOnDisk(_ file: FileEntry) -> Bool { FileManager.default.fileExists(atPath: diskThumb(file).path) }

  /// Into memory, and to disk for the next session.
  private func keep(_ image: UIImage, of file: FileEntry, link: LinkHandle) async {
    memory.set(image, library: connection.libraryID, generation: 0, id: file.stamp,
               cost: Int(image.size.width * image.size.height * image.scale * image.scale * 4))
    guard let data = image.jpegData(compressionQuality: 0.8) else { return }
    let disk = diskThumb(file)
    let connection = self.connection
    await onLinkQueue {
      _ = connection.whileLive(link) {
        try? FileManager.default.createDirectory(at: disk.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: disk, options: .atomic)
      }
    }
  }

  /// The grid's picture: memory, the disk cache, then the file's head (an embedded preview of
  /// at least 256 px, or the whole picture when the head is all of it), then a whole read of a
  /// file of at most 8 MiB, or a clip's first frame. nil leaves the tile its quiet glyph. A head
  /// read here also gives the file's tags.
  func thumbnail(for item: MediaItem) async -> UIImage? {
    guard let file = entry(for: item.id) else { return nil }
    if let hit = memoryImage(of: file) { return hit }
    guard let link = await connection.awaitLink() else { return nil }
    let disk = diskThumb(file)
    if let image = await onLinkQueue({ UIImage(contentsOfFile: disk.path)?.preparingForDisplay() }) {
      memory.set(image, library: connection.libraryID, generation: 0, id: file.stamp,
                 cost: Int(image.size.width * image.size.height * image.scale * image.scale * 4))
      return image
    }
    guard !Task.isCancelled, let image = await makeThumbnail(file, link: link) else { return nil }
    await keep(image, of: file, link: link)
    return image
  }

  /// The viewer's neighbours: behind a small gate, and dropped when too many already wait.
  func prefetch(_ item: MediaItem) {
    guard let file = entry(for: item.id), memoryImage(of: file) == nil else { return }
    let admitted = lock.withLock { () -> Bool in
      guard prefetchesWaiting < Self.prefetchQueueLimit else { return false }
      prefetchesWaiting += 1
      return true
    }
    guard admitted else { return }
    Task.detached(priority: .utility) { [self] in
      _ = await prefetches.run { await thumbnail(for: item) }
      lock.withLock { prefetchesWaiting -= 1 }
    }
  }

  private func makeThumbnail(_ file: FileEntry, link: LinkHandle) async -> UIImage? {
    if file.kind == 2 { return await poster(of: file, link: link) }
    guard let head = await heads.head(of: file, link: link) else { return nil }
    await use(head, of: file, link: link, cache: factCache(), thumbnail: false)
    if let image = await headPicture(head, of: file) { return image }
    guard UInt64(head.count) < file.size, file.size <= Self.wholeReadLimit, !Task.isCancelled else { return nil }
    let data: Data? = await wholeReads.run { await Self.readAll(link: link, file: file) }
    guard let data else { return nil }
    return await onLinkQueue { Self.scaled(data, maxPixels: Self.thumbPixels) }
  }

  /// The picture a head holds for the grid: all of it when the head is the whole file, else an
  /// embedded preview of at least 256 px (HeadPicture), scaled to the grid's size.
  private func headPicture(_ head: Data, of file: FileEntry) async -> UIImage? {
    if UInt64(head.count) >= file.size { return await onLinkQueue { Self.scaled(head, maxPixels: Self.thumbPixels) } }
    return await onLinkQueue {
      guard let source = CGImageSourceCreateWithData(head as CFData, nil) else { return nil }
      let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any] ?? [:]
      let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue ?? 0
      let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue ?? 0
      let options: [CFString: Any] = [kCGImageSourceThumbnailMaxPixelSize: HeadPicture.probePixels,
                                      kCGImageSourceCreateThumbnailFromImageIfAbsent: false,
                                      kCGImageSourceCreateThumbnailWithTransform: true]
      guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary),
            HeadPicture.isUsablePreview(longSide: max(cg.width, cg.height), imageLongSide: max(width, height)) else { return nil }
      return Self.fitted(cg, maxPixels: Self.thumbPixels)
    }
  }

  /// A whole picture's bytes to the grid's size. Link queue only.
  private static func scaled(_ data: Data, maxPixels: Int) -> UIImage? {
    guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
    let options: [CFString: Any] = [kCGImageSourceThumbnailMaxPixelSize: maxPixels,
                                    kCGImageSourceCreateThumbnailFromImageAlways: true,
                                    kCGImageSourceCreateThumbnailWithTransform: true,
                                    kCGImageSourceShouldCacheImmediately: true]
    return CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary).map { UIImage(cgImage: $0) }
  }

  /// A preview larger than the grid's size, drawn down to it.
  private static func fitted(_ cg: CGImage, maxPixels: Int) -> UIImage {
    let long = max(cg.width, cg.height)
    guard long > maxPixels else { return UIImage(cgImage: cg) }
    let scale = CGFloat(maxPixels) / CGFloat(long)
    let size = CGSize(width: (CGFloat(cg.width) * scale).rounded(), height: (CGFloat(cg.height) * scale).rounded())
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    return UIGraphicsImageRenderer(size: size, format: format).image { _ in
      UIImage(cgImage: cg).draw(in: CGRect(origin: .zero, size: size))
    }
  }

  /// A clip's first frame, one clip at a time: AVFoundation reads what it needs over the link.
  private func poster(of file: FileEntry, link: LinkHandle) async -> UIImage? {
    let ext = (file.path.name as NSString).pathExtension.lowercased()
    guard ["mp4", "mov", "m4v"].contains(ext) else { return nil }
    return await posters.run { [self] in
      guard !Task.isCancelled, let (asset, loader) = clipAsset(file, link: link) else { return nil }
      let generator = AVAssetImageGenerator(asset: asset)
      generator.appliesPreferredTrackTransform = true
      generator.maximumSize = CGSize(width: Self.thumbPixels, height: Self.thumbPixels)
      let image = try? await generator.image(at: .zero).image
      return withExtendedLifetime(loader) { image.map { UIImage(cgImage: $0) } }
    }
  }

  private static func readAll(link: LinkHandle, file: FileEntry) async -> Data? {
    await withLinkCancel { cancel in
      await onLinkQueue {
        final class Sink { var data = Data() }
        let sink = Sink()
        sink.data.reserveCapacity(Int(file.size))
        let status = file.path.root.withCString { root in
          file.path.rel.withCString { rel in
            mv_files_read(link.raw, root, rel, 0, file.size, 0, cancel.raw, { ctx, _, bytes, count in
              guard let ctx, let bytes else { return 0 }
              Unmanaged<Sink>.fromOpaque(ctx).takeUnretainedValue().data.append(bytes, count: count)
              return 1
            }, Unmanaged.passUnretained(sink).toOpaque())
          }
        }
        return status == 0 ? sink.data : nil
      }
    }
  }

  static func hashName(_ stamp: String) -> String {
    SHA256.hash(data: Data(stamp.utf8)).prefix(16).map { String(format: "%02x", $0) }.joined()
  }

  // MARK: Originals and clips

  /// The whole file on the phone, from the store when it is already there.
  func originalFile(_ file: FileEntry, progress: @escaping (Int64, Int64) -> Void) async throws -> URL {
    guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }
    let total = Int64(file.size)
    progress(0, total)
    let library = String(connection.libraryID.unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) })
    let connection = self.connection
    let read = readFunction(file, link: link)
    do {
      return try await originals.file(key: "\(library)-f\(Self.hashName(file.stamp))-\(file.size)", size: file.size,
                                      place: { move in try connection.whileLive(link) { try move() } != nil }) { part in
        let status = await withLinkCancel { cancel in
          await onLinkQueue { () -> Int32 in
            guard let made = connection.whileLive(link, { FileManager.default.createFile(atPath: part.path, contents: nil) }) else { return 11 }
            guard made, let handle = try? FileHandle(forWritingTo: part) else { return 13 }
            defer { try? handle.close() }
            var written: Int64 = 0
            var reported: Int64 = 0
            return read(0, file.size, cancel) { data in
              do { try handle.write(contentsOf: data) } catch { return false }
              written += Int64(data.count)
              if written - reported >= 256 * 1024 {
                reported = written
                progress(written, total)
              }
              return true
            }
          }
        }
        switch status {
        case 0: return
        case 11: throw CancellationError()
        default: throw Self.failure(status)
        }
      }
    } catch is OriginalStoreError {
      throw MediaFailure.synologyUnreachable  // cut short: the next look asks again
    }
  }

  /// Reads a range of the file over the link: what a clip's loader and a whole-file copy use.
  func readFunction(_ file: FileEntry, link: LinkHandle) -> ClipRead {
    let root = file.path.root, rel = file.path.rel
    return { start, length, cancel, sink in
      let box = Unmanaged.passRetained(BlockBox(sink))
      defer { box.release() }
      return root.withCString { r in
        rel.withCString { p in
          mv_files_read(link.raw, r, p, start, length, 1, cancel.raw, { ctx, _, bytes, count in
            guard let ctx, let bytes else { return 0 }
            return Unmanaged<BlockBox>.fromOpaque(ctx).takeUnretainedValue().body(Data(bytes: bytes, count: count)) ? 1 : 0
          }, box.toOpaque())
        }
      }
    }
  }

  /// An AVURLAsset whose bytes come over the link; keep the loader alive as long as the asset.
  func clipAsset(_ file: FileEntry, link: LinkHandle) -> (AVURLAsset, ClipLoader)? {
    let ext = (file.path.name as NSString).pathExtension.lowercased()
    var parts = URLComponents()
    parts.scheme = "mvfile"
    parts.host = "file"
    parts.path = "/\(Self.hashName(file.stamp)).\(ext)"
    guard let url = parts.url else { return nil }
    let asset = AVURLAsset(url: url)
    let read = readFunction(file, link: link)
    let loader = ClipLoader(size: file.size, ext: ext) { start, length, cancel, sink in
      read(start, length, cancel, sink)
    }
    asset.resourceLoader.setDelegate(loader, queue: Self.loaderQueue)
    return (asset, loader)
  }

  static let loaderQueue = DispatchQueue(label: "mediaviewer.files.avloader")
}

/// mv_file_index: the folder's names and fields, matched by the core.
final class FileIndex: @unchecked Sendable {
  private let raw: OpaquePointer?

  init(names: [String]) {
    raw = mv_file_index_new()
    guard let raw else { return }
    withCStrings(names) { _ = mv_file_index_names(raw, $0.baseAddress, $0.count) }
  }

  deinit { mv_file_index_free(raw) }

  func setFields(_ i: Int, _ fields: [String]) {
    guard let raw else { return }
    withCStrings(fields) { _ = mv_file_index_fields(raw, i, $0.baseAddress, $0.count) }
  }

  /// Positions in the listing with their why (nil: a name match), names first.
  func match(_ typed: String) -> [(Int, String?)] {
    guard let raw else { return [] }
    final class Hits { var list: [(Int, String?)] = [] }
    let hits = Hits()
    _ = typed.withCString { text in
      mv_file_index_match(raw, text, { ctx, i, why in
        guard let ctx else { return }
        Unmanaged<Hits>.fromOpaque(ctx).takeUnretainedValue().list.append((i, why.map { String(cString: $0) }))
      }, Unmanaged.passUnretained(hits).toOpaque())
    }
    return hits.list
  }

  private func withCStrings<R>(_ strings: [String], _ body: (UnsafeBufferPointer<UnsafePointer<CChar>?>) -> R) -> R {
    let copies = strings.map { strdup($0) }
    defer { copies.forEach { free($0) } }
    let pointers = copies.map { UnsafePointer($0) }
    return pointers.withUnsafeBufferPointer(body)
  }
}

/// One head read per file however many ask (the tag pass and a tile at once), and the last few
/// heads kept, so a tile and the pass reaching the same file a moment apart read it once. The read
/// runs in the asking task, so cancelling that task stops it; anyone waiting on it then reads for
/// itself.
private actor HeadReads {
  private var recent: [String: Data] = [:]
  private var order: [String] = []
  private var reading: Set<String> = []
  private var waiting: [String: [CheckedContinuation<Data?, Never>]] = [:]
  static let kept = 16

  func head(of file: FileEntry, link: LinkHandle) async -> Data? {
    let key = file.stamp
    if let data = recent[key] {
      order.removeAll { $0 == key }
      order.append(key)
      return data
    }
    if reading.contains(key) {
      let data = await withCheckedContinuation { waiting[key, default: []].append($0) }
      if data != nil || Task.isCancelled { return data }
      return await Self.read(file, link: link)  // the reader was cancelled or failed: once, ourselves
    }
    reading.insert(key)
    let data = await Self.read(file, link: link)
    reading.remove(key)
    if let data {
      recent[key] = data
      order.append(key)
      while order.count > Self.kept { recent[order.removeFirst()] = nil }
    }
    for waiter in waiting.removeValue(forKey: key) ?? [] { waiter.resume(returning: data) }
    return data
  }

  private static func read(_ file: FileEntry, link: LinkHandle) async -> Data? {
    await withLinkCancel { cancel in
      await onLinkQueue {
        final class Sink { var data = Data() }
        let sink = Sink()
        let want = min(UInt64(FileTags.headBytes), file.size)
        sink.data.reserveCapacity(Int(want))
        let status = file.path.root.withCString { root in
          file.path.rel.withCString { rel in
            mv_files_read(link.raw, root, rel, 0, want, 0, cancel.raw, { ctx, _, bytes, count in
              guard let ctx, let bytes else { return 0 }
              Unmanaged<Sink>.fromOpaque(ctx).takeUnretainedValue().data.append(bytes, count: count)
              return 1
            }, Unmanaged.passUnretained(sink).toOpaque())
          }
        }
        return status == 0 ? sink.data : nil
      }
    }
  }
}

/// At most `limit` of a kind of work at once (clip posters, whole-file reads for pictures).
final class OneAtATimeGate: @unchecked Sendable {
  private let limit: Int
  private var busy = 0
  private var waiting: [CheckedContinuation<Void, Never>] = []
  private let lock = NSLock()

  init(limit: Int) { self.limit = limit }

  func run<T>(_ body: () async -> T) async -> T {
    await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
      let go = lock.withLock { () -> Bool in
        if busy < limit {
          busy += 1
          return true
        }
        waiting.append(c)
        return false
      }
      if go { c.resume() }
    }
    defer {
      let next = lock.withLock { () -> CheckedContinuation<Void, Never>? in
        if waiting.isEmpty {
          busy -= 1
          return nil
        }
        return waiting.removeFirst()
      }
      next?.resume()
    }
    return await body()
  }
}

// MARK: The library and its search, in file mode

/// The named folders as a library: folders and the files in the one on screen, never a walk.
public struct FileModeLibrary: LibraryStore {
  let files: LinkFiles

  public func list(in scope: LibraryScope) async throws -> any MediaList {
    switch scope {
    case .folder(let folder):
      guard let path = FilePath(id: folder.id) else { return ArrayMediaList([]) }
      let listing = try await files.list(path)
      let list = ArrayMediaList(listing.files.map(\.item))
      try Task.checkCancellation()
      files.show(path, name: folder.name, files: listing.files)
      return list
    case .everything, .person:
      try Task.checkCancellation()
      files.show(nil, name: "", files: [])
      return ArrayMediaList([])
    }
  }

  public func folders(under parent: Folder?) async throws -> [Folder] {
    guard let parent else { return try await files.roots() }
    guard let path = FilePath(id: parent.id) else { return [] }
    return try await files.list(path).folders
  }

  public func people() async throws -> [Person] { [] }

  public func details(for item: MediaItem) async throws -> MediaDetails {
    let entry = files.entry(for: item.id)
    let facts = files.facts(for: item)
    let folder = entry?.path.parent.map { $0.rel } ?? ""
    return MediaDetails(place: nil, camera: facts.flatMap { $0.camera.isEmpty ? nil : $0.camera },
                        lens: facts.flatMap { $0.lens.isEmpty ? nil : $0.lens }, exposure: nil, folderPath: folder,
                        formatName: LinkImages.formatName(of: item.fileName), fileBytes: Int64(entry?.size ?? 0))
  }
}

/// The desktop's File search over the folder on screen (docs/design/16 "File search").
public struct FileModeSearch: SearchService {
  let files: LinkFiles

  public func capability() async -> SearchCapability { .filesOnly }
  public func recents() async -> [String] { [] }
  public func remember(_ query: String) async {}
  public func forget(_ query: String) async {}
  public func forgetAll() async {}
  public func filterOptions() async -> SearchFilterOptions { .none }
  public func isWarmingUp() async -> Bool { false }

  public func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] {
    try await answer(query, filters: filters).hits
  }

  public func answer(_ query: String, filters: SearchFilters) async throws -> SearchAnswer {
    guard let shown = files.shownFolder, shown.folder != nil else {
      return SearchAnswer(hits: [], outOf: 0, folderName: nil)
    }
    let typed = query.trimmingCharacters(in: .whitespaces)
    var hits: [SearchHit]
    if typed.isEmpty {
      hits = shown.files.map { SearchHit(item: $0.item, moment: nil) }
    } else {
      hits = shown.index.match(typed).compactMap { i, why in
        shown.files.indices.contains(i) ? SearchHit(item: shown.files[i].item, moment: nil, why: why) : nil
      }
    }
    if let kind = filters.kind { hits = hits.filter { $0.item.kind == kind } }
    try Task.checkCancellation()
    return SearchAnswer(hits: hits, outOf: shown.files.count, folderName: shown.name, readingTags: shown.progress)
  }
}
