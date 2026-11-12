// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §10: the first pixel is never the full decode. Memory, then the 64 px tile
// from tiny.pack (always here), then JPEG-512 by a merged range read, then the original through
// ImageIO. The step that decodes never runs on the main thread except the tiny tile, which is
// what makes first paint one frame.
import CMobileLink
import Foundation
import ImageIO
import MobileKit
import MobileLinkSupport
import UIKit
import UniformTypeIdentifiers

public final class LinkImages: ImageLadder, @unchecked Sendable {
  private let connection: LinkConnection
  private let memory = ScopedCache<UIImage>(totalCostLimit: 256 << 20)
  private let tiles = ScopedCache<UIImage>(countLimit: 8000)
  private let decodeQueue = DispatchQueue(label: "mediaviewer.link.decode", qos: .userInitiated, attributes: .concurrent)
  private let originals = OriginalStore(dir: LinkFolders.originals, limitBytes: 1 << 30)
  /// File mode's pictures and originals (a library with no catalogue yet), by path.
  public let files: LinkFiles

  public init(connection: LinkConnection) {
    self.connection = connection
    files = LinkFiles(connection: connection, originals: originals)
    // Forget, or the library gone: nothing of it stays in memory.
    Task.detached(priority: .utility) { [memory, tiles] in
      for await snapshot in connection.updates() where !snapshot.hasLibrary {
        memory.removeAll()
        tiles.removeAll()
      }
    }
  }

  private func assetID(_ id: MediaID) -> Int64? { Int64(id.raw) }

  /// The JPEG-512 if it is in memory, and nothing slower: what a grid tile shown before paints in
  /// its first frame.
  public func memoryImage(for id: MediaID) -> UIImage? {
    if FilePath.isFile(id.raw) { return files.memoryImage(for: id) }
    return memory.object(library: connection.libraryID, generation: id.catalogue, id: id.raw)
  }

  public func cachedImage(for id: MediaID) -> UIImage? {
    if FilePath.isFile(id.raw) { return files.memoryImage(for: id) }
    guard let asset = assetID(id) else { return nil }
    let library = connection.libraryID
    if let image = memory.object(library: library, generation: id.catalogue, id: id.raw)
      ?? tiles.object(library: library, generation: id.catalogue, id: id.raw) { return image }
    guard let link = connection.link else { return nil }
    let box = BytesBox()
    let status = mv_tiny_get(link.raw, asset, { ctx, data, size in
      guard let ctx, let data else { return }
      Unmanaged<BytesBox>.fromOpaque(ctx).takeUnretainedValue().data = Data(bytes: data, count: size)
    }, Unmanaged.passUnretained(box).toOpaque())
    guard status == 0, let data = box.data, let image = UIImage(data: data) else { return nil }
    tiles.set(image, library: library, generation: id.catalogue, id: id.raw)
    return image
  }

  public func thumbnail(for item: MediaItem) async -> UIImage? {
    if FilePath.isFile(item.id.raw) { return await files.thumbnail(for: item) }
    guard let asset = assetID(item.id) else { return nil }
    let library = connection.libraryID
    if let image = memory.object(library: library, generation: item.id.catalogue, id: item.id.raw) { return image }
    guard let bytes = await requestThumb(asset: asset, lane: 0), let image = await decode(bytes, maxPixels: 512) else {
      return cachedImage(for: item.id)
    }
    memory.set(image, library: library, generation: item.id.catalogue, id: item.id.raw,
               cost: Int(image.size.width * image.size.height * image.scale * image.scale * 4))
    return image
  }

  public func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    for item in items {
      if FilePath.isFile(item.id.raw) {
        Task.detached(priority: .utility) { [files] in _ = await files.thumbnail(for: item) }
        continue
      }
      guard let asset = assetID(item.id),
            memory.object(library: connection.libraryID, generation: item.id.catalogue, id: item.id.raw) == nil else { continue }
      Task.detached(priority: .utility) { [self] in _ = await requestThumb(asset: asset, lane: 1) }
    }
  }

  private func requestThumb(asset: Int64, lane: Int32) async -> Data? {
    guard let link = await connection.awaitLink() else { return nil }
    let box = ContinuationBox()
    let handle = CancelHandle()
    return await withTaskCancellationHandler {
      await withCheckedContinuation { (continuation: CheckedContinuation<Data?, Never>) in
        box.set(continuation)
        var token: OpaquePointer?
        let status = mv_thumb_request(link.raw, asset, lane, { ctx, status, data, size in
          guard let ctx else { return }
          let out = Unmanaged<ContinuationBox>.fromOpaque(ctx).takeRetainedValue()
          out.finish(status == 0 && data != nil ? Data(bytes: data!, count: size) : nil)
        }, Unmanaged.passRetained(box).toOpaque(), &token)
        handle.adopt(token)
        if status != 0 { Unmanaged<ContinuationBox>.passUnretained(box).release(); box.finish(nil) }
      }
    } onCancel: { handle.fire() }
  }

  private func decode(_ data: Data, maxPixels: Int) async -> UIImage? {
    await withCheckedContinuation { continuation in
      decodeQueue.async { continuation.resume(returning: Self.fitted(data as CFData, maxPixels: maxPixels)) }
    }
  }

  static func fitted(_ data: CFData, maxPixels: Int) -> UIImage? {
    guard let source = CGImageSourceCreateWithData(data, nil) else { return nil }
    return image(from: source, maxPixels: maxPixels)
  }

  static func image(from source: CGImageSource, maxPixels: Int) -> UIImage? {
    let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                    kCGImageSourceThumbnailMaxPixelSize: maxPixels,
                                    kCGImageSourceCreateThumbnailWithTransform: true,
                                    kCGImageSourceShouldCacheImmediately: true]
    guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
    return UIImage(cgImage: cg)
  }

  public func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          try await runLadder(item: item, fittingPixels: fittingPixels, continuation: continuation)
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  private func runLadder(item: MediaItem, fittingPixels: CGSize, continuation: AsyncThrowingStream<LadderEvent, Error>.Continuation) async throws {
    if FilePath.isFile(item.id.raw) {
      try await runFileLadder(item: item, fittingPixels: fittingPixels, continuation: continuation)
      return
    }
    guard let asset = assetID(item.id) else { throw MediaFailure.originalGone }
    if let tile = cachedImage(for: item.id) { continuation.yield(.image(tile, isOriginal: false)) }
    if let thumb = await thumbnail(for: item) { continuation.yield(.image(thumb, isOriginal: false)) }
    try Task.checkCancellation()
    guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }

    let target = try await originalFile(link: link, asset: asset) { done, total in
      continuation.yield(.loading(doneBytes: done, totalBytes: total))
    }
    let longest = Int(max(fittingPixels.width, fittingPixels.height))
    let decoded = await withCheckedContinuation { (out: CheckedContinuation<UIImage?, Never>) in
      decodeQueue.async {
        guard let source = CGImageSourceCreateWithURL(target as CFURL, nil) else { out.resume(returning: nil); return }
        out.resume(returning: Self.image(from: source, maxPixels: longest))
      }
    }
    guard let decoded else { throw MediaFailure.cannotDecode(format: Self.formatName(of: item.fileName)) }
    continuation.yield(.image(decoded, isOriginal: true))
  }

  /// File mode: the grid's picture, then the original over the link, as for a catalogue item.
  private func runFileLadder(item: MediaItem, fittingPixels: CGSize,
                             continuation: AsyncThrowingStream<LadderEvent, Error>.Continuation) async throws {
    if let thumb = await files.thumbnail(for: item) { continuation.yield(.image(thumb, isOriginal: false)) }
    try Task.checkCancellation()
    guard let entry = files.entry(for: item.id) else { throw MediaFailure.originalGone }
    let target = try await files.originalFile(entry) { done, total in
      continuation.yield(.loading(doneBytes: done, totalBytes: total))
    }
    let longest = Int(max(fittingPixels.width, fittingPixels.height))
    let decoded = await withCheckedContinuation { (out: CheckedContinuation<UIImage?, Never>) in
      decodeQueue.async {
        guard let source = CGImageSourceCreateWithURL(target as CFURL, nil) else { out.resume(returning: nil); return }
        out.resume(returning: Self.image(from: source, maxPixels: longest))
      }
    }
    guard let decoded else { throw MediaFailure.cannotDecode(format: Self.formatName(of: item.fileName)) }
    continuation.yield(.image(decoded, isOriginal: true))
  }

  /// The whole original on the phone, from the store when it is already here. One fetch per
  /// original however many ask; its own part file; checked for its size.
  private func originalFile(link: LinkHandle, asset: Int64,
                            progress: @escaping (Int64, Int64) -> Void) async throws -> URL {
    var info = mv_asset_info()
    guard mv_asset_read(link.raw, asset, &info) == 0 else { throw MediaFailure.originalGone }
    let total = Int64(info.size)
    let size = info.size
    progress(0, total)
    do {
      // The library in the name too: after Forget and another pairing, an id can come back.
      let library = String(connection.libraryID.unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) })
      let connection = self.connection
      return try await originals.file(key: "\(library)-\(asset)-\(size)", size: size,
                                      place: { move in try connection.whileLive(link) { try move() } != nil }) { part in
        let status = await Self.readOriginal(connection: connection, link: link, asset: asset, size: size, to: part) {
          done in progress(done, total)
        }
        switch status {
        case 0: return
        case 11: throw CancellationError()
        case 9: throw MediaFailure.originalGone
        case 18: throw MediaFailure.folderNotOnNAS
        default: throw MediaFailure.synologyUnreachable
        }
      }
    } catch is OriginalStoreError {
      throw MediaFailure.synologyUnreachable  // cut short: the next look asks again
    }
  }

  /// The original as a file on the phone, for saving a copy to Photos.
  public func original(for item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          let progress: (Int64, Int64) -> Void = { done, total in
            continuation.yield(.progress(TransferProgress(doneBytes: done, totalBytes: total)))
          }
          let file: URL
          if FilePath.isFile(item.id.raw) {
            guard let entry = files.entry(for: item.id) else { throw MediaFailure.originalGone }
            file = try await files.originalFile(entry, progress: progress)
          } else {
            guard let asset = assetID(item.id) else { throw MediaFailure.originalGone }
            guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }
            file = try await originalFile(link: link, asset: asset, progress: progress)
          }
          continuation.yield(.finished(file))
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  /// Writes the whole original to `part`, which the store then checks and moves in.
  private static func readOriginal(connection: LinkConnection, link: LinkHandle, asset: Int64, size: UInt64, to part: URL,
                                   progress: @escaping (Int64) -> Void) async -> Int32 {
    await withLinkCancel { cancel in
      await onLinkQueue {
        // Created only while the library is still paired: Forget may be removing the folder.
        guard let made = connection.whileLive(link, { FileManager.default.createFile(atPath: part.path, contents: nil) }) else { return 11 }
        guard made, let file = try? FileHandle(forWritingTo: part) else { return 13 }
        defer { try? file.close() }
        let writer = FileWriter(file: file, progress: progress)
        let box = Unmanaged.passRetained(writer)
        let status = mv_file_read(link.raw, asset, 0, size, 1, cancel.raw, { ctx, _, data, length in
          guard let ctx, let data else { return 0 }
          return Unmanaged<FileWriter>.fromOpaque(ctx).takeUnretainedValue().write(Data(bytes: data, count: length)) ? 1 : 0
        }, box.toOpaque())
        box.release()
        return status
      }
    }
  }

  static func formatName(of file: String) -> String {
    let ext = (file as NSString).pathExtension.uppercased()
    return ext.isEmpty ? "This file" : ext
  }
}

private final class BytesBox { var data: Data? }

private final class ContinuationBox: @unchecked Sendable {
  private var continuation: CheckedContinuation<Data?, Never>?
  private let lock = NSLock()
  func set(_ c: CheckedContinuation<Data?, Never>) { lock.withLock { continuation = c } }
  func finish(_ data: Data?) {
    let c = lock.withLock { () -> CheckedContinuation<Data?, Never>? in
      defer { continuation = nil }
      return continuation
    }
    c?.resume(returning: data)
  }
}

private final class CancelHandle: @unchecked Sendable {
  private var token: OpaquePointer?
  private let lock = NSLock()
  private var fired = false

  func adopt(_ t: OpaquePointer?) {
    lock.withLock {
      token = t
      if fired, let t { mv_cancel_fire(t) }
    }
  }

  func fire() {
    lock.withLock {
      fired = true
      if let token { mv_cancel_fire(token) }
    }
  }

  deinit {
    if let token { mv_cancel_free(token) }
  }
}

private final class FileWriter {
  let file: FileHandle
  let progress: (Int64) -> Void
  private var written: Int64 = 0
  private var reported: Int64 = 0

  init(file: FileHandle, progress: @escaping (Int64) -> Void) {
    self.file = file
    self.progress = progress
  }

  func write(_ data: Data) -> Bool {
    do { try file.write(contentsOf: data) } catch { return false }
    written += Int64(data.count)
    if written - reported >= 256 * 1024 {
      reported = written
      progress(written)
    }
    return true
  }
}
