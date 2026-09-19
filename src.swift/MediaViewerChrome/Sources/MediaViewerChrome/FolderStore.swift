// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's filmstrip/gallery follow-up (plan/12 2026-09-17): the SwiftUI-side
// state both views read. Owns nothing MvLabApp doesn't already own on the
// C++ side (folder_model, browse_index) — this only mirrors item
// count/selection/names (cheap, polled) and caches decoded thumbnails as they
// arrive (pushed, via mv_chrome_set_thumb_ready_callback), the same
// "publish state, consumer observes" shape the rest of this bridge uses
// rather than a synchronous/blocking query.
//
// Scroll-lag design (found on real hardware, 2026-09-19): the first version
// kept every thumbnail in one @Published dictionary that every cell observed,
// so each arriving thumbnail (and each selection change) re-rendered every
// visible cell, each of which also made a C call to fetch its name, and the
// image was read and decoded on the main thread. Now:
//   * each thumbnail lives in its own ThumbSlot -- an arrival re-renders one cell;
//   * names are cached, refreshed only when the host's listing generation moves;
//   * the JPEG is decoded off the main thread (ImageIO) before it reaches a slot;
//   * decoded images are bounded (LRU), and thumbnails around the selection are
//     requested ahead of the scroll so cells are already filled when they appear.
import AppKit
import Combine
import ImageIO
import MVChromeBridge

/// One thumbnail. Cells observe their own slot, never the whole store.
@MainActor
final class ThumbSlot: ObservableObject {
  @Published fileprivate(set) var image: CGImage?
}

@MainActor
final class FolderStore: ObservableObject {
  static let shared = FolderStore()

  @Published private(set) var itemCount: Int = 0
  @Published private(set) var currentIndex: Int = -1
  /// Item names by index; replaced wholesale when the host relists.
  @Published private(set) var names: [String] = []
  /// Names of marked items (plan/16 marks), rebuilt only when the host's marks
  /// generation or listing changes.
  @Published private(set) var markedNames: Set<String> = []
  @Published private(set) var markedCount: Int = 0
  /// Gallery cell edge in points (plan/16 `+`/`-`: 24 pt steps, 80-344, start 152).
  @Published private(set) var galleryCellSize: CGFloat = 152

  private var slots: [String: ThumbSlot] = [:]
  // Names asked for and not yet failed/evicted, so scrolling back and forth
  // doesn't re-request a thumbnail that is in flight or already decoded.
  // Keyed by name, not index: a relist can shift which item sits at an index
  // (mv_chrome_bridge.h's thumb-ready callback is keyed by name for the same
  // reason).
  private var requested: Set<String> = []
  // Decode order, oldest first, for eviction.
  private var decoded: [String] = []
  private let maxDecoded = 300
  private var listingGeneration: UInt64 = .max
  private var marksGeneration: UInt64 = .max
  private var pollTimer: Timer?

  private static let decodeQueue = DispatchQueue(
    label: "mediaviewer.thumb-decode", qos: .userInitiated, attributes: .concurrent)

  private init() {
    // main_mac.mm's g_thumb_ready_callback is always called on the main
    // thread (mv_chrome_bridge.h's contract).
    mv_chrome_set_thumb_ready_callback(thumbReadyTrampoline)
    // itemCount/currentIndex are plain integer reads (mv_chrome_bridge.h) --
    // the same 0.15 s cadence MvLabApp's own -refreshFolderIfChanged uses.
    pollTimer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
      // scheduledTimer fires on the run loop it was scheduled from (main,
      // since init runs on the main actor).
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
  }

  private func poll() {
    let count = Int(mv_chrome_item_count())
    let index = Int(mv_chrome_current_index())
    let generation = mv_chrome_listing_generation()

    let listingChanged = generation != listingGeneration || count != names.count
    if listingChanged {
      listingGeneration = generation
      reloadNames(count: count)
    }
    let marks = mv_chrome_marks_generation()
    if listingChanged || marks != marksGeneration {
      marksGeneration = marks
      reloadMarks()
    }
    if count != itemCount { itemCount = count }
    if index != currentIndex {
      currentIndex = index
      prefetch(around: index)
    }
  }

  private func reloadNames(count: Int) {
    var fresh: [String] = []
    fresh.reserveCapacity(count)
    var buf = [CChar](repeating: 0, count: 1024)
    for i in 0..<count {
      let ok = buf.withUnsafeMutableBufferPointer { ptr -> Bool in
        guard let base = ptr.baseAddress else { return false }
        return mv_chrome_item_name(Int32(i), base, Int32(ptr.count))
      }
      fresh.append(ok ? String(cString: buf) : "")
    }
    names = fresh
    if count == 0 {
      slots.removeAll()
      requested.removeAll()
      decoded.removeAll()
    }
  }

  private func reloadMarks() {
    let total = Int(mv_chrome_marked_count())
    var fresh = Set<String>()
    if total > 0 {
      for i in names.indices where mv_chrome_is_marked(Int32(i)) { fresh.insert(names[i]) }
    }
    if total != markedCount { markedCount = total }
    if fresh != markedNames { markedNames = fresh }
  }

  func name(at index: Int) -> String {
    names.indices.contains(index) ? names[index] : ""
  }

  func slot(for name: String) -> ThumbSlot {
    if let existing = slots[name] { return existing }
    let created = ThumbSlot()
    slots[name] = created
    return created
  }

  func select(_ index: Int) { mv_chrome_select_index(Int32(index)) }

  func selectAndCloseGallery(_ index: Int) { mv_chrome_select_index_and_close_gallery(Int32(index)) }

  func adjustGalleryCellSize(direction: Int) {
    let next = galleryCellSize + CGFloat(direction) * 24
    galleryCellSize = min(344, max(80, next))
  }

  // Called from a cell's .onAppear -- requests are lazy, matching PR 18's
  // own verify line ("2000 mixed JPEGs... filmstrip scrolls without a hitch").
  func requestThumbnailIfNeeded(at index: Int) {
    let itemName = name(at: index)
    guard !itemName.isEmpty, !requested.contains(itemName) else { return }
    requested.insert(itemName)
    mv_chrome_request_thumb(Int32(index))
  }

  /// Keeps the neighbours of the selection warm so arrow-key stepping lands on
  /// filled cells instead of placeholders.
  private func prefetch(around index: Int) {
    guard index >= 0 else { return }
    let lo = max(0, index - 12)
    let hi = min(names.count - 1, index + 12)
    guard lo <= hi else { return }
    // Nearest-first, so the cell about to be selected is decoded first.
    for distance in 0...(hi - lo) {
      for i in [index + distance, index - distance] where i >= lo && i <= hi {
        requestThumbnailIfNeeded(at: i)
      }
    }
  }

  fileprivate func thumbnailReady(name: String, path: String?) {
    guard let path else {
      // Failed: allow a later re-request rather than pinning a placeholder.
      requested.remove(name)
      return
    }
    // Read + decode off the main thread (CLAUDE.md rule 1 applies to chrome
    // too): ImageIO with ShouldCacheImmediately does the JPEG decode here, so
    // the main thread only ever assigns a finished bitmap.
    Self.decodeQueue.async {
      let url = URL(fileURLWithPath: path) as CFURL
      let options: [CFString: Any] = [
        kCGImageSourceCreateThumbnailFromImageAlways: true,
        kCGImageSourceCreateThumbnailWithTransform: true,
        kCGImageSourceShouldCacheImmediately: true,
        kCGImageSourceThumbnailMaxPixelSize: 384,
      ]
      let image = CGImageSourceCreateWithURL(url, nil).flatMap {
        CGImageSourceCreateThumbnailAtIndex($0, 0, options as CFDictionary)
      }
      DispatchQueue.main.async {
        MainActor.assumeIsolated { FolderStore.shared.thumbnailDecoded(name: name, image: image) }
      }
    }
  }

  private func thumbnailDecoded(name: String, image: CGImage?) {
    guard let image else {
      requested.remove(name)
      return
    }
    slot(for: name).image = image
    decoded.append(name)
    while decoded.count > maxDecoded {
      let evicted = decoded.removeFirst()
      // Not the selected item, whose cell is always on screen.
      slots[evicted]?.image = nil
      requested.remove(evicted)
    }
  }
}

// A top-level, non-capturing function is what makes this convertible to the
// C function pointer mv_chrome_set_thumb_ready_callback expects
// (@convention(c) requires no captures). It is deliberately not itself
// @MainActor: a C function pointer can't carry actor isolation, so the
// isolation is asserted inside with MainActor.assumeIsolated -- valid because
// main_mac.mm always hops to dispatch_get_main_queue() before calling this
// (mv_chrome_bridge.h's documented contract).
private func thumbReadyTrampoline(
  _ nameUTF8: UnsafePointer<CChar>?, _ pathUTF8: UnsafePointer<CChar>?
) {
  guard let nameUTF8 else { return }
  let name = String(cString: nameUTF8)
  let path = pathUTF8.map { String(cString: $0) }
  MainActor.assumeIsolated {
    FolderStore.shared.thumbnailReady(name: name, path: path)
  }
}
