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
  @State private var loaded: UIImage?
  /// The thumbnail could not be had: a quiet glyph says what the tile is instead of an empty square.
  @State private var missing = false

  var body: some View {
    let shown = loaded ?? images.cachedImage(for: entry.item.id)
    Theme.surface
      .aspectRatio(1, contentMode: .fit)
      .overlay {
        if let shown {
          Image(uiImage: shown).resizable().interpolation(.medium).scaledToFill()
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
      .task(id: entry.item.id) {
        missing = false
        loaded = nil
        if images.cachedImage(for: entry.item.id) == nil {
          loaded = await images.placeholder(for: entry.item)
        }
        if let sharper = await images.thumbnail(for: entry.item) { loaded = sharper }
        if loaded == nil, images.cachedImage(for: entry.item.id) == nil, !Task.isCancelled { missing = true }
      }
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
              ForEach(section.range, id: \.self) { position in tile(entries[position]) }
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
        if let position = Int(focus.dropFirst(context.count + 1)) { proxy.scrollTo(position, anchor: .center) }
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
    .id(entry.position)
    .modifier(TransitionSource(id: source, namespace: namespace))
  }

  private func open(_ entry: GridEntry) {
    model.open(ViewerRequest(context: context, entries: entries, startIndex: entry.position,
                             playClip: entry.item.kind == .clip),
               reduceMotion: reduceMotion)
  }
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
            Image(systemName: "xmark").font(.system(size: 12, weight: .semibold)).foregroundStyle(Theme.body)
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
