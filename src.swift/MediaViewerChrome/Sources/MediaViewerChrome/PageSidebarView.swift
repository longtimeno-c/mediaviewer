// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #429, the page sidebar (docs/design/04 "Page sidebar"): a strip of page
// thumbnails down the left of a PDF, DOCX or PPTX with the filmstrip hidden,
// like Preview's. main_mac.mm decides when it shows (-pageSidebarVisible) and
// reserves its width on the canvas; this view only draws, asks for the
// thumbnails of the pages it shows, and turns to a page that is clicked.
import CoreGraphics
import ImageIO
import MVChromeBridge
import SwiftUI

/// The sidebar's state: the page count and the page on screen (pushed by the
/// host through MVChromeHost.setPageSidebar), and the thumbnails that landed.
@MainActor
final class PageSidebarStore: ObservableObject {
  static let shared = PageSidebarStore()

  @Published private(set) var count = 0
  @Published private(set) var current = 0
  /// Names the document the thumbnails belong to (page_thumb_book's serial).
  private var serial: UInt32 = 0
  /// One slot per page, so a thumbnail landing re-renders its own cell only.
  private var slots: [Int: PageThumbSlot] = [:]
  /// Pages whose cells are on screen; the request is their span.
  private var visible = Set<Int>()
  private var requestQueued = false
  /// Where each landed thumbnail is on disk: a page scrolled back to after
  /// its picture was dropped decodes it again without asking the host.
  private var paths: [Int: String] = [:]
  /// Decoded thumbnails kept, oldest first. 320 px pages are ~0.3 MB each.
  private var decoded: [Int] = []
  private let maxDecoded = 160

  private static let decodeQueue = DispatchQueue(
    label: "mediaviewer.page-thumb-decode", qos: .userInitiated, attributes: .concurrent)

  private init() {}

  func set(count: Int, current: Int, serial: UInt32) {
    if serial != self.serial { adopt(serial) }
    if count != self.count { self.count = count }
    if current != self.current { self.current = current }
  }

  func slot(_ page: Int) -> PageThumbSlot {
    if let s = slots[page] { return s }
    let s = PageThumbSlot()
    slots[page] = s
    return s
  }

  func appeared(_ page: Int) {
    visible.insert(page)
    if slots[page]?.image == nil, let path = paths[page] {
      decode(serial: serial, page: page, path: path)
    }
    queueRequest()
  }

  func disappeared(_ page: Int) {
    visible.remove(page)
  }

  func isVisible(_ page: Int) -> Bool { visible.contains(page) }

  func select(_ page: Int) { mv_chrome_select_page(Int32(page)) }

  /// One request per run-loop turn, however many cells appeared: the span of
  /// the cells on screen and two either side. The host renders only what is
  /// not cached or in flight, and drops what a fling has already left.
  private func queueRequest() {
    guard !requestQueued else { return }
    requestQueued = true
    DispatchQueue.main.async {
      MainActor.assumeIsolated {
        let store = PageSidebarStore.shared
        store.requestQueued = false
        guard store.count > 1, let lo = store.visible.min(), let hi = store.visible.max() else {
          return
        }
        let first = max(0, lo - 2)
        let last = min(store.count - 1, hi + 2)
        guard first <= last else { return }
        mv_chrome_request_page_thumbs(Int32(first), Int32(last - first + 1))
      }
    }
  }

  /// Another document (or none): every cell and thumbnail belonged to the last.
  private func adopt(_ serial: UInt32) {
    self.serial = serial
    for s in slots.values { s.image = nil }
    slots.removeAll()
    paths.removeAll()
    decoded.removeAll()
  }

  fileprivate func thumbReady(serial: UInt32, page: Int, path: String) {
    if serial != self.serial {
      // The host started this document's serial after it last told us
      // (the first request on a new stop); anything older is stale.
      guard serial > self.serial else { return }
      adopt(serial)
    }
    paths[page] = path
    decode(serial: serial, page: page, path: path)
  }

  /// Read and decode off the main thread, as the filmstrip does.
  private func decode(serial: UInt32, page: Int, path: String) {
    Self.decodeQueue.async {
      let image = Self.decode(atPath: path)
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          PageSidebarStore.shared.thumbDecoded(serial: serial, page: page, image: image)
        }
      }
    }
  }

  private func thumbDecoded(serial: UInt32, page: Int, image: CGImage?) {
    guard serial == self.serial, let image else { return }
    slot(page).image = image
    decoded.append(page)
    // The oldest first, but a cell still on screen keeps its picture.
    while decoded.count > maxDecoded, let at = decoded.firstIndex(where: { !visible.contains($0) }) {
      let evicted = decoded.remove(at: at)
      if !decoded.contains(evicted) { slots[evicted]?.image = nil }
    }
  }

  nonisolated private static func decode(atPath path: String) -> CGImage? {
    let url = URL(fileURLWithPath: path) as CFURL
    let options: [CFString: Any] = [
      kCGImageSourceCreateThumbnailFromImageAlways: true,
      kCGImageSourceShouldCacheImmediately: true,
      kCGImageSourceThumbnailMaxPixelSize: 320,
    ]
    return CGImageSourceCreateWithURL(url, nil).flatMap {
      CGImageSourceCreateThumbnailAtIndex($0, 0, options as CFDictionary)
    }
  }
}

@MainActor
final class PageThumbSlot: ObservableObject {
  @Published var image: CGImage?
}

struct PageSidebarView: View {
  @ObservedObject private var store = PageSidebarStore.shared

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView(.vertical, showsIndicators: true) {
        // Lazy: a 300-page document builds (and asks for) only the cells
        // near what is on screen.
        LazyVStack(spacing: 10) {
          ForEach(0..<store.count, id: \.self) { page in
            PageSidebarCell(page: page, isCurrent: page == store.current, slot: store.slot(page))
              .id(page)
              .onTapGesture { store.select(page) }
              .onAppear { store.appeared(page) }
              .onDisappear { store.disappeared(page) }
          }
        }
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
      }
      // The page on screen stays in view as the document scrolls; a page
      // already visible does not move the strip under the pointer.
      .onChange(of: store.current) { _, page in
        guard !store.isVisible(page) else { return }
        proxy.scrollTo(page, anchor: .center)
      }
      .onChange(of: store.count) { _, _ in
        proxy.scrollTo(store.current, anchor: .center)
      }
    }
    .background(.regularMaterial)
    .overlay(alignment: .trailing) {
      Rectangle().fill(MVTheme.hairline).frame(width: 1)
    }
  }
}

private struct PageSidebarCell: View {
  let page: Int
  let isCurrent: Bool
  @ObservedObject var slot: PageThumbSlot

  var body: some View {
    VStack(spacing: 4) {
      ZStack {
        if let image = slot.image {
          Image(decorative: image, scale: 1)
            .resizable()
            .aspectRatio(contentMode: .fit)
            .shadow(color: .black.opacity(0.25), radius: 2, y: 1)
        } else {
          // A portrait page until the thumbnail says otherwise.
          Rectangle().fill(.quaternary).aspectRatio(0.707, contentMode: .fit)
        }
      }
      .frame(width: 104, height: 136)
      .padding(4)
      .overlay(
        RoundedRectangle(cornerRadius: MVTheme.radius(4))
          .strokeBorder(isCurrent ? MVTheme.accent : .clear, lineWidth: 2)
      )
      Text("\(page + 1)")
        .font(MVTheme.font(11))
        .foregroundStyle(isCurrent ? MVTheme.title : MVTheme.body)
    }
    .contentShape(Rectangle())
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("Page \(page + 1)")
    .accessibilityAddTraits(isCurrent ? [.isButton, .isSelected] : .isButton)
  }
}

extension MVChromeHost {
  /// Issue #429: what the page sidebar shows. `count` 0 when it is hidden.
  @MainActor @objc public static func setPageSidebar(count: Int32, current: Int32, serial: UInt32) {
    PageSidebarStore.shared.set(count: Int(count), current: Int(current), serial: serial)
  }

  /// A page's thumbnail landed (main thread), for the document `serial` names.
  @MainActor @objc public static func pageThumbReady(serial: UInt32, page: Int32, path: String) {
    PageSidebarStore.shared.thumbReady(serial: serial, page: Int(page), path: path)
  }

  @objc public static func makePageSidebarView() -> NSView {
    let hosting = NSHostingView(rootView: ThemedRoot(content: PageSidebarView()))
    hosting.sizingOptions = []
    hosting.translatesAutoresizingMaskIntoConstraints = false
    return hosting
  }
}
