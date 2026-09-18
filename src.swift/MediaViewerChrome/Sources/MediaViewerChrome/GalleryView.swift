// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's gallery (plan/10-roadmap.md, folded-in Windows PR 4, plan/12
// 2026-09-17): "a full-client thumbnail grid... a click opens it in the
// viewer" (plan/16-commands.md's `G` row). `G` toggles visibility and `Esc`
// closes it (main_mac.mm's keyDown:, checked ahead of the fallback
// Esc-closes-window case) -- this view only draws and reacts to clicks, it
// doesn't own its own visibility state.
import SwiftUI

struct GalleryView: View {
  @ObservedObject private var store = FolderStore.shared
  private let columns = [GridItem(.adaptive(minimum: 140, maximum: 220), spacing: 8)]

  var body: some View {
    ScrollView {
      // LazyVGrid: same "don't build 2000 cells up front" reasoning as
      // FilmstripView's LazyHStack -- the gallery is the one place all 2000
      // items are visible at once in principle, so laziness matters even
      // more here.
      LazyVGrid(columns: columns, spacing: 8) {
        ForEach(0..<store.itemCount, id: \.self) { index in
          GalleryCell(index: index)
            .onTapGesture { store.selectAndCloseGallery(index) }
        }
      }
      .padding(12)
    }
    .background(.regularMaterial)
  }
}

private struct GalleryCell: View {
  let index: Int
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    VStack(spacing: 4) {
      ZStack {
        if let image = store.thumbnails[index] {
          Image(nsImage: image)
            .resizable()
            .aspectRatio(contentMode: .fit)
        } else {
          Rectangle().fill(.quaternary)
        }
      }
      .aspectRatio(1, contentMode: .fit)
      .clipShape(RoundedRectangle(cornerRadius: 6))
      .overlay(
        RoundedRectangle(cornerRadius: 6)
          .strokeBorder(index == store.currentIndex ? Color.accentColor : .clear, lineWidth: 2)
      )
      Text(store.name(at: index))
        .font(.caption)
        .lineLimit(1)
        .truncationMode(.middle)
    }
    .onAppear { store.requestThumbnailIfNeeded(at: index) }
  }
}
