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
    monitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .leftMouseDragged, .leftMouseUp]) { event in
      MainActor.assumeIsolated { GalleryDrag.shared.handle(event) }
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
      let foldersOnly = !store.folders.isEmpty && store.names.isEmpty
      // The layout (strip or big tiles) follows the folder, not the filter,
      // so typing never flips it; the filter picks the tiles inside it.
      // Clamped to the listing on screen: the filter's indices are one render
      // behind when the listing is swapped (a result list opening in place of
      // the folder has no folders), and a stale index traps in folders[index].
      // With no filter the store's own index arrays are used as they are: they
      // match the listing, and copying N of them per pass is what issue #179 cut.
      let shownItems = search.items.map { $0.filter { $0 < store.names.count } } ?? store.itemIndices
      let shownFolders = search.folders.map { $0.filter { $0 < store.folders.count } } ?? store.folderIndices
      let filteredEmpty = search.items != nil && shownItems.isEmpty && shownFolders.isEmpty
      VStack(spacing: 0) {
        if search.isOpen {
          FileSearchBar()
            .padding(.horizontal, inset)
            .padding(.top, 10)
        }
        if (mixed || foldersOnly), let query = store.folderQuery {
          Text(query.isEmpty ? "Find folder" : "Find folder: \(query)")
            .font(MVTheme.font(14))
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, inset)
            .padding(.top, 8)
        }
        // Issue #413: one share icon for the grid's selection (the shift-click
        // range, else the selected cell): Sharepoint, not the OS share sheet.
        if !store.names.isEmpty && (store.currentIndex >= 0 || !store.range.isEmpty) {
          HStack {
            if store.range.count > 1 {
              Text("\(store.range.count) selected")
                .font(MVTheme.font(13))
                .foregroundStyle(MVTheme.body)
            }
            Spacer()
            GalleryShareButton()
          }
          .padding(.horizontal, inset)
          .padding(.top, 8)
        }
        if mixed && !shownFolders.isEmpty {
          FolderStrip(cell: 44, shown: shownFolders)
            .padding(.horizontal, inset)
            .padding(.vertical, 8)
        }
        ScrollViewReader { proxy in
          ScrollView {
            LazyVGrid(
              columns: Array(repeating: GridItem(.fixed(cell), spacing: spacing), count: columns),
              spacing: spacing
            ) {
              if foldersOnly {
                ForEach(shownFolders, id: \.self) { index in
                  // A stale row of a list that just shrank: see FolderStrip.
                  if store.folders.indices.contains(index) {
                    FolderTile(
                      index: index, path: store.folders[index], size: cell,
                      isCursor: index == store.folderCursor,
                      card: store.card(for: store.folders[index])
                    )
                    .id("folder-\(index)")
                    .onTapGesture { store.openFolder(at: index) }
                  }
                }
              }
              if !store.names.isEmpty {
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
                    // docs/design/25 PR 59: the add-ons' context-menu entries, only when some exist.
                    .modifier(SlotContextMenu(on: tileFlags.context, tile: index))
                    // Drag out the original (the range or the marks, when this
                    // cell is in them). Started by GalleryDrag's event monitor:
                    // the grid's scroll view keeps a SwiftUI drag from reaching
                    // the cell (#413).
                    .onHover { inside in GalleryDrag.shared.hover(index, inside: inside) }
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
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity)
                .padding(.top, 60)
            }
          }
          // In a child that observes only the selection: an arrow key re-runs
          // it, not this body (issue #179).
          .background {
            FollowSelection(proxy: proxy, anchor: nil, onFolders: store.folderCursor >= 0)
          }
          .onChange(of: store.folderCursor) { _, cursor in
            if foldersOnly && cursor >= 0 {
              proxy.scrollTo("folder-\(cursor)", anchor: .center)
            } else if cursor < 0, store.currentIndex >= 0 {
              proxy.scrollTo(store.currentIndex)
            }
          }
          .onAppear {
            if foldersOnly && store.folderCursor >= 0 {
              proxy.scrollTo("folder-\(store.folderCursor)", anchor: .center)
            }
          }
        }
      }
      .onAppear { mv_chrome_set_gallery_columns(Int32(columns)) }
      .onChange(of: columns) { _, new in mv_chrome_set_gallery_columns(Int32(new)) }
    }
  }
}

/// One row of folders above the photos. Big tiles stay for a folder that holds
/// only folders; a mixed folder keeps the pictures in the grid and the folders
/// in this strip.
private struct FolderStrip: View {
  @ObservedObject private var store = FolderStore.shared
  let cell: CGFloat
  /// The folder indices file search shows (all of them when off).
  let shown: [Int]

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 8) {
          ForEach(shown, id: \.self) { index in
            // SwiftUI can re-run a row of the old list after the list shrank
            // (a search or a person swapping the folder out): skip it, never
            // index past the end.
            if store.folders.indices.contains(index) {
              FolderChip(
                index: index, path: store.folders[index],
                isCursor: index == store.folderCursor,
                card: store.card(for: store.folders[index])
              )
              .id("chip-\(index)")
              .onTapGesture { store.openFolder(at: index) }
            }
          }
        }
      }
      .onChange(of: store.folderCursor) { _, cursor in
        guard cursor >= 0 else { return }
        proxy.scrollTo("chip-\(cursor)", anchor: .center)
      }
      .onAppear {
        if store.folderCursor >= 0 {
          proxy.scrollTo("chip-\(store.folderCursor)", anchor: .center)
        }
      }
    }
    .frame(height: cell + 36)
  }
}

/// A child folder: its first photo as a cover (from the folder itself or, for a
/// year of month folders, the first month that has one), name, and a count.
private struct FolderTile: View {
  let index: Int
  let path: String
  let size: CGFloat
  let isCursor: Bool
  @ObservedObject var card: FolderCard

  var body: some View {
    VStack(spacing: 4) {
      Color.clear
        .frame(width: size, height: size)
        .overlay {
          if let cover = card.cover {
            Image(decorative: cover, scale: 1)
              .resizable()
              .aspectRatio(contentMode: .fill)
          } else {
            ZStack {
              Rectangle().fill(.quaternary)
              Image(systemName: "folder.fill")
                .font(.system(size: size * 0.32))
                .foregroundStyle(.secondary)
            }
          }
        }
        .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
        .overlay(alignment: .bottomLeading) {
          if card.loaded {
            Text(card.status)
              .font(MVTheme.font(11))
              .padding(.horizontal, 6)
              .padding(.vertical, 2)
              .background(.ultraThinMaterial, in: Capsule())
              .padding(6)
          }
        }
        .overlay(
          RoundedRectangle(cornerRadius: MVTheme.radius(6))
            .strokeBorder(isCursor ? MVTheme.accent : .clear, lineWidth: 2)
        )
      HStack(spacing: 4) {
        Image(systemName: "folder").font(MVTheme.font(12)).foregroundStyle(.secondary)
        Text(FolderStore.shared.folderName(path))
          .font(MVTheme.font(12))
          .lineLimit(1)
          .truncationMode(.middle)
      }
      .frame(width: size)
    }
    .onAppear { FolderStore.shared.requestFolderSummaryIfNeeded(at: index) }
  }
}

/// A folder in the mixed-folder strip: a small cover, the name, and whether
/// photos were found further down.
private struct FolderChip: View {
  let index: Int
  let path: String
  let isCursor: Bool
  @ObservedObject var card: FolderCard

  var body: some View {
    HStack(spacing: 6) {
      Color.clear
        .frame(width: 36, height: 36)
        .overlay {
          if let cover = card.cover {
            Image(decorative: cover, scale: 1).resizable().aspectRatio(contentMode: .fill)
          } else {
            Image(systemName: "folder.fill").foregroundStyle(.secondary)
          }
        }
        .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(4)))
      VStack(alignment: .leading, spacing: 1) {
        Text(FolderStore.shared.folderName(path))
          .font(MVTheme.font(12))
          .lineLimit(1)
        if card.loaded {
          Text(card.status)
            .font(MVTheme.font(11))
            .foregroundStyle(.secondary)
            .lineLimit(1)
        }
      }
      .frame(width: 120, alignment: .leading)
    }
    .padding(4)
    .background(
      RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .fill(isCursor ? MVTheme.accent.opacity(0.18) : Color.primary.opacity(0.04))
    )
    .overlay(
      RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .strokeBorder(isCursor ? MVTheme.accent : .clear, lineWidth: 2)
    )
    .onAppear { FolderStore.shared.requestFolderSummaryIfNeeded(at: index) }
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

  var body: some View {
    VStack(spacing: 4) {
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
        // Issue #413: the shift-click range is tinted and outlined.
        .overlay {
          if highlight.inRange {
            RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(MVTheme.accent.opacity(0.22))
          }
        }
        .overlay(
          RoundedRectangle(cornerRadius: MVTheme.radius(6))
            .strokeBorder((highlight.isCurrent && !onFolders) || highlight.inRange ? MVTheme.accent : .clear,
                          lineWidth: 2)
        )
        .overlay { if isClip { PlayBadge(diameter: 40) } }
        .overlay(alignment: .topTrailing) { if isMarked { MarkBadge() } }
        // docs/design/25 PR 59: the add-ons' badges, from a table (no add-on call per
        // tile); with no add-on contributing badges the cell is exactly the plain one.
        .modifier(SlotBadgesOverlay(on: slotBadges, name: name))
      Text(name)
        .font(MVTheme.font(12))
        .lineLimit(1)
        .truncationMode(.middle)
        .frame(width: size)
      if let why {
        Text(why)
          .font(.caption2)
          .foregroundStyle(.secondary)
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

