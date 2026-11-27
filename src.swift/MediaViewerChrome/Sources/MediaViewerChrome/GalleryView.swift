// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 18's gallery (docs/design/10-roadmap.md, folded-in Windows PR 4, docs/design/12
// 2026-09-17): "a full-client thumbnail grid... a click opens it in the
// viewer" (docs/design/16-commands.md's `G` row). `G` toggles visibility and `Esc`
// closes it (main_mac.mm's keyDown:, checked ahead of the fallback
// Esc-closes-window case). Up/Down/W/S move by row, `+`/`-` resize the cells
// (also main_mac.mm's keyDown:); this view lays out the grid and reports how
// many cells sit in a row (mv_chrome_set_gallery_columns) so the host can do
// that arithmetic.
//
// File search (docs/design/16 "File search", 2026-09-28) sits above the tiles while
// it is open (FileSearch.swift). Its name filter only chooses which tiles are
// drawn: each tile keeps its identity (its index in the listing) and its
// thumbnail slot, so filtering reloads nothing and a tile that stays shown is
// not rebuilt.
import AppKit
import MVChromeBridge
import SwiftUI

/// Dragging a gallery cell out (issue #413). In the grid's vertical scroll view
/// a SwiftUI drag gesture on the cell never sees the mouse drag, so this picks
/// it up below SwiftUI: cells report the one under the pointer, and a local
/// event monitor turns a press there that moves 4 pt into a file drag (the
/// range, or the marks, when the cell is in them; else the cell, with nothing
/// read from disk). The drag's events are the drag's: SwiftUI does not see them,
/// so the press is no click. Installed the first time a cell is hovered;
/// costs one comparison per mouse event after that.
private struct EventBox: @unchecked Sendable {
  let event: NSEvent?
}

@MainActor
final class GalleryDrag {
  static let shared = GalleryDrag()
  private var hovered: Int?
  private var pressed: (index: Int, point: NSPoint, window: NSWindow?)?
  private var monitor: Any?

  func hover(_ index: Int, inside: Bool) {
    if inside {
      hovered = index
      install()
    } else if hovered == index {
      hovered = nil
    }
  }

  private func install() {
    guard monitor == nil else { return }
    // A local monitor runs on the main thread; the box carries the event
    // across the isolation check, which NSEvent (not Sendable) cannot.
    monitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .leftMouseDragged, .leftMouseUp]) { event in
      let box = EventBox(event: event)
      return MainActor.assumeIsolated { EventBox(event: box.event.flatMap { GalleryDrag.shared.handle($0) }) }.event
    }
  }

  private func handle(_ event: NSEvent) -> NSEvent? {
    switch event.type {
    case .leftMouseDown:
      pressed = nil
      if let index = hovered, mv_chrome_gallery_visible() {
        pressed = (index, event.locationInWindow, event.window)
      }
      return event
    case .leftMouseDragged:
      guard let p = pressed, event.window === p.window else { return event }
      let dx = event.locationInWindow.x - p.point.x, dy = event.locationInWindow.y - p.point.y
      guard dx * dx + dy * dy >= 16 else { return event }
      pressed = nil
      FileDragSource.shared.begin(with: event) { FolderStore.shared.dragFiles(from: p.index) }
      return nil
    default:
      pressed = nil
      return event
    }
  }
}

struct GalleryView: View {
  @ObservedObject private var store = FolderStore.shared
  @ObservedObject private var search = FileSearchStore.shared
  // Whether any add-on fills tile badges / context entries (changes only when one
  // attaches or goes): the cells carry those wrappers only then.
  @ObservedObject private var tileFlags = SlotTileFlags.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  // The photo grid keeps its sizes under a theme's density (docs/ADDONS.md
  // "A theme's shape"): the gutter, the inset and the tiles are geometry.
  private let spacing: CGFloat = 8
  private let inset: CGFloat = 12

  var body: some View {
    grid
      // docs/design/25 PR 59: badges for the folder the grid shows, one request per
      // folder (and only while the grid is open: no add-on work for a viewer
      // that never opens it).
      .onAppear { SlotStore.shared.requestBadges(folder: SlotTargets.folder()) }
      .onChange(of: store.listingGeneration) { _, _ in
        SlotStore.shared.requestBadges(folder: SlotTargets.folder())
      }
      // A folder of folders has no image to show through the material, and the
      // canvas behind may still hold the previous folder's last photo.
      .background {
        // No photos: an opaque ground, so the previous picture does not show
        // through, and a built-in theme can lay its effect here. With photos,
        // the material stays so the picture shows through the grid.
        if store.names.isEmpty {
          ThemeField()
        } else {
          Rectangle().fill(.regularMaterial)
        }
      }
  }

  private var grid: some View {
    GeometryReader { geo in
      let cell = store.galleryCellSize
      let columns = max(1, Int((geo.size.width - 2 * inset + spacing) / (cell + spacing)))
      let mixed = !store.folders.isEmpty && !store.names.isEmpty
      let hasFolders = !store.folders.isEmpty
      // Clamped to the listing on screen: the filter's indices are one render
      // behind when the listing is swapped (a result list opening in place of
      // the folder has no folders), and a stale index traps in folders[index].
      // With no filter the store's own index arrays are used as they are: they
      // match the listing, and copying N of them per pass is what issue #179 cut.
      let shownItems = search.items.map { $0.filter { $0 < store.names.count } } ?? store.itemIndices
      let shownFolders = search.folders.map { $0.filter { $0 < store.folders.count } } ?? store.folderIndices
      let filteredEmpty = search.items != nil && shownItems.isEmpty && shownFolders.isEmpty
      let here = store.currentDir ?? ""
      // The grid's columns are fixed and centred: a section header lines up
      // with the first column, not the window's edge.
      let gridWidth = CGFloat(columns) * cell + CGFloat(columns - 1) * spacing
      let headerLead = max(0, (geo.size.width - 2 * inset - gridWidth) / 2)
      VStack(spacing: 0) {
        // Fades in and out over the grid (docs/design/20 "Craft rules": nothing
        // pops); the grid takes its new place at once, as on Windows.
        VStack(spacing: 0) {
          if search.isOpen {
            FileSearchBar()
              .padding(.horizontal, inset)
              .padding(.top, MVTheme.space(10))
              .transition(.opacity)
          }
        }
        .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: search.isOpen)
        if hasFolders, let query = store.folderQuery {
          Text(query.isEmpty ? "Find folder" : "Find folder: \(query)")
            .font(MVTheme.font(13))
            .fontWeight(.medium)
            .foregroundStyle(MVTheme.title)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, inset)
            .padding(.top, MVTheme.space(8))
        }
        ScrollViewReader { proxy in
          ScrollView {
            // docs/design/16 "Folder navigation": folders first, then the photos,
            // in one grid (Finder). Each is its own section, so the photos start
            // on a row of their own -- the host's arrow arithmetic (galleryMoveRows)
            // treats the tiles as a block of rows above them.
            LazyVGrid(
              columns: Array(repeating: GridItem(.fixed(cell), spacing: spacing), count: columns),
              spacing: spacing
            ) {
              if hasFolders {
                Section {
                  ForEach(shownFolders, id: \.self) { index in
                    // A stale row of a list that just shrank: skip it, never index past the end.
                    if store.folders.indices.contains(index) {
                      let path = store.folders[index]
                      FolderTile(
                        index: index, path: path, size: cell,
                        isCursor: index == store.folderCursor,
                        card: store.card(for: path)
                      )
                      .id("folder-\(index)")
                      .onTapGesture {
                        switch TileClick.of(clickCount: NSApp.currentEvent?.clickCount ?? 1) {
                        case .select: store.selectFolder(at: index)
                        case .open: store.openFolder(at: index)
                        }
                      }
                      // Drop into it; rest on it with a drag and it opens.
                      .folderDropTarget(path, name: store.folderName(path)) {
                        if let i = store.folders.firstIndex(of: path) { store.openFolder(at: i) }
                      }
                      .onAppear {
                        store.folderTileAppeared(index)
                        GalleryScroll.shared.visible.appear(-(index + 1))
                      }
                      .onDisappear {
                        store.folderTileDisappeared(index)
                        GalleryScroll.shared.visible.disappear(-(index + 1))
                      }
                    }
                  }
                }
              }
              if !store.names.isEmpty {
                Section {
                  ForEach(shownItems, id: \.self) { index in
                    if store.names.indices.contains(index) {
                      GalleryCell(
                        index: index, name: store.names[index], size: cell,
                        onFolders: store.folderCursor >= 0,
                        highlight: store.highlight(at: index),
                        isMarked: store.markedNames.contains(store.names[index]),
                        isClip: store.isClip(at: index),
                        thumbKey: store.thumbKey(at: index), slot: store.slot(at: index),
                        slotBadges: tileFlags.badges,
                        why: search.why[index]
                      )
                      .id(index)
                      .onTapGesture {
                        // Issue #413: Shift-click selects every image from the
                        // selected cell to this one and the grid stays; a plain
                        // click opens this one.
                        if NSEvent.modifierFlags.contains(.shift) {
                          store.extendRange(to: index, shown: shownItems)
                        } else {
                          store.selectAndCloseGallery(index)
                        }
                      }
                      // Drag out the original (the range or the marks, when this
                      // cell is in them). Started by GalleryDrag's event monitor:
                      // the grid's scroll view keeps a SwiftUI drag from reaching
                      // the cell (#413). Right-click: the add-ons' entries, then
                      // Share (TileMenu, #444), picked up the same way.
                      .onHover { inside in
                        GalleryDrag.shared.hover(index, inside: inside)
                        TileMenu.shared.hover(index, inside: inside, gallery: true)
                      }
                      .onAppear { GalleryScroll.shared.visible.appear(index) }
                      .onDisappear { GalleryScroll.shared.visible.disappear(index) }
                    }
                  }
                } header: {
                  // The photos' row, right over them (Windows' photosRow): the
                  // section's name in a mixed folder. Share is the command bar's
                  // one button (BarShareButton), not a second one over the grid.
                  if mixed && !shownItems.isEmpty {
                    HStack(spacing: MVTheme.space(8)) {
                      Text("Photos and videos  \(shownItems.count)")
                        .font(MVTheme.font(12))
                        .fontWeight(.semibold)
                        .foregroundStyle(MVTheme.body)
                        .monospacedDigit()
                        .lineLimit(1)
                      Spacer(minLength: 0)
                    }
                    .padding(.top, MVTheme.space(8))
                    .padding(.leading, headerLead)
                  }
                }
              }
            }
            .padding(inset)
            .frame(maxWidth: .infinity)

            if filteredEmpty {
              FileSearchNotice()
            } else if store.folders.isEmpty && store.names.isEmpty {
              Text("No supported photos or videos in this folder")
                .font(MVTheme.font(14))
                .foregroundStyle(MVTheme.body)
                .frame(maxWidth: .infinity)
                .padding(.top, MVTheme.space(60))
            }
          }
          // In a child that observes only the selection: an arrow key re-runs
          // it, not this body (issue #179).
          .background {
            FollowSelection(proxy: proxy, anchor: nil, onFolders: store.folderCursor >= 0)
          }
          .onChange(of: store.folderCursor) { _, cursor in
            if cursor >= 0 {
              proxy.scrollTo("folder-\(cursor)")
            } else if store.currentIndex >= 0 {
              proxy.scrollTo(store.currentIndex)
            }
          }
          // Another folder on screen: Back / Forward put the grid where that
          // folder had it; any other way in starts at the top, on what the host
          // selected. One jump, unanimated, before the cells draw.
          .onChange(of: store.listingGeneration) { _, _ in
            let scroll = GalleryScroll.shared
            let dir = store.currentDir ?? ""
            guard dir != scroll.dir else { return }
            scroll.memory.remember(scroll.dir, top: scroll.visible.top)
            scroll.dir = dir
            if store.arrivedByHistory, let top = scroll.memory.top(for: dir) {
              proxy.scrollTo(top < 0 ? AnyHashable("folder-\(-(top + 1))") : AnyHashable(top), anchor: .top)
            } else if store.folderCursor >= 0 {
              proxy.scrollTo("folder-\(store.folderCursor)")
            } else if store.currentIndex > 0 {
              proxy.scrollTo(store.currentIndex)
            } else {
              proxy.scrollTo(store.folders.isEmpty ? AnyHashable(0) : AnyHashable("folder-0"), anchor: .top)
            }
          }
          .onAppear {
            GalleryScroll.shared.dir = store.currentDir ?? ""
            if store.folderCursor >= 0 {
              proxy.scrollTo("folder-\(store.folderCursor)", anchor: .center)
            }
          }
        }
      }
      // The grid is the folder on screen: what is dropped on it goes there.
      .folderDropTarget(here, name: store.crumbs.last?.name ?? "", radius: MVTheme.radius(10), inset: -6)
      .overlay(alignment: .bottom) { DropLabel() }
      .onAppear { mv_chrome_set_gallery_columns(Int32(columns)) }
      .onChange(of: columns) { _, new in mv_chrome_set_gallery_columns(Int32(new)) }
    }
  }
}

/// Where the grid is scrolled, per folder (FolderNavModel.swift). Not
/// published: cells write it as they come and go, and only a change of folder
/// reads it.
@MainActor
final class GalleryScroll {
  static let shared = GalleryScroll()
  var memory = ScrollMemory()
  var visible = VisibleCells()
  var dir = ""
}

/// A child folder: up to three photos as a cover, one big and two small (from
/// the folder itself, spread across it, or for a year of month folders one from
/// each of the first months that have one), name, and a count.
private struct FolderTile: View {
  let index: Int
  let path: String
  let size: CGFloat
  let isCursor: Bool
  @ObservedObject var card: FolderCard
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(spacing: 4) {  // the tile's height: grid geometry
      Color.clear
        .frame(width: size, height: size)
        .overlay {
          if let cover = card.cover {
            FolderMosaic(big: cover, smalls: card.smallCovers.compactMap { $0 }, size: size)
          } else {
            ZStack {
              Rectangle().fill(.quaternary)
              Image(systemName: "folder.fill")
                .font(.system(size: size * 0.32))
                .foregroundStyle(MVTheme.body)
            }
          }
        }
        .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
        .overlay(alignment: .bottomLeading) {
          if card.loaded {
            Text(card.status)
              .font(MVTheme.font(11))
              .foregroundStyle(MVTheme.title)
              .padding(.horizontal, MVTheme.space(6))
              .padding(.vertical, MVTheme.space(2))
              .background(.ultraThinMaterial, in: Capsule())
              .padding(MVTheme.space(6))
          }
        }
        // The keyboard's tile (arrows on the folder row): the one ring.
        .overlay(
          RoundedRectangle(cornerRadius: MVTheme.radius(6))
            .strokeBorder(isCursor ? MVTheme.accent : .clear, lineWidth: 2)
        )
        .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: isCursor)
      // Filled at this size (docs/design/20 "Craft rules"), centred under
      // the cover like a photo's name.
      HStack(spacing: MVTheme.space(4)) {
        Image(systemName: "folder.fill").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        Text(FolderStore.shared.folderName(path))
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
          .truncationMode(.middle)
      }
      .frame(width: size)
    }
    .contextMenu { FolderShareMenu(path: path) }
    .onAppear { FolderStore.shared.requestFolderSummaryIfNeeded(at: index) }
  }
}

/// A folder tile's photos: one fills it; two split it in halves; three put the
/// big one over two thirds and stack the small ones beside it, split by a
/// hairline of the gallery behind (WinUI's LayoutFolderMosaic, the same gap).
private struct FolderMosaic: View {
  let big: CGImage
  let smalls: [CGImage]
  let size: CGFloat
  private static let gap: CGFloat = 2

  var body: some View {
    let gap = Self.gap
    switch smalls.count {
    case 0:
      cell(big, width: size, height: size)
    case 1:
      let half = (size - gap) / 2
      HStack(spacing: gap) {
        cell(big, width: half, height: size)
        cell(smalls[0], width: half, height: size)
      }
    default:
      let side = (size - gap) / 3
      let row = (size - gap) / 2
      HStack(spacing: gap) {
        cell(big, width: size - gap - side, height: size)
        VStack(spacing: gap) {
          cell(smalls[0], width: side, height: row)
          cell(smalls[1], width: side, height: row)
        }
      }
    }
  }

  private func cell(_ image: CGImage, width: CGFloat, height: CGFloat) -> some View {
    Image(decorative: image, scale: 1)
      .resizable()
      .aspectRatio(contentMode: .fill)
      .frame(width: width, height: height)
      .clipped()
  }
}

/// Scrolls the selection into view as it moves. Observes only the selection,
/// so the grid or strip around it is not rebuilt on each arrow key (issue #179).
/// Deliberately not animated: an animation per key-repeat queued up behind
/// each other and read as lag.
struct FollowSelection: View {
  @ObservedObject private var current = FolderStore.shared.current
  let proxy: ScrollViewProxy
  let anchor: UnitPoint?
  /// The gallery's keyboard is on a folder tile: the images do not scroll.
  var onFolders = false

  var body: some View {
    Color.clear
      .frame(width: 0, height: 0)
      .accessibilityHidden(true)
      .onChange(of: current.index) { _, newIndex in
        guard newIndex >= 0, !onFolders else { return }
        proxy.scrollTo(newIndex, anchor: anchor)
      }
  }
}

private struct GalleryCell: View {
  let index: Int
  let name: String
  let size: CGFloat
  /// The keyboard is on a folder tile, so no image is current.
  let onFolders: Bool
  @ObservedObject var highlight: CellHighlight
  let isMarked: Bool
  let isClip: Bool
  let thumbKey: String
  @ObservedObject var slot: ThumbSlot
  var slotBadges = false
  /// #291: the tag a file search matched on ("Canon EOS R5"), under the name.
  var why: String? = nil
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    // Lift, don't ring (docs/design/20 "Craft rules"): the range is a wash
    // over its thumbnails; the accent ring is the keyboard's cell alone. Both
    // fade in 150 ms on the change (a cut with Reduce Motion), nothing on scroll.
    let fade: Animation? = reduceMotion ? nil : .easeOut(duration: 0.15)
    VStack(spacing: 4) {  // the tile's height: grid geometry
      // Always a square: the placeholder and the loaded image must occupy the
      // same box, or rows go ragged as thumbnails arrive.
      Color.clear
        .frame(width: size, height: size)
        .overlay {
          if let image = slot.image {
            Image(decorative: image, scale: 1)
              .resizable()
              .aspectRatio(contentMode: .fit)
          } else {
            Rectangle().fill(.quaternary)
          }
        }
        .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
        // Issue #413: the shift-click range is tinted.
        .overlay {
          if highlight.inRange {
            RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(MVTheme.accent.opacity(0.22))
              .transition(.opacity)
          }
        }
        .animation(fade, value: highlight.inRange)
        .overlay(
          RoundedRectangle(cornerRadius: MVTheme.radius(6))
            .strokeBorder(highlight.isCurrent && !onFolders ? MVTheme.accent : .clear, lineWidth: 2)
            .animation(fade, value: highlight.isCurrent && !onFolders)
        )
        .overlay { if isClip { PlayBadge(diameter: 40) } }
        .overlay(alignment: .topTrailing) { if isMarked { MarkBadge() } }
        // docs/design/25 PR 59: the add-ons' badges, from a table (no add-on call per
        // tile); with no add-on contributing badges the cell is exactly the plain one.
        .modifier(SlotBadgesOverlay(on: slotBadges, name: name))
      Text(name)
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.title)
        .lineLimit(1)
        .truncationMode(.middle)
        .frame(width: size)
      if let why {
        Text(why)
          .font(MVTheme.font(11))
          .foregroundStyle(MVTheme.body)
          .lineLimit(1)
          .truncationMode(.tail)
          .frame(width: size)
          .accessibilityLabel("Matched tag: \(why)")
      }
    }
    .onAppear { FolderStore.shared.requestThumbnailIfNeeded(at: index) }
    // Cells are identified by index: a new listing (a result list replacing
    // the folder, another folder with the same names) or a rewritten file
    // reuses them without a fresh onAppear, so ask again.
    .onChange(of: thumbKey) { _, _ in FolderStore.shared.requestThumbnailIfNeeded(at: index) }
  }
}

