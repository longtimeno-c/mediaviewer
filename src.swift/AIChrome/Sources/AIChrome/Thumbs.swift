// SPDX-License-Identifier: GPL-2.0-or-later
// Result tiles and people covers, decoded off the main thread (rule 1) with
// ImageIO into a small in-memory LRU. Face crops are cut from the picture the
// pack names (face_thumb: the viewer's JPEG-512 of the image or moment the face
// was found in) in memory and never written anywhere (plan/17 PR 24 "never
// exported"; the chrome brief: "crop the cover thumbnail with cover_box").
import AppKit
import CAiApi
import CoreGraphics
import Foundation
import ImageIO

/// One tile's picture. Tiles observe their own slot, so an arriving thumbnail
/// re-renders one tile, not the grid (the lesson in FolderStore.swift).
@MainActor
final class ImageSlot: ObservableObject {
  @Published var image: CGImage?
  var requested = false
}

/// NSCache is the LRU: bounded by count, evicted under memory pressure.
final class ImageCache: @unchecked Sendable {
  static let shared = ImageCache()
  private let cache = NSCache<NSString, CGImageBox>()

  private init() { cache.countLimit = 400 }

  final class CGImageBox {
    let image: CGImage
    init(_ image: CGImage) { self.image = image }
  }

  func get(_ key: String) -> CGImage? { cache.object(forKey: key as NSString)?.image }
  func put(_ key: String, _ image: CGImage) { cache.setObject(CGImageBox(image), forKey: key as NSString) }
}

enum ImageLoad {
  /// A JPEG (the JPEG-512 cache) or a still, box-scaled and oriented.
  static func decode(path: String, maxPixel: Int) -> CGImage? {
    let url = URL(fileURLWithPath: path) as CFURL
    let options: [CFString: Any] = [
      kCGImageSourceCreateThumbnailFromImageAlways: true,
      kCGImageSourceCreateThumbnailWithTransform: true,
      kCGImageSourceShouldCacheImmediately: true,
      kCGImageSourceThumbnailMaxPixelSize: maxPixel,
    ]
    return CGImageSourceCreateWithURL(url, nil).flatMap {
      CGImageSourceCreateThumbnailAtIndex($0, 0, options as CFDictionary)
    }
  }

  /// The face at `box` (x, y, w, h in 0..1 of the oriented image), squared and
  /// padded so a circle frames it. In memory only.
  static func crop(_ image: CGImage, box: [Double]) -> CGImage? {
    guard box.count == 4, box[2] > 0, box[3] > 0 else { return image }
    let w = Double(image.width), h = Double(image.height)
    let cx = (box[0] + box[2] / 2) * w, cy = (box[1] + box[3] / 2) * h
    let side = min(max(box[2] * w, box[3] * h) * 1.6, min(w, h))
    let x = min(max(cx - side / 2, 0), w - side), y = min(max(cy - side / 2, 0), h - side)
    return image.cropping(to: CGRect(x: x, y: y, width: side, height: side).integral)
  }

  /// A face, cropped from the picture it was found in (face_thumb, made on a
  /// miss by the pack for every format the viewer decodes: RAW, HEIC, clips).
  /// [worker-thread]: face_thumb may decode.
  static func face(_ t: AITable, faceID: UInt64, box: [Double]) -> CGImage? {
    let key = "face|\(faceID)|\(box.map { String(format: "%.4f", $0) }.joined(separator: ","))"
    if let hit = ImageCache.shared.get(key) { return hit }
    guard t.hasFaceThumb,
          let jpeg = t.path({ t.a.face_thumb?(t.ctx, faceID, $0, $1) ?? MV_ERR_INVALID_ARG }),
          let full = decode(path: jpeg, maxPixel: 1024),
          let cut = crop(full, box: box) else { return nil }
    ImageCache.shared.put(key, cut)
    return cut
  }
}
