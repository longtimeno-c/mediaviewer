// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Folders as tiles, as the desktop's folder tiles are (#698): one big picture and two small ones from
// inside, the name under them, and what the folder holds. A folder of folders is a page of tiles;
// a folder with photos too shows its folders as one row above them.
import MobileKit
import SwiftUI

/// The page for a folder that holds only folders (and the top of a library in file mode).
struct FolderGrid: View {
  let folders: [Folder]
  @ScaledMetric(relativeTo: .subheadline) private var tileWidth: CGFloat = 150

  var body: some View {
    LazyVGrid(columns: [GridItem(.adaptive(minimum: tileWidth), spacing: 12, alignment: .top)],
              alignment: .leading, spacing: 20) {
      ForEach(folders) { FolderTile(folder: $0) }
    }
    .padding(.horizontal, Theme.inset)
    .padding(.top, 8)
    .padding(.bottom, 24)
  }
}

/// A folder's folders above its photos: one row that scrolls sideways.
struct FolderShelf: View {
  let folders: [Folder]
  @ScaledMetric(relativeTo: .subheadline) private var tileWidth: CGFloat = 140

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(folders.count == 1 ? "1 folder" : "\(folders.count.formatted()) folders")
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(Theme.title)
        .padding(.horizontal, Theme.inset)
        .accessibilityAddTraits(.isHeader)
      ScrollView(.horizontal, showsIndicators: false) {
        LazyHStack(alignment: .top, spacing: 12) {
          ForEach(folders) { FolderTile(folder: $0).frame(width: tileWidth) }
        }
        .padding(.horizontal, Theme.inset)
      }
      .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
    }
    .padding(.top, 8)
  }
}

struct FolderTile: View {
  let folder: Folder
  @Environment(AppModel.self) private var model
  /// Asked for once the tile is on screen; a folder already seen paints its preview at once.
  @State private var asked: FolderPreview?

  var body: some View {
    let preview = asked ?? model.library.knownPreview(of: folder)
    Button { model.library.enter(folder) } label: {
      VStack(alignment: .leading, spacing: 6) {
        FolderCover(items: preview?.items ?? [], known: preview != nil, offline: folder.offline,
                    images: model.services.images)
          .aspectRatio(4 / 3, contentMode: .fit)
          .clipShape(RoundedRectangle(cornerRadius: Theme.cardRadius, style: .continuous))
        VStack(alignment: .leading, spacing: 1) {
          Text(folder.name)
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(folder.offline ? Theme.body : Theme.title)
            .lineLimit(2)
            .multilineTextAlignment(.leading)
          if let line = detail(preview) {
            Text(line)
              .font(.caption.monospacedDigit())
              .foregroundStyle(Theme.body)
              .lineLimit(1)
          }
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .contentShape(Rectangle())
    }
    .buttonStyle(TileButtonStyle())
    .accessibilityElement(children: .ignore)
    .accessibilityLabel([folder.name, detail(preview)].compactMap { $0 }.joined(separator: ", "))
    .accessibilityAddTraits(.isButton)
    .accessibilityHint("Opens the folder")
    .task(id: folder.id) {
      guard asked == nil, model.library.knownPreview(of: folder) == nil, !folder.offline else { return }
      let got = await model.library.preview(of: folder)
      if !Task.isCancelled { asked = got }
    }
  }

  /// "1,204 items" from a catalogue; "12 folders · 48 items" from a folder listed in file mode.
  private func detail(_ preview: FolderPreview?) -> String? {
    if folder.offline { return FailureCopy.notConnected }
    if folder.itemCount >= 0 { return Self.count(folder.itemCount, "item") }
    guard let preview, let files = preview.files, let folders = preview.folders else { return nil }
    let parts = [folders > 0 ? Self.count(folders, "folder") : nil, files > 0 ? Self.count(files, "item") : nil].compactMap { $0 }
    return parts.isEmpty ? "Empty" : parts.joined(separator: " · ")
  }

  private static func count(_ n: Int, _ noun: String) -> String {
    n == 1 ? "1 \(noun)" : "\(n.formatted()) \(noun)s"
  }
}

/// One big picture and two small ones; one picture fills it; none leaves the folder's glyph.
private struct FolderCover: View {
  let items: [MediaItem]
  /// The preview has come: an empty one shows the glyph, one still coming shows the plain fill.
  let known: Bool
  let offline: Bool
  let images: ImageLadder

  var body: some View {
    GeometryReader { space in
      let gap: CGFloat = 2
      let side = (space.size.height - gap) / 2
      if items.count >= 3 {
        HStack(spacing: gap) {
          CoverPicture(item: items[0], images: images)
          VStack(spacing: gap) {
            CoverPicture(item: items[1], images: images)
            CoverPicture(item: items[2], images: images)
          }
          .frame(width: side)
        }
      } else if let first = items.first {
        CoverPicture(item: first, images: images)
      } else {
        Theme.surface.overlay {
          if known || offline {
            Image(systemName: offline ? "externaldrive.badge.xmark" : "folder.fill")
              .font(.title2)
              .foregroundStyle(Theme.body.opacity(0.7))
          }
        }
      }
    }
    .background(Theme.surface)
    .accessibilityHidden(true)
  }
}

/// A cover's picture: from memory in its first frame, else when the grid-sized one comes.
private struct CoverPicture: View {
  let item: MediaItem
  let images: ImageLadder
  @State private var loaded: (id: MediaID, image: UIImage)?
  /// No picture could be had (a format this phone cannot draw): the tile's glyph instead.
  @State private var missing: MediaID?

  var body: some View {
    let shown = loaded?.id == item.id ? loaded?.image : images.cachedImage(for: item.id)
    Theme.lifted
      .overlay {
        if let shown {
          Image(uiImage: shown).resizable().interpolation(.medium).scaledToFill()
        } else if missing == item.id {
          Image(systemName: item.kind == .clip ? "film" : "photo").foregroundStyle(Theme.body.opacity(0.6))
        }
      }
      .clipped()
      .task(id: item.id) {
        guard images.cachedImage(for: item.id) == nil else { return }
        if let image = await images.thumbnail(for: item) {
          if !Task.isCancelled { loaded = (item.id, image) }
        } else if !Task.isCancelled {
          missing = item.id
        }
      }
  }
}
