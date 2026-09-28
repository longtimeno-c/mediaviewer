// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Photos library in the chrome (issue #72, plan/17 "Photos library
// source"): asking for access (only ever from a click), result tiles from
// PhotoKit's own thumbnail cache, and opening a result in the viewer.
//
// The pack indexes the library; this file is everything the chrome itself
// asks of PhotoKit. Every request here has network access off: nothing is
// downloaded from iCloud, and nothing in the library is ever changed (rule 5).
//
// Opening (owner, 2026-09-28: "without writing"). The viewer reads each
// result where Photos keeps it, read-only: the host refuses every write,
// rename, move and Trash for a file inside a Photos library bundle, and for the
// files registered with the list (shell/write_guard.h). An original only
// iCloud has (Optimize Mac Storage: most of them) opens as Photos' best local
// picture, "<name> (preview).jpg" in the cache folder; the original is fetched
// from iCloud only when that item is actually viewed (the owner: "when viewing
// but cleared after"), into the same folder, which is emptied when the next
// list opens and when the chrome attaches. The index never downloads.
import AppKit
import AVFoundation
import ImageIO
import Photos
import UniformTypeIdentifiers

enum PhotosLibrary {
  /// The pack's key for the library root and the prefix of its assets' keys.
  static let rootKey = "photos:"

  static func isKey(_ s: String) -> Bool { s.hasPrefix(rootKey) }
  static func identifier(_ key: String) -> String { String(key.dropFirst(rootKey.count)) }

  /// The app says why it asks (NSPhotoLibraryUsageDescription). An app from
  /// before issue #72 does not, and asking would end the process: the Photos
  /// row then says to update instead of offering the button.
  static var declared: Bool {
    Bundle.main.object(forInfoDictionaryKey: "NSPhotoLibraryUsageDescription") != nil
  }

  static var status: PHAuthorizationStatus { PHPhotoLibrary.authorizationStatus(for: .readWrite) }
  static var readable: Bool { status == .authorized || status == .limited }

  /// The system prompt, once, from a click. Later answers come from System
  /// Settings; this never asks twice.
  @MainActor static func requestAccess() async -> PHAuthorizationStatus {
    guard declared else { return .denied }
    guard status == .notDetermined else { return status }
    return await PHPhotoLibrary.requestAuthorization(for: .readWrite)
  }

  static func openPrivacySettings() {
    if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Photos") {
      NSWorkspace.shared.open(url)
    }
  }

  private static func asset(_ key: String) -> PHAsset? {
    guard readable, isKey(key) else { return nil }
    return PHAsset.fetchAssets(withLocalIdentifiers: [identifier(key)], options: nil).firstObject
  }

  // MARK: tiles

  /// A result tile or a face's picture: PhotoKit's own cached rendition (the
  /// pack keeps no second thumbnail cache of the library). [worker-thread]
  static func image(key: String, maxPixel: Int) -> CGImage? {
    guard let a = asset(key) else { return nil }
    let o = PHImageRequestOptions()
    o.isSynchronous = true
    o.isNetworkAccessAllowed = false
    o.deliveryMode = .highQualityFormat
    o.resizeMode = .fast
    o.version = .current
    var out: CGImage?
    PHImageManager.default().requestImage(for: a, targetSize: CGSize(width: maxPixel, height: maxPixel),
                                          contentMode: .aspectFit, options: o) { image, _ in
      out = image?.cgImage(forProposedRect: nil, context: nil, hints: nil)
    }
    return out
  }

  // MARK: opening in the viewer

  static var openFolder: URL {
    FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("MediaViewer", isDirectory: true)
      .appendingPathComponent("Photos Library", isDirectory: true)
  }

  /// Off the main thread: it deletes files (previews, on-view downloads).
  static func clearOpened() {
    let dir = openFolder
    DispatchQueue.global(qos: .utility).async { try? FileManager.default.removeItem(at: dir) }
  }

  /// The on-view iCloud downloads only: when the next list opens and at quit
  /// (the owner: "cleared after"). Unlinking is quick even for a clip, and a
  /// file the viewer still has open stays readable until it lets go.
  static func clearDownloads() {
    try? FileManager.default.removeItem(at: openFolder.appendingPathComponent("icloud", isDirectory: true))
  }

  /// What the viewer opens for an asset: where Photos keeps it (read-only),
  /// or a preview in the cache when only iCloud has the original.
  struct ViewerFile: Sendable {
    let path: String
    let preview: Bool
  }

  /// One per key, in the keys' order; nil where the asset is gone. Four at a
  /// time. `progress` hears the count done. [worker-thread]
  static func viewerFiles(for keys: [String], progress: @escaping @Sendable (Int) -> Void) async -> [ViewerFile?] {
    var out = [ViewerFile?](repeating: nil, count: keys.count)
    await withTaskGroup(of: (Int, ViewerFile?).self) { group in
      var next = 0
      var done = 0
      func add() {
        guard next < keys.count else { return }
        let i = next
        next += 1
        group.addTask { (i, await viewerFile(for: keys[i])) }
      }
      for _ in 0..<4 { add() }
      for await (i, file) in group {
        out[i] = file
        done += 1
        progress(done)
        add()
      }
    }
    return out
  }

  static func viewerFile(for key: String) async -> ViewerFile? {
    guard let a = asset(key) else { return nil }
    // The current rendition (the original, or Photos' render of an edit), in place.
    let local: URL? = a.mediaType == .video ? await videoURL(a) : await imageURL(a)
    if let local, local.isFileURL { return ViewerFile(path: local.path, preview: false) }
    // Only in iCloud: the best picture this Mac has, said to be a preview.
    let dir = folder(for: a, key: key, kind: "preview")
    let fm = FileManager.default
    let dst = dir.appendingPathComponent("\(baseName(a)) (preview)").appendingPathExtension("jpg")
    if fm.fileExists(atPath: dst.path) { return ViewerFile(path: dst.path, preview: true) }
    try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
    guard let cg = bestLocalImage(a),
          let out = CGImageDestinationCreateWithURL(dst as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else {
      return nil
    }
    CGImageDestinationAddImage(out, cg, [kCGImageDestinationLossyCompressionQuality: 0.92] as CFDictionary)
    return CGImageDestinationFinalize(out) ? ViewerFile(path: dst.path, preview: true) : nil
  }

  /// The original from iCloud, for the item being viewed: the one network
  /// request this chrome makes, on the user's own viewing, into the cache
  /// folder (cleared after). nil when it cannot be had (offline, cancelled).
  static func downloadOriginal(for key: String) async -> String? {
    guard let a = asset(key) else { return nil }
    let resources = PHAssetResource.assetResources(for: a)
    let order: [PHAssetResourceType] = a.mediaType == .video
      ? [.fullSizeVideo, .video] : [.fullSizePhoto, .photo]
    guard let res = order.lazy.compactMap({ t in resources.first { $0.type == t } }).first else { return nil }
    let dir = folder(for: a, key: key, kind: "icloud")
    let dst = dir.appendingPathComponent(res.originalFilename)
    let fm = FileManager.default
    if fm.fileExists(atPath: dst.path) { return dst.path }
    try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
    let part = dir.appendingPathComponent(".download")
    try? fm.removeItem(at: part)
    let o = PHAssetResourceRequestOptions()
    o.isNetworkAccessAllowed = true
    let ok = await withCheckedContinuation { (c: CheckedContinuation<Bool, Never>) in
      PHAssetResourceManager.default().writeData(for: res, toFile: part, options: o) { error in
        c.resume(returning: error == nil)
      }
    }
    guard ok, !Task.isCancelled, (try? fm.moveItem(at: part, to: dst)) != nil else {
      try? fm.removeItem(at: part)
      return nil
    }
    return dst.path
  }

  /// One folder per asset version (an edit in Photos makes a new one).
  private static func folder(for a: PHAsset, key: String, kind: String) -> URL {
    let stamp = Int64((a.modificationDate ?? a.creationDate ?? Date.distantPast).timeIntervalSince1970)
    let name = identifier(key).replacingOccurrences(of: "/", with: "_") + "-\(stamp)"
    return openFolder.appendingPathComponent(kind, isDirectory: true).appendingPathComponent(name, isDirectory: true)
  }

  private static func baseName(_ a: PHAsset) -> String {
    let resources = PHAssetResource.assetResources(for: a)
    let primary = resources.first { [.photo, .video, .fullSizePhoto, .fullSizeVideo].contains($0.type) } ?? resources.first
    return ((primary?.originalFilename ?? "Photo") as NSString).deletingPathExtension
  }

  /// The current rendition's file when it is on this Mac.
  private static func imageURL(_ a: PHAsset) async -> URL? {
    await withCheckedContinuation { (c: CheckedContinuation<URL?, Never>) in
      let o = PHContentEditingInputRequestOptions()
      o.isNetworkAccessAllowed = false
      a.requestContentEditingInput(with: o) { input, _ in c.resume(returning: input?.fullSizeImageURL) }
    }
  }

  /// Current first (a trimmed or filtered clip as Photos plays it); an edited
  /// clip's Current can be a composition with no file, so then the original,
  /// as the pack's video_file does. Both in place, never the network.
  private static func videoURL(_ a: PHAsset) async -> URL? {
    for version: PHVideoRequestOptionsVersion in [.current, .original] {
      let url: URL? = await withCheckedContinuation { (c: CheckedContinuation<URL?, Never>) in
        let o = PHVideoRequestOptions()
        o.isNetworkAccessAllowed = false
        o.version = version
        o.deliveryMode = .highQualityFormat
        PHImageManager.default().requestAVAsset(forVideo: a, options: o) { av, _, _ in
          c.resume(returning: (av as? AVURLAsset)?.url)
        }
      }
      if let url, url.isFileURL { return url }
    }
    return nil
  }

  /// The largest picture of an asset this Mac has, without the network. A
  /// synchronous request is answered in full quality or not at all, and the
  /// full size of an iCloud-only original is not here; so the sizes are asked
  /// in turn, largest first, and the first one PhotoKit can serve from what it
  /// keeps locally (Optimize Mac Storage keeps a screen-sized rendition) wins.
  /// A refused size costs a lookup, not a decode.
  private static func bestLocalImage(_ a: PHAsset) -> CGImage? {
    let o = PHImageRequestOptions()
    o.isSynchronous = true
    o.isNetworkAccessAllowed = false
    o.deliveryMode = .highQualityFormat
    o.resizeMode = .exact
    o.version = .current
    let sizes: [CGSize] = [PHImageManagerMaximumSize] + [4096, 2048, 1024, 512].map { CGSize(width: $0, height: $0) }
    for size in sizes {
      var out: CGImage?
      PHImageManager.default().requestImage(for: a, targetSize: size, contentMode: .aspectFit, options: o) { image, _ in
        out = image?.cgImage(forProposedRect: nil, context: nil, hints: nil)
      }
      if let out { return out }
    }
    return nil
  }
}

/// Which viewer file stands for which library asset, so the scrub markers,
/// N / Shift+N and Find Similar on an opened Photos result ask the pack about
/// the asset, not about the copy.
@MainActor
final class PhotosOpened {
  static let shared = PhotosOpened()
  private var keyOf: [String: String] = [:]

  private var previews: Set<String> = []

  func remember(file: String, key: String, preview: Bool = false) {
    keyOf[file] = key
    if preview { previews.insert(file) }
  }
  /// A cache preview of an iCloud-only original (its original is fetched on view).
  func isPreview(_ path: String) -> Bool { previews.contains(path) }
  /// The pack's path for what the viewer shows: a library key, or the path itself.
  func key(for path: String) -> String { keyOf[path] ?? path }
  func forget() {
    keyOf.removeAll()
    previews.removeAll()
  }
}
