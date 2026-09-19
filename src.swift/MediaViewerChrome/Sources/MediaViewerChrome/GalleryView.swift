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
    GeometryReader { geo in
      let cell = store.galleryCellSize
      let columns = max(1, Int((geo.size.width - 2 * inset + spacing) / (cell + spacing)))
      ScrollViewReader { proxy in
        ScrollView {
          // LazyVGrid: same "don't build 2000 cells up front" reasoning as
          // FilmstripView's LazyHStack. A fixed column count (not .adaptive)
          // so the host knows exactly how many cells are in a row.
          LazyVGrid(
            columns: Array(repeating: GridItem(.fixed(cell), spacing: spacing), count: columns),
            spacing: spacing
          ) {
            ForEach(store.names.indices, id: \.self) { index in
              GalleryCell(
                index: index, name: store.names[index], size: cell,
                isCurrent: index == store.currentIndex,
                isMarked: store.markedNames.contains(store.names[index]),
                slot: store.slot(for: store.names[index])
              )
              .id(index)
              .onTapGesture { store.selectAndCloseGallery(index) }
            }
          }
          .padding(inset)
          .frame(maxWidth: .infinity)
        }
        // Row-wise keyboard movement must keep the selection on screen.
        .onChange(of: store.currentIndex) { _, newIndex in
          guard newIndex >= 0 else { return }
          proxy.scrollTo(newIndex)
        }
        .onChange(of: store.galleryCellSize) { _, _ in
          if store.currentIndex >= 0 { proxy.scrollTo(store.currentIndex, anchor: .center) }
        }
      }
      .onAppear { mv_chrome_set_gallery_columns(Int32(columns)) }
      .onChange(of: columns) { _, new in mv_chrome_set_gallery_columns(Int32(new)) }
    }
    .background(.regularMaterial)
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
