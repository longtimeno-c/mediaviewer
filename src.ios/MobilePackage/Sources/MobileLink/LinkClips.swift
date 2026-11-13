// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §10: AVPlayer's byte-range requests become read_range calls on the link. A
// clip AVPlayer can open streams; a clip above what the link measures says so; MKV and friends get
// moments as stills and the file.
import AVFoundation
import CMobileLink
import Foundation
import MobileKit
import MobileLinkSupport
import UIKit

public final class LinkClips: NSObject, ClipService, @unchecked Sendable {
  private let connection: LinkConnection
  private let images: LinkImages
  private let loaderQueue = DispatchQueue(label: "mediaviewer.link.avloader")
  private static let scheme = "mvlink"
  private var loaders: [Int64: ClipLoader] = [:]
  /// File mode's clips by item id, the last few only (an AVURLAsset holds its loader weakly).
  private var fileLoaders: [String: ClipLoader] = [:]
  private var fileLoaderOrder: [String] = []
  static let fileLoadersKept = 4
  private let lock = NSLock()
  private var measured: (epoch: Int, mbps: Int)?  // per route: a network change measures again

  public init(connection: LinkConnection, images: LinkImages) {
    self.connection = connection
    self.images = images
    super.init()
    // Another library paired, or Forget: no loader of the last one's stays.
    Task.detached(priority: .utility) { [weak self] in
      var watch = LinkConnection.LibraryChange(current: connection.current.libraryID)
      for await snapshot in connection.updates() where watch.changed(to: snapshot.libraryID) || !snapshot.hasLibrary {
        guard let self else { return }
        self.lock.withLock {
          self.fileLoaders.removeAll()
          self.fileLoaderOrder.removeAll()
        }
      }
    }
  }

  public func route(for item: MediaItem) async -> ClipRoute {
    if FilePath.isFile(item.id.raw) { return await fileRoute(for: item) }
    guard let asset = Int64(item.id.raw), let link = await connection.awaitLink() else { return .unavailable(.synologyUnreachable) }
    var info = mv_asset_info()
    guard mv_asset_read(link.raw, asset, &info) == 0 else { return .unavailable(.originalGone) }
    // Before AVPlayer gets it: a player error could not say which folder is missing.
    if await onLinkQueue({ mv_asset_folder_check(link.raw, asset) }) == 18 { return .unavailable(.folderNotOnNAS) }
    let ext = (item.fileName as NSString).pathExtension.lowercased()
    guard ["mp4", "mov", "m4v"].contains(ext) else {
      return .notPlayableHere(containerName: ext.uppercased(), fileBytes: Int64(info.size))
    }
    // The sample library's clips are a few megabytes, played from a copy checked whole against its
    // signed hash: timing that download over a 4 MB probe would understate the line.
    if let duration = item.duration, duration > 0, !connection.current.isSample {
      let needed = Int(Double(info.size) * 8 / duration / 1_000_000) + 1
      let available = await throughput(link: link, asset: asset, size: info.size)
      if let available, needed > available { return .tooBigToStream(neededMbps: needed, availableMbps: available, fileBytes: Int64(info.size)) }
    }
    var parts = URLComponents()
    parts.scheme = Self.scheme
    parts.host = "asset"
    parts.path = "/\(asset).\(ext)"
    guard let url = parts.url else { return .unavailable(.originalGone) }
    let av = AVURLAsset(url: url)
    let loader = ClipLoader(link: link, asset: asset, size: info.size, ext: ext)
    lock.withLock { loaders[asset] = loader }
    av.resourceLoader.setDelegate(loader, queue: loaderQueue)
    return .stream(av)
  }

  /// File mode: the clip by its path. Played as it streams; there is no catalogue duration to
  /// weigh it against the link first.
  private func fileRoute(for item: MediaItem) async -> ClipRoute {
    guard let entry = images.files.entry(for: item.id) else { return .unavailable(.originalGone) }
    guard let link = await connection.awaitLink() else { return .unavailable(.synologyUnreachable) }
    let ext = (item.fileName as NSString).pathExtension.lowercased()
    guard ["mp4", "mov", "m4v"].contains(ext) else {
      return .notPlayableHere(containerName: ext.uppercased(), fileBytes: Int64(entry.size))
    }
    guard let (asset, loader) = images.files.clipAsset(entry, link: link) else { return .unavailable(.originalGone) }
    lock.withLock {
      fileLoaders[item.id.raw] = loader
      fileLoaderOrder.removeAll { $0 == item.id.raw }
      fileLoaderOrder.append(item.id.raw)
      while fileLoaderOrder.count > Self.fileLoadersKept { fileLoaders[fileLoaderOrder.removeFirst()] = nil }
    }
    return .stream(asset)
  }

  /// What a clip can count on: measured once per connection, from the file itself, on the first play.
  private func throughput(link: LinkHandle, asset: Int64, size: UInt64) async -> Int? {
    let epoch = connection.epoch
    if let known = lock.withLock({ measured }), known.epoch == epoch { return known.mbps }
    let result: Int? = await onLinkQueue {
      let probe = min(size, 4 << 20)
      var got: UInt64 = 0
      let started = CACurrentMediaTime()
      let status = withUnsafeMutablePointer(to: &got) { pointer in
        mv_file_read(link.raw, asset, 0, probe, 0, nil, { ctx, _, _, length in
          ctx?.assumingMemoryBound(to: UInt64.self).pointee += UInt64(length)
          return 1
        }, pointer)
      }
      guard status == 0, got > 256 * 1024 else { return nil }
      let seconds = max(CACurrentMediaTime() - started, 0.001)
      return Int(Double(got) * 8 / seconds / 1_000_000)
    }
    if let result { lock.withLock { measured = (epoch, result) } }
    return result
  }

  public func moments(for item: MediaItem) async -> [Moment] {
    guard !FilePath.isFile(item.id.raw) else { return [] }  // moments are the catalogue's
    guard let asset = Int64(item.id.raw), let link = await connection.awaitLink() else { return [] }
    final class Collector { var rows: [Moment] = [] }
    let collector = Collector()
    _ = mv_moments_get(link.raw, asset, { ctx, start, kind, text in
      guard let ctx, let text else { return }
      let list = Unmanaged<Collector>.fromOpaque(ctx).takeUnretainedValue()
      let mapped: Moment.Kind = kind == 2 ? .wordsOnScreen : kind == 1 ? .scene : .speech
      list.rows.append(Moment(seconds: TimeInterval(start) / 1000, kind: mapped, text: String(cString: text)))
    }, Unmanaged.passUnretained(collector).toOpaque())
    return collector.rows
  }

  public func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage? {
    guard case .stream(let asset) = await route(for: item) else { return await images.thumbnail(for: item) }
    let generator = AVAssetImageGenerator(asset: asset)
    generator.appliesPreferredTrackTransform = true
    generator.maximumSize = CGSize(width: 1024, height: 1024)
    let time = CMTime(seconds: seconds, preferredTimescale: 600)
    if let (cg, _) = try? await generator.image(at: time) { return UIImage(cgImage: cg) }
    return await images.thumbnail(for: item)
  }

  public func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          let url = try await runDownload(item: item) { done, total in
            continuation.yield(.progress(TransferProgress(doneBytes: done, totalBytes: total)))
          }
          continuation.yield(.finished(url))
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  private func runDownload(item: MediaItem, report: @escaping (Int64, Int64) -> Void) async throws -> URL {
    guard let link = await connection.awaitLink() else { throw MediaFailure.synologyUnreachable }
    let read: ClipRead
    let total: Int64
    if FilePath.isFile(item.id.raw) {
      guard let entry = images.files.entry(for: item.id) else { throw MediaFailure.originalGone }
      read = images.files.readFunction(entry, link: link)
      total = Int64(entry.size)
    } else {
      guard let asset = Int64(item.id.raw) else { throw MediaFailure.synologyUnreachable }
      var info = mv_asset_info()
      guard mv_asset_read(link.raw, asset, &info) == 0 else { throw MediaFailure.originalGone }
      read = { start, length, cancel, sink in
        let box = Unmanaged.passRetained(BlockBox(sink))
        defer { box.release() }
        return mv_file_read(link.raw, asset, start, length, 1, cancel.raw, { ctx, _, data, count in
          guard let ctx, let data else { return 0 }
          return Unmanaged<BlockBox>.fromOpaque(ctx).takeUnretainedValue().body(Data(bytes: data, count: count)) ? 1 : 0
        }, box.toOpaque())
      }
      total = Int64(info.size)
    }
    let target = LinkFolders.clips.appendingPathComponent(item.fileName)
    let connection = self.connection
    let status = await withLinkCancel { cancel in
      await onLinkQueue {
        // Created only while the library is still paired: Forget may be removing the folder.
        guard let made = connection.whileLive(link, { () -> Bool in
          try? FileManager.default.removeItem(at: target)
          return FileManager.default.createFile(atPath: target.path, contents: nil)
        }) else { return Int32(11) }
        guard made, let file = try? FileHandle(forWritingTo: target) else { return Int32(13) }
        defer { try? file.close() }
        var written: Int64 = 0
        return read(0, UInt64.max, cancel) { data -> Bool in
          do { try file.write(contentsOf: data) } catch { return false }
          written += Int64(data.count)
          report(written, total)
          return true
        }
      }
    }
    switch status {
    case 0: return target
    case 11: throw CancellationError()
    case 9: throw MediaFailure.originalGone
    case 18: throw MediaFailure.folderNotOnNAS
    default: throw MediaFailure.synologyUnreachable
    }
  }
}

final class BlockBox {
  let body: (Data) -> Bool
  init(_ body: @escaping (Data) -> Bool) { self.body = body }
}

/// Reads bytes [start, start + length) of one clip (length UInt64.max: to the end), handing each
/// chunk to `sink` (false stops it). Blocks; returns an mv_link_status.
typealias ClipRead = @Sendable (_ start: UInt64, _ length: UInt64, _ cancel: LinkCancel,
                                _ sink: @escaping (Data) -> Bool) -> Int32

/// One clip's AVAssetResourceLoader: every byte-range request becomes a cancellable read on the link.
final class ClipLoader: NSObject, AVAssetResourceLoaderDelegate, @unchecked Sendable {
  private let size: UInt64
  private let ext: String
  private let read: ClipRead
  private var running: [ObjectIdentifier: LinkCancel] = [:]
  private let lock = NSLock()

  /// A catalogue asset's original.
  convenience init(link: LinkHandle, asset: Int64, size: UInt64, ext: String) {
    self.init(size: size, ext: ext) { start, length, cancel, sink in
      let box = Unmanaged.passRetained(BlockBox(sink))
      defer { box.release() }
      return mv_file_read(link.raw, asset, start, length, 0, cancel.raw, { ctx, _, bytes, count in
        guard let ctx, let bytes else { return 0 }
        return Unmanaged<BlockBox>.fromOpaque(ctx).takeUnretainedValue().body(Data(bytes: bytes, count: count)) ? 1 : 0
      }, box.toOpaque())
    }
  }

  init(size: UInt64, ext: String, read: @escaping ClipRead) {
    self.size = size
    self.ext = ext
    self.read = read
  }

  func resourceLoader(_ resourceLoader: AVAssetResourceLoader, shouldWaitForLoadingOfRequestedResource request: AVAssetResourceLoadingRequest) -> Bool {
    if let info = request.contentInformationRequest {
      info.contentType = ext == "mov" ? "com.apple.quicktime-movie" : "public.mpeg-4"
      info.contentLength = Int64(size)
      info.isByteRangeAccessSupported = true
    }
    guard let data = request.dataRequest else {
      request.finishLoading()
      return true
    }
    let cancel = LinkCancel()
    let key = ObjectIdentifier(request)
    lock.withLock { running[key] = cancel }
    let start = UInt64(max(data.currentOffset, data.requestedOffset))
    let length = data.requestsAllDataToEndOfResource ? UInt64.max : UInt64(data.requestedLength) - (start - UInt64(data.requestedOffset))
    let queueCancel = cancel
    linkNetQueue.async { [self] in
      let response = ResponseBox(request: request, cancel: queueCancel)
      let status = read(start, length, queueCancel) { response.deliver($0) }
      lock.withLock { running[key] = nil }
      guard status != 11 else { return }  // AVPlayer cancelled it
      if status == 0 {
        request.finishLoading()
      } else {
        request.finishLoading(with: NSError(domain: "MediaViewerLink", code: Int(status)))
      }
    }
    return true
  }

  func resourceLoader(_ resourceLoader: AVAssetResourceLoader, didCancel request: AVAssetResourceLoadingRequest) {
    let key = ObjectIdentifier(request)
    let cancel = lock.withLock { running[key] }
    cancel?.fire()
  }
}

private final class ResponseBox {
  let request: AVAssetResourceLoadingRequest
  let cancel: LinkCancel
  init(request: AVAssetResourceLoadingRequest, cancel: LinkCancel) {
    self.request = request
    self.cancel = cancel
  }
  func deliver(_ data: Data) -> Bool {
    guard let dataRequest = request.dataRequest, !request.isCancelled, !request.isFinished else { return false }
    dataRequest.respond(with: data)
    return true
  }
}
