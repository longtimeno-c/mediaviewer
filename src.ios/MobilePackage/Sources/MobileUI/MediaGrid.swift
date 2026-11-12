// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The photo is the interface: no cards around thumbnails, 2 pt between them. One tile paints from
// memory in its first frame, then sharpens when the grid-sized picture arrives.
import MobileKit
import SwiftUI

extension String {
  static func clockFrom(_ seconds: TimeInterval) -> String {
    let total = Int(seconds.rounded(.down))
    let m = total / 60, s = total % 60
    if m >= 60 { return String(format: "%d:%02d:%02d", m / 60, m % 60, s) }
    return String(format: "%d:%02d", m, s)
  }
}

struct MediaTile: View {
  let entry: GridEntry
  let images: ImageLadder
  /// The picture this tile has had, and whose it is: a tile shows only its own item's picture.
  @State private var loaded: Loaded?
  /// The thumbnail could not be had: a quiet glyph says what the tile is instead of an empty square.
  @State private var missing = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  /// Whether the last drawing painted a picture. A box, not state: written while drawing, read
  /// when a picture arrives, and never a reason to draw again.
  @State private var drawn = Drawn()

  private final class Drawn { var painted = false }

  private struct Loaded {
    let id: MediaID
    let image: UIImage
  }

  /// What the tile paints now: its own picture, else the best one already in memory, so an item
  /// shown before (in this answer or an earlier one) paints in its first frame and never flashes.
  private var shown: UIImage? {
    if let loaded, loaded.id == entry.item.id { return loaded.image }
    return images.cachedImage(for: entry.item.id)
  }

  var body: some View {
    let shown = shown
    let _ = drawn.painted = shown != nil
    Theme.surface
      .aspectRatio(1, contentMode: .fit)
      .overlay {
        if let shown {
          Image(uiImage: shown).resizable().interpolation(.medium).scaledToFill()
            .transition(.opacity)
        } else if missing {
          Image(systemName: entry.item.kind == .clip ? "film" : "photo")
            .font(.title2)
            .foregroundStyle(Theme.body.opacity(0.6))
            .accessibilityHidden(true)
        }
      }
      .clipped()
      .overlay(alignment: .bottomTrailing) { badge }
      .contentShape(Rectangle())
      .task(id: entry.item.id) { await load() }
  }

  /// The ladder, off the main thread: the tile from the phone, then the sharper JPEG-512. Nothing
  /// is cleared first, so the tile keeps what it shows until something better arrives.
  private func load() async {
    let item = entry.item
    missing = false
    if !drawn.painted, let quick = await images.placeholder(for: item), !Task.isCancelled {
      show(quick, of: item.id)
    }
    if let sharper = await images.thumbnail(for: item), !Task.isCancelled {
      show(sharper, of: item.id)
    }
    if loaded?.id != item.id, !drawn.painted, !Task.isCancelled { missing = true }
  }

  /// Fades in only the first picture a tile gets; a sharper one replaces it in place.
  private func show(_ image: UIImage, of id: MediaID) {
    guard id == entry.item.id else { return }
    // Whether the tile already paints something: its own picture, or one the ladder had in
    // memory (no new ladder call; `shown` asked the same question when the tile was drawn).
    let first = loaded?.id != id && !drawn.painted
    var change = Transaction(animation: first && !reduceMotion ? .easeOut(duration: 0.18) : nil)
    change.disablesAnimations = !first || reduceMotion
    withTransaction(change) { loaded = Loaded(id: id, image: image) }
  }

  @ViewBuilder
  private var badge: some View {
    if entry.item.kind == .clip || entry.moment != nil {
      HStack(spacing: 3) {
        Image(systemName: "play.fill").font(.system(size: 8))
        if let label = badgeText { Text(label).font(.caption2.monospacedDigit().weight(.medium)) }
      }
      .foregroundStyle(.white)
      .padding(.horizontal, 5)
      .padding(.vertical, 2)
      .background(Capsule().fill(.black.opacity(0.55)))
      .padding(4)
      .dynamicTypeSize(...DynamicTypeSize.xLarge)
    }
  }

  private var badgeText: String? {
    if let moment = entry.moment { return .clockFrom(moment.seconds) }
    return entry.item.duration.map { .clockFrom($0) }
  }
}

struct MediaGrid<Header: View>: View {
  let sections: [GridSection]
  /// Everything in order, the viewer's list too.
  let entries: GridSource
  /// The name of this grid; a viewer closes onto a tile in the grid that opened it.
  let context: String
  /// Said in each tile's identifier, never spoken: the search screen names how many results there
  /// are and the words they answer ("40|mountains"), so a test can tell an answer from the one
  /// before it and count what is off screen.
  var answering: String = ""
  @ViewBuilder var header: Header

  @Environment(AppModel.self) private var model
  @Environment(\.viewerNamespace) private var namespace
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private let columns = [GridItem(.adaptive(minimum: 120), spacing: 2)]

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView {
        header
        LazyVGrid(columns: columns, spacing: 2) {
          ForEach(sections) { section in
            Section {
              if let ids = entries.ids {
                // Keyed by item: a tile follows its item from one answer to the next.
                ForEach(section.range.map { Slot(position: $0, id: ids[$0]) }) { slot in
                  tile(entries[slot.position]).id(slot.id)
                }
              } else {
                ForEach(section.range, id: \.self) { position in tile(entries[position]).id(position) }
              }
            } header: {
              if let title = section.title {
                Text(title)
                  .font(.subheadline.weight(.semibold))
                  .foregroundStyle(Theme.title)
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .padding(.horizontal, Theme.inset)
                  .padding(.top, 16)
                  .padding(.bottom, 8)
              }
            }
          }
        }
      }
      .onChange(of: model.viewerFocus) { _, focus in
        guard let focus, focus.hasPrefix(context + "/") else { return }
        guard let position = Int(focus.dropFirst(context.count + 1)) else { return }
        if let ids = entries.ids {
          if ids.indices.contains(position) { proxy.scrollTo(ids[position], anchor: .center) }
        } else {
          proxy.scrollTo(position, anchor: .center)
        }
      }
    }
    .background(Theme.ground)
  }

  @ViewBuilder
  private func tile(_ entry: GridEntry) -> some View {
    let source = "\(context)/\(entry.position)"
    Button {
      open(entry)
    } label: {
      MediaTile(entry: entry, images: model.services.images)
    }
    .buttonStyle(TileButtonStyle())
    .accessibilityLabel(entry.accessibilityName)
    // Never spoken: lets a test name which item a tile is (the live parity run).
    .accessibilityIdentifier(answering.isEmpty ? "tile:\(entry.item.id.raw)" : "tile:\(entry.item.id.raw)|\(answering)")
    .modifier(TransitionSource(id: source, namespace: namespace))
  }

  private func open(_ entry: GridEntry) {
    model.open(ViewerRequest(context: context, entries: entries, startIndex: entry.position,
                             playClip: entry.item.kind == .clip),
               reduceMotion: reduceMotion)
  }
}

private struct Slot: Identifiable {
  let position: Int
  let id: String
}

private struct TransitionSource: ViewModifier {
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

/// A wait past a second shows the content's own empty geometry.
struct GridSkeleton: View {
  var body: some View {
    GeometryReader { geo in
      let columns = max(3, Int((geo.size.width + 2) / 122))
      let side = (geo.size.width - CGFloat(columns - 1) * 2) / CGFloat(columns)
      let rows = Int(geo.size.height / (side + 2)) + 1
      VStack(spacing: 2) {
        ForEach(0..<rows, id: \.self) { _ in
          HStack(spacing: 2) {
            ForEach(0..<columns, id: \.self) { _ in Theme.surface.frame(width: side, height: side) }
          }
        }
      }
    }
    .accessibilityHidden(true)
  }
}

/// A card: one fill, words in from the edge by 12 pt, and a single action.
struct NoticeCard: View {
  let message: String
  var onDismiss: (() -> Void)? = nil
  var actionTitle: String? = nil
  var action: (() -> Void)? = nil

  private var parts: (head: String, rest: String?) {
    guard let range = message.range(of: ". "), message.index(after: range.lowerBound) != message.endIndex else {
      return (message, nil)
    }
    return (String(message[...range.lowerBound]), String(message[range.upperBound...]))
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack(alignment: .top, spacing: 8) {
        VStack(alignment: .leading, spacing: 4) {
          Text(parts.head).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title)
          if let rest = parts.rest {
            Text(rest).font(.footnote).foregroundStyle(Theme.body)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .fixedSize(horizontal: false, vertical: true)
        if let onDismiss {
          Button(action: onDismiss) {
            Image(systemName: "xmark").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
              .frame(width: 44, height: 44)
              .contentShape(Rectangle())
          }
          .padding(-12)
          .accessibilityLabel("Dismiss")
        }
      }
      if let actionTitle, let action {
        Button(actionTitle, action: action)
          .buttonStyle(SecondaryButtonStyle())
          .padding(.top, 12)
      }
    }
    .padding(Theme.inset)
    .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
    .accessibilityElement(children: .contain)
  }
}
