// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Viewer: one photo, pinch and swipe. It opens as one object: the tile grows into it and
// sharpens in place. Details share the file: Details is a column on an iPad and a sheet on an iPhone.
import MobileKit
import SwiftUI

struct ViewerScreen: View {
  let request: ViewerRequest
  let namespace: Namespace.ID

  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  @State private var index: Int
  @State private var chromeShown = true
  @State private var zoomed = false
  @State private var pulling = false
  @State private var jump: PagerJump?
  @AppStorage(Preferences.filmstrip) private var showsFilmstrip = true
  @State private var pageStates: [MediaID: PageState] = [:]
  @State private var showDetails: Bool
  @State private var playing: GridEntry?
  @State private var save: SaveState?
  /// The item `save` is about. Swiping away leaves the save running but out of sight.
  @State private var saveItem: MediaItem?
  /// The last save failed because Photos access was refused: the line offers Settings.
  @State private var photosRefused = false
  @Environment(\.openURL) private var openURL
  @State private var saveToken = UUID()

  private enum SaveState: Equatable {
    case running(TransferProgress?)
    case outcome(ProgressOutcome)
  }

  init(request: ViewerRequest, namespace: Namespace.ID) {
    self.request = request
    self.namespace = namespace
    _index = State(initialValue: request.startIndex)
    _showDetails = State(initialValue: request.showDetails)
    let start = request.entries[request.startIndex]
    _playing = State(initialValue: request.playClip && start.item.kind == .clip ? start : nil)
  }

  private var current: GridEntry { request.entries[index] }

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()
      PagerView(entries: request.entries, startIndex: request.startIndex, images: model.services.images,
                onIndex: pageChanged, onTap: toggleChrome, onZoomed: { zoomed = $0 },
                onPlay: { playing = $0 }, onState: { pageStates[$0] = $1 },
                onPullChanged: { pulling = $0 }, onClose: close,
                onSettled: { model.viewerFocus = "\(request.context)/\($0)" }, jump: jump, isCovered: playing != nil)
        .ignoresSafeArea()
      // Not just invisible: nothing under an open clip may be reachable by VoiceOver.
      if chromeShown && !pulling && playing == nil {
        chrome.transition(.opacity)
      }
      if let playing {
        ClipScreen(entry: playing) { closeClip() }
          .id(playing.id)
          .transition(.opacity)
      }
    }
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: chromeShown && !pulling)
    .statusBarHidden(!chromeShown || playing != nil)
    .persistentSystemOverlays(chromeShown ? .automatic : .hidden)
    .inspector(isPresented: $showDetails) {
      DetailsScreen(item: current.item)
        .presentationDetents([.medium, .large])
        .inspectorColumnWidth(min: 300, ideal: 340, max: 420)
    }
    .navigationTransition(.zoom(sourceID: "\(request.context)/\(index)", in: namespace))
    .interactiveDismissDisabled(zoomed)
    .presentationBackground(.black)
  }

  private func pageChanged(_ new: Int) {
    index = new
  }

  private func toggleChrome() { chromeShown.toggle() }

  private func close() {
    model.viewerFocus = "\(request.context)/\(index)"
    model.closeViewer(reduceMotion: reduceMotion)
  }

  private func closeClip() {
    if request.playClip, index == request.startIndex {
      close()
    } else {
      playing = nil
    }
  }

  private var chrome: some View {
    VStack(spacing: 0) {
      controls
      if showsFilmstrip {
        Filmstrip(entries: request.entries, current: index, images: model.services.images) { target in
          jump = PagerJump(index: target)
        }
      }
    }
  }

  private var controls: some View {
    VStack(spacing: 0) {
      ZStack(alignment: .top) {
        HStack(alignment: .top, spacing: 8) {
          Button(action: close) { Image(systemName: "xmark") }
            .buttonStyle(ChromeIconStyle())
            .accessibilityLabel("Close")
            .accessibilityIdentifier(zoomed ? "viewer-close-zoomed" : "viewer-close")
          Spacer(minLength: 0)
          Button(action: startSave) { Image(systemName: "square.and.arrow.down") }
            .buttonStyle(ChromeIconStyle())
            .accessibilityLabel("Save to Photos")
            .disabled(isSaving)
          Button { showDetails = true } label: { Image(systemName: "info") }
            .buttonStyle(ChromeIconStyle())
            .accessibilityLabel("Details")
        }
        // Centred on the screen whatever sits at either end, kept clear of the buttons.
        Text(current.item.capturedAt.formatted(date: .abbreviated, time: .shortened))
          .font(.footnote.monospacedDigit())
          .foregroundStyle(.white)
          .multilineTextAlignment(.center)
          .padding(.horizontal, 12)
          .padding(.vertical, 6)
          .background(Capsule().fill(.black.opacity(0.5)))
          .dynamicTypeSize(...DynamicTypeSize.xxLarge)
          .padding(.top, 7)
          .padding(.horizontal, 96)
      }
      Spacer(minLength: 0)
      bottomSlot
        .frame(maxWidth: Theme.readableWidth)
    }
    .padding(12)
  }

  private var shownSave: SaveState? { saveItem?.id == current.item.id ? save : nil }

  private var isSaving: Bool {
    if case .running = shownSave { return true }
    return false
  }

  @ViewBuilder
  private var bottomSlot: some View {
    if let save = shownSave {
      slotCard {
        switch save {
        case .running(let progress):
          ProgressLine(verb: "Saving to Photos",
                       fraction: progress.map { $0.totalBytes > 0 ? Double($0.doneBytes) / Double($0.totalBytes) : 0 },
                       detail: progress.map { ProgressWords.amounts($0.doneBytes, of: $0.totalBytes) },
                       key: "save-\(current.item.id.raw)")
        case .outcome(let outcome):
          ProgressLine(verb: "Saving to Photos", fraction: nil, outcome: outcome, onRetry: retrySave,
                       extra: photosRefused ? (title: "Open Settings", action: openSettings) : nil)
        }
      }
    } else {
      switch pageStates[current.item.id] {
      case .refining(let done, let total):
        slotCard {
          ProgressLine(verb: "Loading original",
                       fraction: fraction(done, total),
                       detail: done.flatMap { d in total.map { ProgressWords.amounts(d, of: $0) } },
                       key: "original-\(current.id)")
        }
      case .failed(let failure):
        slotCard {
          Text(FailureCopy.line(for: failure))
            .font(.footnote)
            .foregroundStyle(Theme.title)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityAddTraits(.isStaticText)
        }
      default:
        EmptyView()
      }
    }
  }

  private func fraction(_ done: Int64?, _ total: Int64?) -> Double? {
    guard let done, let total, total > 0 else { return nil }
    return Double(done) / Double(total)
  }

  private func slotCard<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
    content()
      .padding(Theme.inset)
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
      .arrives(reduceMotion: reduceMotion)
  }

  private func startSave() { startSave(current.item) }

  private func retrySave() {
    if let saveItem { startSave(saveItem) }
  }

  private func openSettings() {
    if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
  }

  private func startSave(_ item: MediaItem) {
    let token = UUID()
    saveToken = token
    saveItem = item
    photosRefused = false
    save = .running(nil)
    Task {
      do {
        for try await progress in model.services.saver.save(item) {
          if token == saveToken { save = .running(progress) }
        }
        guard token == saveToken else { return }
        save = .outcome(.done("Saved to Photos"))
        try? await Task.sleep(for: .seconds(2))
        if token == saveToken, case .outcome(.done) = save { save = nil }
      } catch let failure as MediaFailure {
        if token == saveToken {
          photosRefused = failure == .photosNotAllowed
          save = .outcome(.failed(FailureCopy.line(for: failure), canRetry: false))
        }
      } catch {
        if token == saveToken { save = .outcome(.failed("Couldn't save this to Photos.", canRetry: true)) }
      }
    }
  }
}

// MARK: Filmstrip

/// The Mac's bottom strip, for a phone. It follows the page once a swipe has settled, and shows and
/// hides with the rest of the viewer's controls.
private struct Filmstrip: View {
  let entries: GridSource
  let current: Int
  let images: ImageLadder
  let choose: (Int) -> Void

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  private static let cellSide: CGFloat = 48

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView(.horizontal, showsIndicators: false) {
        // Lazy, so a very large library builds only the cells on screen.
        LazyHStack(spacing: 4) {
          ForEach(entries.indices, id: \.self) { index in
            Button { choose(index) } label: {
              FilmstripCell(entry: entries[index], images: images, side: Self.cellSide, isCurrent: index == current)
            }
            .buttonStyle(TileButtonStyle())
            .accessibilityLabel(entries[index].accessibilityName)
            .accessibilityAddTraits(index == current ? .isSelected : [])
            .id(index)
          }
        }
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
      }
      .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
      .onAppear { proxy.scrollTo(current, anchor: .center) }
      .onChange(of: current) { _, new in
        withAnimation(reduceMotion ? nil : .easeOut(duration: 0.2)) {
          proxy.scrollTo(new, anchor: .center)
        }
      }
    }
    .frame(height: Self.cellSide + 16)
    .background(Color.black.opacity(0.55).ignoresSafeArea(edges: .bottom))
  }
}

private struct FilmstripCell: View {
  let entry: GridEntry
  let images: ImageLadder
  let side: CGFloat
  let isCurrent: Bool
  @State private var loaded: UIImage?
  @State private var missing = false

  var body: some View {
    Theme.surface
      .overlay {
        if let image = loaded ?? images.cachedImage(for: entry.item.id) {
          Image(uiImage: image).resizable().interpolation(.medium).scaledToFill()
        } else if missing {
          Image(systemName: entry.item.kind == .clip ? "film" : "photo")
            .font(.system(size: 14))
            .foregroundStyle(Theme.body.opacity(0.6))
        }
      }
      .frame(width: side, height: side)
      .clipped()
      .overlay(alignment: .bottomTrailing) {
        if entry.item.kind == .clip {
          Image(systemName: "play.fill")
            .font(.system(size: 9))
            .foregroundStyle(.white)
            .padding(3)
            .shadow(radius: 1)
        }
      }
      // Selection lifts: the one on screen is bright, the others recede.
      .opacity(isCurrent ? 1 : 0.55)
      .scaleEffect(isCurrent ? 1.08 : 1)
      .animation(.easeOut(duration: 0.15), value: isCurrent)
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
}

// MARK: Details

/// A card: one fill, its rows inside.
struct InfoCard<Content: View>: View {
  @ViewBuilder var content: Content

  var body: some View {
    VStack(spacing: 12) { content }
      .padding(Theme.inset)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
  }
}

/// A label and its value on one line, or stacked once type is too large to fit both.
struct InfoRow: View {
  let label: String
  let value: String

  var body: some View {
    ViewThatFits(in: .horizontal) {
      HStack(alignment: .firstTextBaseline, spacing: 8) {
        Text(label).font(.subheadline).foregroundStyle(Theme.body).fixedSize()
        Spacer(minLength: 8)
        Text(value).font(.subheadline.monospacedDigit()).foregroundStyle(Theme.title).fixedSize()
      }
      VStack(alignment: .leading, spacing: 2) {
        Text(label).font(.subheadline).foregroundStyle(Theme.body)
        Text(value).font(.subheadline.monospacedDigit()).foregroundStyle(Theme.title)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .accessibilityElement(children: .combine)
  }
}

struct DetailsScreen: View {
  let item: MediaItem
  @Environment(AppModel.self) private var model
  @State private var details: MediaDetails?

  var body: some View {
    ScrollView {
      VStack(spacing: 12) {
        InfoCard {
          InfoRow(label: "Date", value: item.capturedAt.formatted(date: .long, time: .shortened))
          if let place = details?.place { InfoRow(label: "Place", value: place) }
          if let duration = item.duration { InfoRow(label: "Length", value: .clockFrom(duration)) }
        }
        if let details, details.camera != nil || details.lens != nil || details.exposure != nil {
          InfoCard {
            if let camera = details.camera { InfoRow(label: "Camera", value: camera) }
            if let lens = details.lens { InfoRow(label: "Lens", value: lens) }
            if let exposure = details.exposure { InfoRow(label: "Exposure", value: exposure) }
          }
        }
        InfoCard {
          InfoRow(label: "File", value: item.fileName)
          if let details {
            InfoRow(label: "Folder", value: details.folderPath)
            InfoRow(label: "Format", value: details.formatName)
            InfoRow(label: "Size", value: ProgressWords.size(details.fileBytes))
          }
          if item.pixelWidth > 0 {
            InfoRow(label: "Dimensions", value: "\(item.pixelWidth) × \(item.pixelHeight)")
          }
        }
      }
      .padding(Theme.inset)
    }
    .background(Theme.ground)
    .task(id: item.id) { details = try? await model.services.library.details(for: item) }
  }
}
