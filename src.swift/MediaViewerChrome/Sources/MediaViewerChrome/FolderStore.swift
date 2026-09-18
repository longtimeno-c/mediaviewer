// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's filmstrip/gallery follow-up (plan/12 2026-09-17): the SwiftUI-side
// state both views read. Owns nothing MvLabApp doesn't already own on the
// C++ side (folder_model, browse_index) — this only mirrors item
// count/selection (cheap, polled) and caches thumbnail images as they
// arrive (pushed, via mv_chrome_set_thumb_ready_callback), the same
// "publish state, consumer observes" shape the rest of this bridge uses
// rather than a synchronous/blocking query.
import AppKit
import Combine
import MVChromeBridge

@MainActor
final class FolderStore: ObservableObject {
  static let shared = FolderStore()

  @Published private(set) var itemCount: Int = 0
  @Published private(set) var currentIndex: Int = -1
  @Published private(set) var thumbnails: [Int: NSImage] = [:]

  // Indices already asked for, so scrolling back and forth over the same
  // cells doesn't re-request a thumbnail that's already in flight or cached
  // (folder_model_mac's own SQLite lookup is cheap on a hit, but still a
  // pool-thread round trip worth not repeating per redraw).
  private var requested: Set<Int> = []
  private var pollTimer: Timer?

  private init() {
    // main_mac.mm's g_thumb_ready_callback is always called on the main
    // thread (mv_chrome_bridge.h's contract) -- thumbReadyTrampoline below
    // can touch @Published state directly rather than hopping again.
    mv_chrome_set_thumb_ready_callback(thumbReadyTrampoline)
    // itemCount/currentIndex are plain integer reads (mv_chrome_bridge.h),
    // not I/O -- polling them is the same cadence/shape MvLabApp's own
    // -refreshFolderIfChanged uses for folder_model's watch (a 0.2s NSTimer),
    // not a new mechanism.
    pollTimer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
      // Timer's closure isn't statically known to the compiler to run on
      // the main actor, but scheduledTimer(withTimeInterval:) always fires
      // on the run loop it was scheduled from -- main, since -init runs on
      // FolderStore's own @MainActor -- so this is a real (not just
      // asserted-and-hoped) isolation, same as thumbReadyTrampoline below.
      MainActor.assumeIsolated {
        self?.pollSelection()
      }
    }
    pollSelection()
  }

  private func pollSelection() {
    let count = Int(mv_chrome_item_count())
    let index = Int(mv_chrome_current_index())
    if count != itemCount { itemCount = count }
    if index != currentIndex { currentIndex = index }
    if count == 0 {
      if !thumbnails.isEmpty { thumbnails.removeAll() }
      if !requested.isEmpty { requested.removeAll() }
    }
  }

  func name(at index: Int) -> String {
    var buf = [CChar](repeating: 0, count: 512)
    let ok = buf.withUnsafeMutableBufferPointer { ptr -> Bool in
      guard let base = ptr.baseAddress else { return false }
      return mv_chrome_item_name(Int32(index), base, Int32(ptr.count))
    }
    return ok ? String(cString: buf) : ""
  }

  func select(_ index: Int) {
    mv_chrome_select_index(Int32(index))
  }

  func selectAndCloseGallery(_ index: Int) {
    mv_chrome_select_index_and_close_gallery(Int32(index))
  }

  // Called from a cell's .onAppear -- requests are lazy, matching PR 18's
  // own verify line ("2000 mixed JPEGs... second folder visit has
  // near-instant thumbnails" -- a cache hit is fast regardless, but a miss
  // decoding 2000 files up front is exactly the "filmstrip scrolls without
  // a hitch" bar this is here to clear).
  func requestThumbnailIfNeeded(at index: Int) {
    guard thumbnails[index] == nil, !requested.contains(index) else { return }
    requested.insert(index)
    mv_chrome_request_thumb(Int32(index))
  }

  fileprivate func thumbnailReady(index: Int, path: String?) {
    guard let path, let image = NSImage(contentsOfFile: path) else { return }
    thumbnails[index] = image
  }
}

// A top-level, non-capturing function is what makes this convertible to the
// C function pointer mv_chrome_set_thumb_ready_callback expects
// (@convention(c) requires no captures) -- it cannot be a closure or a
// method reference. It is deliberately not itself @MainActor: a C function
// pointer can't carry actor isolation, so the isolation is asserted inside
// with MainActor.assumeIsolated instead -- valid because main_mac.mm's
// implementation of mv_chrome_request_thumb always hops to
// dispatch_get_main_queue() before calling this (mv_chrome_bridge.h's
// documented contract), which is the same thread Swift's main actor runs on.
private func thumbReadyTrampoline(_ index: Int32, _ pathUTF8: UnsafePointer<CChar>?) {
  let path = pathUTF8.map { String(cString: $0) }
  MainActor.assumeIsolated {
    FolderStore.shared.thumbnailReady(index: Int(index), path: path)
  }
}
