// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's filmstrip (plan/10-roadmap.md, folded-in Windows PR 4, plan/12
// 2026-09-17): a bottom strip over the same folder_model/JPEG-512 cache the
// gallery uses. `T` toggles it (main_mac.mm's keyDown:, which also toggles
// input_snapshot.chrome_bottom_px so the canvas reclaims the space when
// hidden) -- this view only draws, it doesn't decide its own visibility.
import SwiftUI

struct FilmstripView: View {
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView(.horizontal, showsIndicators: false) {
        // LazyHStack, not HStack: PR 18's verify line asks for 2000 mixed
        // JPEGs to scroll "without a hitch" -- eagerly building 2000 cells
        // (and requesting 2000 thumbnails) up front is exactly the hitch.
        LazyHStack(spacing: 4) {
          ForEach(store.names.indices, id: \.self) { index in
            FilmstripCell(
              index: index, name: store.names[index], isCurrent: index == store.currentIndex,
              isMarked: store.markedNames.contains(store.names[index]),
              slot: store.slot(for: store.names[index])
            )
            .id(index)
            .onTapGesture { store.select(index) }
          }
        }
        .padding(.horizontal, 8)
        .frame(height: 88)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      .background(.regularMaterial)
      // Keeps the current selection in view across arrow-key navigation, not
      // just mouse clicks inside the strip. Deliberately not animated: an
      // animation per key-repeat queued up behind each other and read as lag.
      .onChange(of: store.currentIndex) { _, newIndex in
        guard newIndex >= 0 else { return }
        proxy.scrollTo(newIndex, anchor: .center)
      }
    }
  }
}

private struct FilmstripCell: View {
  let index: Int
  let name: String
  let isCurrent: Bool
  let isMarked: Bool
  // Observed per cell: a thumbnail arriving re-renders this cell only.
  @ObservedObject var slot: ThumbSlot

  var body: some View {
    ZStack {
      if let image = slot.image {
        Image(decorative: image, scale: 1)
          .resizable()
          .aspectRatio(contentMode: .fit)
      } else {
        Rectangle().fill(.quaternary)
      }
    }
    .frame(width: 78, height: 78)
    .clipShape(RoundedRectangle(cornerRadius: 4))
    .overlay(
      RoundedRectangle(cornerRadius: 4)
        .strokeBorder(isCurrent ? Color.accentColor : .clear, lineWidth: 2)
    )
    .overlay(alignment: .topTrailing) { if isMarked { MarkBadge() } }
    .onAppear { FolderStore.shared.requestThumbnailIfNeeded(at: index) }
  }
}
