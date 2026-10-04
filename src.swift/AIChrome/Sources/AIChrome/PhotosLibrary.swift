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
// Opening a result is the host's (plan/26, shell/photos_items_mac.h): a
// result's path is its library key, the host lists it as a virtual item and
// resolves it to the file Photos keeps (read-only), or to a preview whose
// original is fetched after a short stay. The "added" flag below is how the
// host knows the library may be shown as a folder and backed up.
import AppKit
import Photos

enum PhotosLibrary {
  /// The pack's key for the library root and the prefix of its assets' keys.
  static let rootKey = "photos:"

  static func isKey(_ s: String) -> Bool { s.hasPrefix(rootKey) }
  static func identifier(_ key: String) -> String { String(key.dropFirst(rootKey.count)) }

  /// Settings added (or removed) the library: the host's folder row, menu
  /// item and backup follow this (shell/photos_items_mac.h kAddedDefault).
  static let addedKey = "mv.photosLibrary.added"
  static func setAdded(_ on: Bool) {
    if UserDefaults.standard.bool(forKey: addedKey) != on { UserDefaults.standard.set(on, forKey: addedKey) }
  }

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

}
