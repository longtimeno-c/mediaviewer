// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Folders as tiles: a folder drawn as one, with up to three pictures from inside peeking out of it
// (so a folder never reads as a photo), the name under it, and what the folder holds. A folder of folders is a page of tiles;
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
      .dropsHold()
      .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
    }
    .padding(.top, 8)
  }
}

struct FolderTile: View {
  let folder: Folder
  @Environment(AppModel.self) private var model
  @Environment(\.folderNamespace) private var namespace
  @Environment(\.editingPage) private var editingPage
  /// The page the tile is on: a tap on it then finds its folder listed (LibraryModel.prefetch).
  @Environment(\.shownPage) private var shownPage
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  /// Asked for once the tile is on screen; a folder already seen paints its preview at once.
  @State private var asked: FolderPreview?
  /// Which change of its folder `asked` was had after.
  @State private var askedVersion = 0

  var body: some View {
    let preview = asked ?? model.library.knownPreview(of: folder)
    Button { TileTap(page: editingPage, model: model).run(.folder(folder)) { model.library.enter(folder) } } label: {
      VStack(alignment: .leading, spacing: 6) {
        FolderCover(items: preview?.items ?? [], known: preview != nil, documents: preview?.others ?? 0,
                    offline: folder.offline, images: model.services.images)
          .aspectRatio(4 / 3, contentMode: .fit)
          .modifier(TileSource(id: folder.id, namespace: namespace))
        VStack(alignment: .leading, spacing: 1) {
          Text(folder.name)
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(folder.offline ? Theme.body : Theme.title)
            .lineLimit(2)
            .multilineTextAlignment(.leading)
          // Its line is held while a folder's counts are coming, so the tile never grows.
          Text(detail(preview) ?? " ")
            .font(.caption.monospacedDigit())
            .foregroundStyle(Theme.body)
            .lineLimit(1)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .contentShape(Rectangle())
    }
    .buttonStyle(TileButtonStyle())
    .accessibilityElement(children: .ignore)
    .accessibilityLabel([folder.name, detail(preview)].compactMap { $0 }.joined(separator: ", "))
    .accessibilityAddTraits(.isButton)
    .accessibilityHint(editingPage?.selecting == true ? "Chooses the folder" : "Opens the folder")
    .choosable(.folder(folder))
    // Never spoken: lets a test name which folder a tile is.
    .accessibilityIdentifier("folder:\(folder.id)")
    // Asked again when its folder changes (something added, moved, deleted).
    .task(id: "\(folder.id)#\(model.library.previewVersion[folder.id] ?? 0)") {
      if let shownPage { model.library.prefetch(folder, in: shownPage) }
      let version = model.library.previewVersion[folder.id] ?? 0
      guard !folder.offline, model.library.knownPreview(of: folder) == nil,
            asked == nil || askedVersion != version else { return }
      let got = await model.library.preview(of: folder)
      guard !Task.isCancelled else { return }
      askedVersion = version
      // Its pictures fade in, in the space already held for them.
      withAnimation(reduceMotion ? nil : .easeOut(duration: 0.2)) { asked = got }
    }
  }

  /// "1,204 items" from a catalogue; "12 folders · 48 items · 3 files" from a folder listed in file mode.
  private func detail(_ preview: FolderPreview?) -> String? {
    if folder.offline { return FailureCopy.notConnected }
    if folder.itemCount >= 0 {
      // A catalogue's: everything beneath, its documents and audio counted apart from its photos.
      let parts = [folder.itemCount > 0 || folder.fileCount == 0 ? Self.count(folder.itemCount, "item") : nil,
                   folder.fileCount > 0 ? Self.count(folder.fileCount, "file") : nil].compactMap { $0 }
      return parts.joined(separator: " · ")
    }
    guard let preview, let files = preview.files, let folders = preview.folders else { return nil }
    let others = preview.others ?? 0
    let parts = [folders > 0 ? Self.count(folders, "folder") : nil, files > 0 ? Self.count(files, "item") : nil,
                 others > 0 ? Self.count(others, "file") : nil].compactMap { $0 }
    return parts.isEmpty ? "Empty" : parts.joined(separator: " · ")
  }

  private static func count(_ n: Int, _ noun: String) -> String {
    n == 1 ? "1 \(noun)" : "\(n.formatted()) \(noun)s"
  }
}

/// The folder's page grows out of its picture (iOS 18's zoom). Not with Reduce Motion.
private struct TileSource: ViewModifier {
  let id: String
  let namespace: Namespace.ID?

  func body(content: Content) -> some View {
    if let namespace {
      content.matchedTransitionSource(id: id, in: namespace)
    } else {
      content
    }
  }
}

/// A folder, drawn as one: its back with a tab, up to three of its pictures (or sheets of paper, for
/// a folder of documents) peeking out of the top, and its front over them. Nothing peeks while the
/// preview is coming, so a tile never changes shape when it arrives.
private struct FolderCover: View {
  let items: [MediaItem]
  /// The preview has come.
  let known: Bool
  /// Other files directly inside (PDFs and the like): paper peeks out when there are no pictures.
  let documents: Int
  let offline: Bool
  let images: ImageLadder

  var body: some View {
    GeometryReader { space in
      let w = space.size.width, h = space.size.height
      let radius = min(10, w * 0.06)
      ZStack(alignment: .topLeading) {
        FolderBack(radius: radius, tabWidth: w * 0.38, tabHeight: h * 0.1)
          .fill(offline ? Theme.lifted : Theme.folderBack)
        // Its own stack, so the fan's order never lifts a picture over the folder's front.
        ZStack(alignment: .topLeading) { peeking(w: w, h: h) }
          .frame(width: w, height: h, alignment: .topLeading)
        UnevenRoundedRectangle(cornerRadii: .init(topLeading: radius * 0.6, bottomLeading: radius,
                                                  bottomTrailing: radius, topTrailing: radius * 0.6),
                               style: .continuous)
          .fill(offline ? Theme.surface : Theme.folderFront)
          .overlay {
            if offline {
              Image(systemName: "externaldrive.badge.xmark").font(.title3).foregroundStyle(Theme.body)
            }
          }
          .frame(width: w, height: h * 0.44)
          .offset(y: h * 0.56)
      }
      .frame(width: w, height: h, alignment: .topLeading)
    }
    .accessibilityHidden(true)
  }

  /// Fanned like prints in a wallet: the middle one in front, the outer two tilted away.
  @ViewBuilder
  private func peeking(w: CGFloat, h: CGFloat) -> some View {
    let card = CGSize(width: w * 0.4, height: h * 0.56)
    let shown = Array(items.prefix(3))
    let sheets = shown.isEmpty && known && documents > 0 ? min(3, documents) : 0
    let count = shown.isEmpty ? sheets : shown.count
    let layout = Self.fan(count)
    ForEach(Array(layout.enumerated()), id: \.offset) { order, place in
      Group {
        if shown.isEmpty {
          PaperSheet()
        } else {
          CoverPicture(item: shown[place.index], images: images)
        }
      }
      .frame(width: card.width, height: card.height)
      .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
      .padding(2)
      .background(RoundedRectangle(cornerRadius: 5, style: .continuous).fill(Theme.paper))
      .rotationEffect(.degrees(place.angle), anchor: .bottom)
      .position(x: w * (0.5 + place.x), y: h * 0.13 + card.height / 2)
      .zIndex(Double(order))
    }
  }

  private struct Place {
    let index: Int
    let x: CGFloat
    let angle: Double
  }

  /// Back to front.
  private static func fan(_ count: Int) -> [Place] {
    switch count {
    case 0: return []
    case 1: return [Place(index: 0, x: 0, angle: -3)]
    case 2: return [Place(index: 1, x: 0.12, angle: 6), Place(index: 0, x: -0.1, angle: -4)]
    default:
      return [Place(index: 1, x: -0.2, angle: -7), Place(index: 2, x: 0.2, angle: 7), Place(index: 0, x: 0, angle: 0)]
    }
  }
}

/// A folder's back: a tab on the left of its top edge.
private struct FolderBack: Shape {
  let radius: CGFloat
  let tabWidth: CGFloat
  let tabHeight: CGFloat

  func path(in rect: CGRect) -> Path {
    var p = Path()
    let r = radius, t = tabHeight
    p.move(to: CGPoint(x: 0, y: r))
    p.addQuadCurve(to: CGPoint(x: r, y: 0), control: .zero)
    p.addLine(to: CGPoint(x: tabWidth - t * 0.6, y: 0))
    p.addCurve(to: CGPoint(x: tabWidth + t * 0.6, y: t), control1: CGPoint(x: tabWidth, y: 0),
               control2: CGPoint(x: tabWidth, y: t))
    p.addLine(to: CGPoint(x: rect.maxX - r, y: t))
    p.addQuadCurve(to: CGPoint(x: rect.maxX, y: t + r), control: CGPoint(x: rect.maxX, y: t))
    p.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY - r))
    p.addQuadCurve(to: CGPoint(x: rect.maxX - r, y: rect.maxY), control: CGPoint(x: rect.maxX, y: rect.maxY))
    p.addLine(to: CGPoint(x: r, y: rect.maxY))
    p.addQuadCurve(to: CGPoint(x: 0, y: rect.maxY - r), control: CGPoint(x: 0, y: rect.maxY))
    p.closeSubpath()
    return p
  }
}

/// A sheet of a document peeking out: paper with a few lines of text on it.
private struct PaperSheet: View {
  var body: some View {
    GeometryReader { space in
      let w = space.size.width, h = space.size.height
      VStack(alignment: .leading, spacing: h * 0.07) {
        ForEach(0..<4, id: \.self) { line in
          Capsule().fill(Theme.paperInk)
            .frame(width: w * (line == 0 ? 0.5 : line == 3 ? 0.6 : 0.78), height: max(1.5, h * 0.035))
        }
      }
      .padding(.top, h * 0.14)
      .padding(.leading, w * 0.12)
      .frame(width: w, height: h, alignment: .topLeading)
      .background(Theme.paper)
    }
  }
}

/// A cover's picture: from memory in its first frame, else when the grid-sized one comes.
private struct CoverPicture: View {
  let item: MediaItem
  let images: ImageLadder
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var loaded: (id: MediaID, image: UIImage)?
  /// No picture could be had (a format this phone cannot draw): the tile's glyph instead.
  @State private var missing: MediaID?

  var body: some View {
    let shown = loaded?.id == item.id ? loaded?.image : images.cachedImage(for: item.id)
    Theme.lifted
      .overlay {
        if let shown {
          Image(uiImage: shown).resizable().interpolation(.medium).scaledToFill()
            .transition(.opacity)
        } else if missing == item.id {
          Image(systemName: item.kind == .clip ? "film" : "photo").foregroundStyle(Theme.body.opacity(0.6))
        }
      }
      .clipped()
      .task(id: item.id) {
        guard images.cachedImage(for: item.id) == nil else { return }
        if let image = await images.thumbnail(for: item) {
          if !Task.isCancelled {
            withAnimation(reduceMotion ? nil : .easeOut(duration: 0.2)) { loaded = (item.id, image) }
          }
        } else if !Task.isCancelled {
          missing = item.id
        }
      }
  }
}
