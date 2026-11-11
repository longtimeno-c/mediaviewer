// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Library (date-ordered grid, folder switch, the route chip only when it matters), People.
import MobileKit
import SwiftUI

struct LibraryScreen: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    content
      .safeAreaInset(edge: .top, spacing: 0) { PlaceBar() }
      .background(Theme.ground)
  }

  @ViewBuilder
  private var content: some View {
    if model.library.isLoading {
      GridSkeleton().background(Theme.ground)
    } else if model.library.sections.isEmpty {
      // Centred while it fits; scrolls at the largest text sizes rather than cut its words short.
      GeometryReader { space in
        ScrollView {
          VStack(spacing: 0) {
            noticeSlot
            emptyWords.frame(maxHeight: .infinity)
          }
          .frame(maxWidth: .infinity, minHeight: space.size.height)
        }
        .scrollBounceBehavior(.basedOnSize)
      }
      .background(Theme.ground)
    } else {
      MediaGrid(sections: model.library.sections, entries: model.library.entries, context: "library") { noticeSlot }
        // A new folder starts at its top, not at the last folder's scroll position.
        .id(model.library.scope)
    }
  }

  private var emptyWords: some View {
    Text(model.library.failure != nil ? "" : model.library.scope == .everything
         ? "Nothing here yet. Photos appear once MediaViewer on your computer publishes them."
         : "This folder has no photos or clips.")
      .font(.subheadline)
      .foregroundStyle(Theme.body)
      .multilineTextAlignment(.center)
      .fixedSize(horizontal: false, vertical: true)
      .padding(24)
  }

  @ViewBuilder
  private var noticeSlot: some View {
    if let failure = model.library.failure {
      NoticeCard(message: failure, actionTitle: "Try again") { Task { await model.library.load() } }
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
        .arrives(reduceMotion: reduceMotion)
    }
    if let notice = model.libraryNotice {
      NoticeCard(message: notice.message,
                 onDismiss: notice.canDismiss ? {
                   withAnimation(reduceMotion ? nil : .easeOut(duration: 0.15)) { model.dismiss(notice) }
                 } : nil,
                 actionTitle: notice.action == .scan ? "Scan code" : nil,
                 action: notice.action == .scan ? { model.isScanning = true } : nil)
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
        .arrives(reduceMotion: reduceMotion)
    }
  }
}

/// Fixed at the top while the photos scroll under it: where you are, each step of it a way back,
/// and the folders you can go into from here.
private struct PlaceBar: View {
  private static let pathEnd = "path-end"

  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    let library = model.library
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: 8) {
        path(library)
        if let chip = model.routeChip {
          RouteChip(text: chip) { model.tab = .connection }
            .arrives(reduceMotion: reduceMotion)
        }
      }
      .padding(.trailing, Theme.inset)
      if !library.subfolders.isEmpty {
        subfolders(library)
      }
    }
    .padding(.bottom, 4)
    .background(Theme.ground.ignoresSafeArea(edges: .top))
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: library.trail)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: library.subfolders)
  }

  private func path(_ library: LibraryModel) -> some View {
    ScrollViewReader { proxy in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 0) {
        HStack(spacing: 6) {
          step("All media", isCurrent: library.trail.isEmpty) { library.goToAllMedia() }
          ForEach(Array(library.trail.enumerated()), id: \.element.id) { index, folder in
            Image(systemName: "chevron.right")
              .font(.caption2.weight(.semibold))
              .foregroundStyle(Theme.body.opacity(0.7))
              .accessibilityHidden(true)
            step(folder.name, isCurrent: index == library.trail.count - 1) { library.goBack(to: index) }
          }
        }
        .padding(.horizontal, Theme.inset)
        Color.clear.frame(width: 1, height: 1).id(Self.pathEnd)
        }
      }
      .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
      // A long path keeps its end, the folder you are in, on screen.
      .onAppear { proxy.scrollTo(Self.pathEnd, anchor: .trailing) }
      .onChange(of: library.trail) {
        withAnimation(reduceMotion ? nil : .easeOut(duration: 0.15)) {
          proxy.scrollTo(Self.pathEnd, anchor: .trailing)
        }
      }
    }
  }

  private func step(_ name: String, isCurrent: Bool, action: @escaping () -> Void) -> some View {
    Button(action: action) {
      Text(name)
        .font(isCurrent ? .headline : .subheadline)
        .foregroundStyle(isCurrent ? Theme.title : Theme.body)
        .fixedSize()
        .frame(minHeight: 44)
        .contentShape(Rectangle())
    }
    .buttonStyle(TileButtonStyle())
    .allowsHitTesting(!isCurrent)
    .accessibilityAddTraits(isCurrent ? .isHeader : [])
    .accessibilityHint(isCurrent ? "" : "Goes back to this folder")
  }

  private func subfolders(_ library: LibraryModel) -> some View {
    ScrollView(.horizontal, showsIndicators: false) {
      HStack(spacing: 8) {
        ForEach(library.subfolders) { folder in
          Button { library.enter(folder) } label: { chip(folder) }
            .buttonStyle(TileButtonStyle())
            .accessibilityLabel("\(folder.name), \(folder.itemCount) items")
        }
      }
      .padding(.horizontal, Theme.inset)
    }
    .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
    // A new folder's list starts from its first entry.
    .id(library.trail.last?.id ?? "")
  }

  private func chip(_ folder: Folder) -> some View {
    ChipLabel {
      Image(systemName: "folder.fill").font(.footnote).foregroundStyle(Theme.body)
      Text(folder.name).font(.subheadline).foregroundStyle(Theme.title).fixedSize()
      Text(folder.itemCount, format: .number).font(.caption.monospacedDigit()).foregroundStyle(Theme.body).fixedSize()
    }
  }
}

/// The app's one chip: a fill and its words, in a 44 pt touch area. Chosen, it lifts.
struct ChipLabel<Content: View>: View {
  var isOn = false
  @ViewBuilder var content: Content

  var body: some View {
    HStack(spacing: 6) { content }
      .padding(.horizontal, Theme.inset)
      .frame(minHeight: 36)
      .background(Capsule().fill(isOn ? Theme.lifted : Theme.surface))
      .padding(.vertical, 4)
      .contentShape(Rectangle())
  }
}

struct RouteChip: View {
  let text: String
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Text(text)
        .font(.caption.weight(.medium))
        .foregroundStyle(Theme.warm)
        .fixedSize()
    }
    .accessibilityHint("Shows the connection")
  }
}

struct PeopleScreen: View {
  @Environment(AppModel.self) private var model

  @ScaledMetric(relativeTo: .subheadline) private var cellWidth: CGFloat = 104

  var body: some View {
    NavigationStack {
      ScrollView {
        if model.peopleFailed {
          NoticeCard(message: FailureCopy.peopleFailed, actionTitle: "Try again") { model.retryPeople() }
            .padding(.horizontal, Theme.inset)
            .padding(.vertical, 8)
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: cellWidth), spacing: 12)], spacing: 16) {
          ForEach(model.people) { person in
            NavigationLink(value: person) { PersonCell(person: person) }
              .buttonStyle(TileButtonStyle())
          }
        }
        .padding(Theme.inset)
      }
      .background(Theme.ground)
      .navigationTitle("People")
      .navigationBarTitleDisplayMode(.inline)
      .navigationDestination(for: Person.self) { PersonScreen(person: $0) }
    }
  }
}

private struct PersonCell: View {
  let person: Person
  @Environment(AppModel.self) private var model
  @State private var face: UIImage?

  var body: some View {
    VStack(spacing: 8) {
      Theme.surface
        .aspectRatio(1, contentMode: .fit)
        .overlay {
          if let face = face ?? model.services.images.cachedImage(for: person.face.id) {
            Image(uiImage: face).resizable().scaledToFill()
          }
        }
        .clipShape(Circle())
      VStack(spacing: 2) {
        Text(person.name).font(.subheadline).foregroundStyle(Theme.title)
          .multilineTextAlignment(.center)
        Text(person.itemCount, format: .number).font(.caption.monospacedDigit()).foregroundStyle(Theme.body)
      }
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("\(person.name), \(person.itemCount) photos and clips")
    .accessibilityAddTraits(.isButton)
    .task { face = await model.services.images.thumbnail(for: person.face) }
  }
}

private struct PersonScreen: View {
  let person: Person
  @Environment(AppModel.self) private var model
  @State private var grid: (sections: [GridSection], entries: GridSource)?
  @State private var failed = false
  @State private var attempt = 0

  var body: some View {
    Group {
      if failed {
        VStack(spacing: 0) {
          NoticeCard(message: FailureCopy.libraryFailed, actionTitle: "Try again") {
            failed = false
            attempt += 1
          }
          .padding(.horizontal, Theme.inset)
          .padding(.vertical, 8)
          Spacer(minLength: 0)
        }
        .background(Theme.ground)
      } else if let grid {
        MediaGrid(sections: grid.sections, entries: grid.entries, context: "person-\(person.id)") { EmptyView() }
      } else {
        GridSkeleton().background(Theme.ground)
      }
    }
    .navigationTitle(person.name)
    .task(id: attempt) {
      do {
        let list = try await model.services.library.list(in: .person(person))
        grid = await Task.detached(priority: .userInitiated) { LibraryModel.grid(of: list) }.value
      } catch {
        guard !Task.isCancelled, !(error is CancellationError) else { return }
        failed = true
      }
    }
  }
}
