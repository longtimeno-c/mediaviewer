// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's gallery (plan/10-roadmap.md, folded-in Windows PR 4, plan/12
// 2026-09-17): "a full-client thumbnail grid... a click opens it in the
// viewer" (plan/16-commands.md's `G` row). `G` toggles visibility and `Esc`
// closes it (main_mac.mm's keyDown:, checked ahead of the fallback
// Esc-closes-window case). Up/Down/W/S move by row, `+`/`-` resize the cells
// (also main_mac.mm's keyDown:); this view lays out the grid and reports how
// many cells sit in a row (mv_chrome_set_gallery_columns) so the host can do
// that arithmetic.
import MVChromeBridge
import SwiftUI

struct GalleryView: View {
  @ObservedObject private var store = FolderStore.shared
  private let spacing: CGFloat = 8
  private let inset: CGFloat = 12

  var body: some View {
    VStack(spacing: 0) {
      if !store.crumbs.isEmpty { BreadcrumbBar() }
      grid
    }
    // A folder of folders has no image to show through the material, and the
    // canvas behind may still hold the previous folder's last photo.
    .background(store.names.isEmpty ? AnyShapeStyle(Color(nsColor: .windowBackgroundColor))
                                    : AnyShapeStyle(.regularMaterial))
  }

  private var grid: some View {
    GeometryReader { geo in
      let cell = store.galleryCellSize
      let columns = max(1, Int((geo.size.width - 2 * inset + spacing) / (cell + spacing)))
      let both = !store.folders.isEmpty && !store.names.isEmpty
      ScrollViewReader { proxy in
        ScrollView {
          // LazyVGrid: same "don't build 2000 cells up front" reasoning as
          // FilmstripView's LazyHStack. A fixed column count (not .adaptive)
          // so the host knows exactly how many cells are in a row; folder tiles
          // and images share it, so Up/Down land in the same column.
          LazyVGrid(
            columns: Array(repeating: GridItem(.fixed(cell), spacing: spacing), count: columns),
            spacing: spacing
          ) {
            if !store.folders.isEmpty {
              Section {
                ForEach(store.folders.indices, id: \.self) { index in
                  FolderTile(
                    index: index, path: store.folders[index], size: cell,
                    isCursor: index == store.folderCursor,
                    card: store.card(for: store.folders[index])
                  )
                  .id("folder-\(index)")
                  .onTapGesture { store.openFolder(at: index) }
                }
              } header: {
                if both { SectionLabel(title: "Folders", count: store.folders.count) }
              }
            }
            if !store.names.isEmpty {
              Section {
                ForEach(store.names.indices, id: \.self) { index in
                  GalleryCell(
                    index: index, name: store.names[index], size: cell,
                    isCurrent: index == store.currentIndex && store.folderCursor < 0,
                    isMarked: store.markedNames.contains(store.names[index]),
                    slot: store.slot(for: store.names[index])
                  )
                  .id(index)
                  .onTapGesture { store.selectAndCloseGallery(index) }
                }
              } header: {
                if both { SectionLabel(title: "Photos and videos", count: store.names.count) }
              }
            }
          }
          .padding(inset)
          .frame(maxWidth: .infinity)

          if store.folders.isEmpty && store.names.isEmpty {
            Text("No supported photos or videos in this folder")
              .foregroundStyle(.secondary)
              .frame(maxWidth: .infinity)
              .padding(.top, 60)
          }
        }
        // Row-wise keyboard movement must keep the selection on screen.
        .onChange(of: store.currentIndex) { _, newIndex in
          guard newIndex >= 0, store.folderCursor < 0 else { return }
          proxy.scrollTo(newIndex)
        }
        .onChange(of: store.folderCursor) { _, cursor in
          if cursor >= 0 {
            proxy.scrollTo("folder-\(cursor)")
          } else if store.currentIndex >= 0 {
            proxy.scrollTo(store.currentIndex)
          }
        }
        .onChange(of: store.galleryCellSize) { _, _ in
          if store.folderCursor >= 0 {
            proxy.scrollTo("folder-\(store.folderCursor)", anchor: .center)
          } else if store.currentIndex >= 0 {
            proxy.scrollTo(store.currentIndex, anchor: .center)
          }
        }
      }
      .onAppear { mv_chrome_set_gallery_columns(Int32(columns)) }
      .onChange(of: columns) { _, new in mv_chrome_set_gallery_columns(Int32(new)) }
    }
  }
}

/// Up button and the trail from the highest folder reached to the one on
/// screen. Every crumb but the last is a button.
private struct BreadcrumbBar: View {
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    HStack(spacing: 6) {
      Button { store.navigateUp() } label: {
        Image(systemName: "chevron.up")
      }
      .buttonStyle(.borderless)
      .disabled(!store.canGoUp)
      .help("Up one folder (\u{2318}\u{2191})")

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 4) {
          ForEach(store.crumbs) { crumb in
            if crumb.index > 0 {
              Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.tertiary)
            }
            if crumb.index == store.crumbs.count - 1 {
              Text(crumb.name).fontWeight(.semibold).lineLimit(1)
            } else {
              Button(crumb.name) { store.openCrumb(crumb.index) }
                .buttonStyle(.borderless)
                .lineLimit(1)
            }
          }
        }
      }
      Spacer(minLength: 0)
    }
    .font(.callout)
    .padding(.horizontal, 12)
    .padding(.vertical, 8)
    .background(.bar)
  }
}

private struct SectionLabel: View {
  let title: String
  let count: Int

  var body: some View {
    HStack {
      Text("\(title)  \(count)").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
      Spacer()
    }
    .padding(.top, 6)
    .padding(.horizontal, 2)
    .frame(maxWidth: .infinity)
    .background(.clear)
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
            Text(countLabel)
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

  private var countLabel: String {
    switch (card.mediaCount, card.subfolderCount) {
    case (0, 0): return "Empty"
    case (let m, 0): return m == 1 ? "1 item" : "\(m) items"
    case (0, let f): return f == 1 ? "1 folder" : "\(f) folders"
    case (let m, let f): return "\(m) items, \(f) folders"
    }
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
  }
}
