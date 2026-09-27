// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 18's gallery (plan/10-roadmap.md, folded-in Windows PR 4, plan/12
// 2026-09-17): "a full-client thumbnail grid... a click opens it in the
// viewer" (plan/16-commands.md's `G` row). `G` toggles visibility and `Esc`
// closes it (main_mac.mm's keyDown:, checked ahead of the fallback
// Esc-closes-window case). Up/Down/W/S move by row, `+`/`-` resize the cells
// (also main_mac.mm's keyDown:); this view lays out the grid and reports how
// many cells sit in a row (mv_chrome_set_gallery_columns) so the host can do
// that arithmetic.
//
// The search bar (plan/17 "Gallery search bar", 2026-09-27) is pinned above
// the tiles (GallerySearch.swift). Its Names filter only chooses which tiles
// are drawn: each tile keeps its identity (its index in the listing) and its
// thumbnail slot, so filtering reloads nothing and a tile that stays shown is
// not rebuilt.
import MVChromeBridge
import SwiftUI

struct GalleryView: View {
  @ObservedObject private var store = FolderStore.shared
  @ObservedObject private var search = GallerySearchStore.shared
  private let spacing: CGFloat = 8
  private let inset: CGFloat = 12

  var body: some View {
    grid
      // A folder of folders has no image to show through the material, and the
      // canvas behind may still hold the previous folder's last photo.
      .background(store.names.isEmpty ? AnyShapeStyle(Color(nsColor: .windowBackgroundColor))
                                      : AnyShapeStyle(.regularMaterial))
  }

  private var grid: some View {
    GeometryReader { geo in
      let cell = store.galleryCellSize
      let columns = max(1, Int((geo.size.width - 2 * inset + spacing) / (cell + spacing)))
      let mixed = !store.folders.isEmpty && !store.names.isEmpty
      let foldersOnly = !store.folders.isEmpty && store.names.isEmpty
      // The layout (strip or big tiles) follows the folder, not the filter,
      // so typing never flips it; the filter picks the tiles inside it.
      let shownItems = search.items ?? Array(store.names.indices)
      let shownFolders = search.folders ?? Array(store.folders.indices)
      let filteredEmpty = search.items != nil && shownItems.isEmpty && shownFolders.isEmpty
      let contentsNotice = search.contentsMode && search.active &&
        [.nothing, .failed, .notIndexed].contains(search.contents)
      VStack(spacing: 0) {
        GallerySearchBar()
          .padding(.horizontal, inset)
          .padding(.top, 10)
        if (mixed || foldersOnly), let query = store.folderQuery {
          Text(query.isEmpty ? "Find folder" : "Find folder: \(query)")
            .font(.callout.weight(.medium))
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, inset)
            .padding(.top, 8)
        }
        if mixed && !shownFolders.isEmpty && !contentsNotice {
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
              if foldersOnly && !contentsNotice {
                ForEach(shownFolders, id: \.self) { index in
                  FolderTile(
                    index: index, path: store.folders[index], size: cell,
                    isCursor: index == store.folderCursor,
                    card: store.card(for: store.folders[index])
                  )
                  .id("folder-\(index)")
                  .onTapGesture { store.openFolder(at: index) }
                }
              }
              if !store.names.isEmpty && !contentsNotice {
                ForEach(shownItems, id: \.self) { index in
                  GalleryCell(
                    index: index, name: store.names[index], size: cell,
                    isCurrent: index == store.currentIndex && store.folderCursor < 0,
                    isMarked: store.markedNames.contains(store.names[index]),
                    slot: store.slot(for: store.names[index])
                  )
                  .id(index)
                  .onTapGesture { store.selectAndCloseGallery(index) }
                }
              }
            }
            .padding(inset)
            .frame(maxWidth: .infinity)

            if filteredEmpty || contentsNotice {
              GallerySearchNotice()
            } else if store.folders.isEmpty && store.names.isEmpty {
              Text("No supported photos or videos in this folder")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity)
                .padding(.top, 60)
            }
          }
          .onChange(of: store.currentIndex) { _, newIndex in
            guard newIndex >= 0, store.folderCursor < 0 else { return }
            proxy.scrollTo(newIndex)
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
  /// The folder indices the search bar's filter shows (all of them when off).
  let shown: [Int]

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 8) {
          ForEach(shown, id: \.self) { index in
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
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .overlay(alignment: .bottomLeading) {
          if card.loaded {
            Text(card.status)
              .font(.caption2.weight(.medium))
              .padding(.horizontal, 6)
              .padding(.vertical, 2)
              .background(.ultraThinMaterial, in: Capsule())
              .padding(6)
          }
        }
        .overlay(
          RoundedRectangle(cornerRadius: 6)
            .strokeBorder(isCursor ? Color.accentColor : .clear, lineWidth: 2)
        )
      HStack(spacing: 4) {
        Image(systemName: "folder").font(.caption).foregroundStyle(.secondary)
        Text(FolderStore.shared.folderName(path))
          .font(.caption)
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
        .clipShape(RoundedRectangle(cornerRadius: 4))
      VStack(alignment: .leading, spacing: 1) {
        Text(FolderStore.shared.folderName(path))
          .font(.caption)
          .lineLimit(1)
        if card.loaded {
          Text(card.status)
            .font(.caption2)
            .foregroundStyle(.secondary)
            .lineLimit(1)
        }
      }
      .frame(width: 120, alignment: .leading)
    }
    .padding(4)
    .background(
      RoundedRectangle(cornerRadius: 6)
        .fill(isCursor ? Color.accentColor.opacity(0.18) : Color.primary.opacity(0.04))
    )
    .overlay(
      RoundedRectangle(cornerRadius: 6)
        .strokeBorder(isCursor ? Color.accentColor : .clear, lineWidth: 2)
    )
    .onAppear { FolderStore.shared.requestFolderSummaryIfNeeded(at: index) }
  }
}

private struct GalleryCell: View {
  let index: Int
  let name: String
  let size: CGFloat
  let isCurrent: Bool
  let isMarked: Bool
  @ObservedObject var slot: ThumbSlot

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
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .overlay(
          RoundedRectangle(cornerRadius: 6)
            .strokeBorder(isCurrent ? Color.accentColor : .clear, lineWidth: 2)
        )
        .overlay(alignment: .topTrailing) { if isMarked { MarkBadge() } }
      Text(name)
        .font(.caption)
        .lineLimit(1)
        .truncationMode(.middle)
        .frame(width: size)
    }
    .onAppear { FolderStore.shared.requestThumbnailIfNeeded(at: index) }
    // Cells are identified by index: a new listing (a result list replacing
    // the folder) reuses them without a fresh onAppear, so ask again.
    .onChange(of: name) { _, _ in FolderStore.shared.requestThumbnailIfNeeded(at: index) }
  }
}
