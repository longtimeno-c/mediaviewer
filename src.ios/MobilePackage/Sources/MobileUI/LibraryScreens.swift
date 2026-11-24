// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Library (date-ordered grid, folders as pages, the route chip only when it matters), People.
import MobileKit
import QuickLook
import SwiftUI

/// The library as a navigation stack: All media at the bottom, a page per folder above it. The
/// system's push and pop, so the edge swipe follows the finger and can be let go of, and the back
/// button's menu goes back several folders at once. A folder's page grows out of its tile.
struct LibraryScreen: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @Namespace private var tiles

  var body: some View {
    @Bindable var library = model.library
    NavigationStack(path: $library.stack) {
      FolderScreen(page: library.root)
        .navigationDestination(for: FolderPage.self) { page in
          FolderScreen(page: page).modifier(GrowsFromTile(id: page.folder?.id, namespace: reduceMotion ? nil : tiles))
        }
    }
    .environment(\.folderNamespace, reduceMotion ? nil : tiles)
    .quickLookPreview($library.previewing)
  }
}

/// With Reduce Motion, the system's plain push instead.
private struct GrowsFromTile: ViewModifier {
  let id: String?
  let namespace: Namespace.ID?

  func body(content: Content) -> some View {
    if let id, let namespace {
      content.navigationTransition(.zoom(sourceID: id, in: namespace))
    } else {
      content
    }
  }
}

/// One folder's page (or All media's): its folders, its files and its photos.
struct FolderScreen: View {
  let page: FolderPage
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    content
      .background(Theme.ground)
      .navigationBarTitleDisplayMode(.inline)
      // Its title, and Add, Select and the bar under what is chosen where this phone may change files.
      .modifier(FolderEditing(page: page))
      .environment(\.shownPage, page)
      .toolbar {
        if let chip = model.routeChip, !page.selecting {
          // Under the folder's name, in the bar's middle: beside Add and Select on the trailing
          // side it took the room the name needs, which then read "…". The same on a page that
          // has no Add or Select, so the name does not move when a page learns it may be changed.
          ToolbarItem(placement: .principal) {
            VStack(spacing: 0) {
              Text(page.title).font(.headline).lineLimit(1).truncationMode(.middle)
                .accessibilityAddTraits(.isHeader)
                .accessibilityIdentifier("folder-title")
              RouteChip(text: chip, tone: model.connection.isSample ? Theme.accent : Theme.warm) { model.tab = .connection }
            }
            // Two lines in a 44 pt bar: larger type than this would be clipped.
            .dynamicTypeSize(...DynamicTypeSize.xLarge)
          }
        }
      }
      // A listing that comes after the page has slid in fades in over its skeleton.
      .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: page.isLoading)
  }

  /// Above photos, a few rows of files; with no photos, every file.
  private static let filesAbovePhotos = 3

  @ViewBuilder
  private var content: some View {
    if page.isLoading {
      // All media at launch shows its skeleton at once; a folder's page, only if its listing is slow.
      GridSkeleton().background(Theme.ground)
        .transition(.opacity)
    } else if page.sections.isEmpty, !page.subfolders.isEmpty || !page.others.isEmpty {
      // A folder of folders (a year, an archive, the top of a server) or of documents: its folders
      // and its files are the page.
      ScrollView {
        VStack(alignment: .leading, spacing: 0) {
          noticeSlot
          if !page.subfolders.isEmpty { FolderGrid(folders: page.subfolders) }
          if !page.others.isEmpty { FileList(files: page.others) }
        }
        .padding(.bottom, 16)
      }
      .background(Theme.ground)
      .transition(.opacity)
    } else if page.sections.isEmpty {
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
      .transition(.opacity)
    } else {
      MediaGrid(sections: page.sections, entries: page.entries, context: page.context) {
        noticeSlot
        if !page.subfolders.isEmpty { FolderShelf(folders: page.subfolders) }
        if !page.others.isEmpty {
          FileList(files: page.others, collapsedTo: Self.filesAbovePhotos)
        }
      }
      .transition(.opacity)
    }
  }

  private var emptyWords: some View {
    Text(page.failure != nil ? "" : page.path.isEmpty
         ? (model.connection.fileMode ? FailureCopy.fileModeTop
            : "Nothing here yet. Photos appear once MediaViewer on your computer publishes them.")
         : "This folder has no photos or clips.")
      .font(.subheadline)
      .foregroundStyle(Theme.body)
      .multilineTextAlignment(.center)
      .fixedSize(horizontal: false, vertical: true)
      .padding(24)
  }

  @ViewBuilder
  private var noticeSlot: some View {
    if let refused = page.changeFailure {
      NoticeCard(message: refused, onDismiss: {
        withAnimation(reduceMotion ? nil : .easeOut(duration: 0.15)) { model.library.dismissChangeFailure(on: page) }
      })
      .padding(.horizontal, Theme.inset)
      .padding(.vertical, 8)
      .arrives(reduceMotion: reduceMotion)
    }
    // The sample's changes are kept on this phone: said wherever they can be made.
    if model.connection.isSample, page.changeable {
      NoticeCard(message: FailureCopy.sampleChangesDemo)
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
        .accessibilityIdentifier("sample-changes-demo")
    }
    UploadRows(page: page)
    if let failure = page.failure {
      NoticeCard(message: failure, actionTitle: "Try again") { model.library.load(page) }
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
        .arrives(reduceMotion: reduceMotion)
    }
    if let notice = model.libraryNotice {
      NoticeCard(message: notice.message,
                 onDismiss: notice.canDismiss ? {
                   withAnimation(reduceMotion ? nil : .easeOut(duration: 0.15)) { model.dismiss(notice) }
                 } : nil,
                 actionTitle: notice.action == .scan ? "Scan code" : notice.action == .retry ? "Try again" : nil,
                 action: noticeAction(notice))
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
        .arrives(reduceMotion: reduceMotion)
    }
  }
}

extension EnvironmentValues {
  /// The library's folder tiles, which each folder's page grows out of; nil with Reduce Motion.
  @Entry var folderNamespace: Namespace.ID? = nil
  /// The page a view is on.
  @Entry var shownPage: FolderPage? = nil
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

extension FolderScreen {
  /// What a notice's button does: scan a new code, or try the connection again.
  fileprivate func noticeAction(_ notice: LibraryNotice) -> (() -> Void)? {
    switch notice.action {
    case .scan: return { model.isScanning = true }
    case .retry:
      let connection = model.services.connection
      return { Task { await connection.retry() } }
    case nil: return nil
    }
  }
}

struct RouteChip: View {
  let text: String
  var tone: Color = Theme.warm
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Text(text)
        .font(.caption.weight(.medium))
        .foregroundStyle(tone)
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
        let built = await Task.detached(priority: .userInitiated) { LibraryModel.grid(of: list) }.value
        grid = (built.sections, built.entries)
      } catch {
        guard !Task.isCancelled, !(error is CancellationError) else { return }
        failed = true
      }
    }
  }
}
