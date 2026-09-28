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
// Opening. A Photos asset has no path the viewer may have: the viewer rates,
// renames and moves files, and must never do that inside the Photos library.
// So each result opens as a file of its own in the cache folder:
//   - the original, when it is on this Mac: an APFS clone (clonefile), which
//     copies no bytes and which the library never sees change;
//   - otherwise (Optimize Mac Storage keeps most originals in iCloud: 174 of
//     200 on the owner's library, 2026-09-28) the best picture Photos keeps on
//     this Mac, saved as "<name> (preview).jpg" so the viewer says what it is.
// The folder is emptied when the chrome attaches and when it quits.
import AppKit
import AVFoundation
import Darwin
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

  /// Off the main thread: it deletes files.
  static func clearOpened() {
    let dir = openFolder
    DispatchQueue.global(qos: .utility).async { try? FileManager.default.removeItem(at: dir) }
  }

  /// A file for each key, in the keys' order; nil where the asset is gone.
  /// Four at a time. `progress` hears the count done. [worker-thread]
  static func files(for keys: [String], progress: @escaping @Sendable (Int) -> Void) async -> [String?] {
    var out = [String?](repeating: nil, count: keys.count)
    await withTaskGroup(of: (Int, String?).self) { group in
      var next = 0
      var done = 0
      func add() {
        guard next < keys.count else { return }
        let i = next
        next += 1
        group.addTask { (i, await file(for: keys[i])) }
      }
      for _ in 0..<4 { add() }
      for await (i, path) in group {
        out[i] = path
        done += 1
        progress(done)
        add()
      }
    }
    return out
  }

  /// The asset's file for the viewer, made once per version: the folder is
  /// named by the identifier and the modification time, so an edit in Photos
  /// makes a new one.
  static func file(for key: String) async -> String? {
    guard let a = asset(key) else { return nil }
    let stamp = Int64((a.modificationDate ?? a.creationDate ?? Date.distantPast).timeIntervalSince1970)
    let folderName = identifier(key).replacingOccurrences(of: "/", with: "_") + "-\(stamp)"
    let dir = openFolder.appendingPathComponent(folderName, isDirectory: true)
    let fm = FileManager.default
    if let existing = try? fm.contentsOfDirectory(atPath: dir.path).first(where: { !$0.hasPrefix(".") }) {
      return dir.appendingPathComponent(existing).path
    }
    try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
    let resources = PHAssetResource.assetResources(for: a)
    let primary = resources.first { [.photo, .video, .fullSizePhoto, .fullSizeVideo].contains($0.type) } ?? resources.first
    let base = ((primary?.originalFilename ?? "Photo") as NSString).deletingPathExtension

    let local: URL? = a.mediaType == .video ? await videoURL(a) : await imageURL(a)
    if let src = local {
      let dst = dir.appendingPathComponent(base).appendingPathExtension(src.pathExtension)
      if clonefile(src.path, dst.path, 0) == 0 || (try? fm.copyItem(at: src, to: dst)) != nil {
        return dst.path
      }
    }
    // Only in iCloud: the best picture this Mac has, said to be a preview.
    guard let cg = bestLocalImage(a) else { return nil }
    let dst = dir.appendingPathComponent("\(base) (preview)").appendingPathExtension("jpg")
    guard let out = CGImageDestinationCreateWithURL(dst as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else {
      return nil
    }
    CGImageDestinationAddImage(out, cg, [kCGImageDestinationLossyCompressionQuality: 0.92] as CFDictionary)
    return CGImageDestinationFinalize(out) ? dst.path : nil
  }

  /// The current rendition's file (the original, or Photos' render of an
  /// edit) when it is on this Mac.
  private static func imageURL(_ a: PHAsset) async -> URL? {
    await withCheckedContinuation { (c: CheckedContinuation<URL?, Never>) in
      let o = PHContentEditingInputRequestOptions()
      o.isNetworkAccessAllowed = false
      a.requestContentEditingInput(with: o) { input, _ in c.resume(returning: input?.fullSizeImageURL) }
    }
  }

  private static func videoURL(_ a: PHAsset) async -> URL? {
    await withCheckedContinuation { (c: CheckedContinuation<URL?, Never>) in
      let o = PHVideoRequestOptions()
      o.isNetworkAccessAllowed = false
      o.version = .current
      o.deliveryMode = .highQualityFormat
      PHImageManager.default().requestAVAsset(forVideo: a, options: o) { av, _, _ in
        c.resume(returning: (av as? AVURLAsset)?.url)
      }
    }
  }

  private static func bestLocalImage(_ a: PHAsset) -> CGImage? {
    let o = PHImageRequestOptions()
    o.isSynchronous = true
    o.isNetworkAccessAllowed = false
    o.deliveryMode = .highQualityFormat
    o.version = .current
    var out: CGImage?
    PHImageManager.default().requestImage(for: a, targetSize: PHImageManagerMaximumSize, contentMode: .default,
                                          options: o) { image, _ in
      out = image?.cgImage(forProposedRect: nil, context: nil, hints: nil)
    }
    return out
  }
}

/// Which viewer file stands for which library asset, so the scrub markers,
/// N / Shift+N and Find Similar on an opened Photos result ask the pack about
/// the asset, not about the copy.
@MainActor
final class PhotosOpened {
  static let shared = PhotosOpened()
  private var keyOf: [String: String] = [:]

  func remember(file: String, key: String) { keyOf[file] = key }
  /// The pack's path for what the viewer shows: a library key, or the path itself.
  func key(for path: String) -> String { keyOf[path] ?? path }
  func forget() { keyOf.removeAll() }
}
