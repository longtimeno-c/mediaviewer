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
import UIKit
import UniformTypeIdentifiers

public final class LinkImages: ImageLadder, @unchecked Sendable {
  private let connection: LinkConnection
  private let memory = NSCache<NSNumber, UIImage>()
  private let tiles = NSCache<NSNumber, UIImage>()
  private let decodeQueue = DispatchQueue(label: "mediaviewer.link.decode", qos: .userInitiated, attributes: .concurrent)

  public init(connection: LinkConnection) {
    self.connection = connection
    memory.totalCostLimit = 256 << 20
    tiles.countLimit = 8000
  }

  private func assetID(_ id: MediaID) -> Int64? { Int64(id.raw) }

  public func cachedImage(for id: MediaID) -> UIImage? {
    guard let asset = assetID(id) else { return nil }
    let key = NSNumber(value: asset)
    if let image = memory.object(forKey: key) ?? tiles.object(forKey: key) { return image }
    guard let link = connection.link else { return nil }
    let box = BytesBox()
    let status = mv_tiny_get(link.raw, asset, { ctx, data, size in
      guard let ctx, let data else { return }
      Unmanaged<BytesBox>.fromOpaque(ctx).takeUnretainedValue().data = Data(bytes: data, count: size)
    }, Unmanaged.passUnretained(box).toOpaque())
    guard status == 0, let data = box.data, let image = UIImage(data: data) else { return nil }
    tiles.setObject(image, forKey: key)
    return image
  }

  public func thumbnail(for item: MediaItem) async -> UIImage? {
    guard let asset = assetID(item.id) else { return nil }
    let key = NSNumber(value: asset)
    if let image = memory.object(forKey: key) { return image }
    guard let bytes = await requestThumb(asset: asset, lane: 0), let image = await decode(bytes, maxPixels: 512) else {
      return cachedImage(for: item.id)
    }
    memory.setObject(image, forKey: key, cost: Int(image.size.width * image.size.height * image.scale * image.scale * 4))
    return image
  }

  public func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    for item in items {
      guard let asset = assetID(item.id), memory.object(forKey: NSNumber(value: asset)) == nil else { continue }
      Task.detached(priority: .utility) { [self] in _ = await requestThumb(asset: asset, lane: 1) }
    }
  }

  private func requestThumb(asset: Int64, lane: Int32) async -> Data? {
    guard let link = connection.link else { return nil }
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
    guard let asset = assetID(item.id) else { throw MediaFailure.originalGone }
    if let tile = cachedImage(for: item.id) { continuation.yield(.image(tile, isOriginal: false)) }
    if let thumb = await thumbnail(for: item) { continuation.yield(.image(thumb, isOriginal: false)) }
    try Task.checkCancellation()
    guard let link = connection.link else { throw MediaFailure.synologyUnreachable }

    var info = mv_asset_info()
    guard mv_asset_read(link.raw, asset, &info) == 0 else { throw MediaFailure.originalGone }
    let total = Int64(info.size)
    let target = LinkFolders.originals.appendingPathComponent("\(asset)-\(info.size)")
    if !FileManager.default.fileExists(atPath: target.path) {
      continuation.yield(.loading(doneBytes: 0, totalBytes: total))
      let status = await readOriginal(link: link, asset: asset, size: info.size, to: target) { done in
        continuation.yield(.loading(doneBytes: done, totalBytes: total))
      }
      switch status {
      case 0: break
      case 11: throw CancellationError()
      case 9: throw MediaFailure.originalGone
      default: throw MediaFailure.synologyUnreachable
      }
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
    Self.trimOriginals()
  }

  private func readOriginal(link: LinkHandle, asset: Int64, size: UInt64, to target: URL, progress: @escaping (Int64) -> Void) async -> Int32 {
    await withLinkCancel { cancel in
      await onLinkQueue {
        let part = target.appendingPathExtension("part")
        guard FileManager.default.createFile(atPath: part.path, contents: nil), let file = try? FileHandle(forWritingTo: part) else { return 13 }
        defer { try? file.close() }
        let writer = FileWriter(file: file, progress: progress)
        let box = Unmanaged.passRetained(writer)
        let status = mv_file_read(link.raw, asset, 0, size, 1, cancel.raw, { ctx, offset, data, length in
          guard let ctx, let data else { return 0 }
          return Unmanaged<FileWriter>.fromOpaque(ctx).takeUnretainedValue().write(Data(bytes: data, count: length)) ? 1 : 0
        }, box.toOpaque())
        box.release()
        if status == 0 { try? FileManager.default.moveItem(at: part, to: target) } else { try? FileManager.default.removeItem(at: part) }
        return status
      }
    }
  }

  private static func trimOriginals() {
    let fm = FileManager.default
    guard let names = try? fm.contentsOfDirectory(at: LinkFolders.originals, includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey]) else { return }
    var total = 0
    var entries: [(URL, Date, Int)] = []
    for url in names {
      guard let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey]), let date = values.contentModificationDate else { continue }
      let size = values.fileSize ?? 0
      total += size
      entries.append((url, date, size))
    }
    entries.sort { $0.1 > $1.1 }
    var allowed = 1 << 30
    for entry in entries where total > allowed {
      if allowed > 0 { allowed = 1 << 30 }
      try? fm.removeItem(at: entry.0)
      total -= entry.2
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
