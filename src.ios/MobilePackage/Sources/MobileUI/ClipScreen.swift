// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Clip: the player and its moment ticks. A speech or scene hit opens at that moment, the
// feature nothing else on a NAS does. A clip too big for the connection, or in a container the
// phone cannot play, keeps its moments as stills and says why.
import AVFoundation
import MobileKit
import Observation
import SwiftUI

@MainActor
@Observable
final class ClipModel {
  enum Phase: Equatable {
    case reading
    case playable
    case tooBig(neededMbps: Int, availableMbps: Int, fileBytes: Int64)
    case notPlayable(containerName: String, fileBytes: Int64)
    case unavailable(MediaFailure)
    /// It started, then the player gave up.
    case stopped
  }

  enum Download: Equatable {
    case running(TransferProgress?)
    case failed
    /// A file the phone cannot play, ready to hand on.
    case finished(URL)
  }

  private(set) var phase: Phase = .reading
  private(set) var moments: [Moment] = []
  private(set) var isPlaying = false
  private(set) var seconds: TimeInterval = 0
  private(set) var length: TimeInterval
  private(set) var still: UIImage?
  private(set) var download: Download?
  /// The player has been waiting for data for more than a second.
  private(set) var isWaiting = false

  let player = AVPlayer()
  private let item: MediaItem
  private let clips: ClipService
  private var timeToken: Any?
  private var endToken: NSObjectProtocol?
  private var failToken: NSObjectProtocol?
  private var statusObservation: NSKeyValueObservation?
  private var controlObservation: NSKeyValueObservation?
  private var waitingCheck: Task<Void, Never>?
  private var downloadTask: Task<Void, Never>?
  private var scrubbing = false

  init(item: MediaItem, clips: ClipService) {
    self.item = item
    self.clips = clips
    length = item.duration ?? 0
  }

  func load(startAt: Moment?) async {
    try? AVAudioSession.sharedInstance().setCategory(.playback)
    moments = await clips.moments(for: item)
    await route(startAt: startAt)
    if phase != .playable {
      seconds = startAt?.seconds ?? 0
      still = await clips.still(of: item, at: seconds)
    }
  }

  private func route(startAt: Moment?) async {
    switch await clips.route(for: item) {
    case .stream(let asset):
      let playerItem = AVPlayerItem(asset: asset)
      player.replaceCurrentItem(with: playerItem)
      observe(playerItem)
      if let startAt {
        await player.seek(to: CMTime(seconds: startAt.seconds, preferredTimescale: 600),
                          toleranceBefore: .zero, toleranceAfter: .zero)
        seconds = startAt.seconds
      }
      phase = .playable
      play()
    case .tooBigToStream(let needed, let available, let bytes):
      phase = .tooBig(neededMbps: needed, availableMbps: available, fileBytes: bytes)
    case .notPlayableHere(let name, let bytes):
      phase = .notPlayable(containerName: name, fileBytes: bytes)
    case .unavailable(let failure):
      phase = .unavailable(failure)
    }
  }

  private func observe(_ playerItem: AVPlayerItem) {
    removeObservers()
    statusObservation = playerItem.observe(\.status) { [weak self] item, _ in
      guard item.status == .failed else { return }
      DispatchQueue.main.async { self?.playbackStopped() }
    }
    controlObservation = player.observe(\.timeControlStatus) { [weak self] player, _ in
      let waiting = player.timeControlStatus == .waitingToPlayAtSpecifiedRate
      DispatchQueue.main.async { self?.playerWaiting(waiting) }
    }
    failToken = NotificationCenter.default.addObserver(forName: AVPlayerItem.failedToPlayToEndTimeNotification,
                                                       object: playerItem, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated { self?.playbackStopped() }
    }
    timeToken = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 8), queue: .main) { [weak self] time in
      MainActor.assumeIsolated {
        guard let self, !self.scrubbing else { return }
        self.seconds = time.seconds
        let total = playerItem.duration.seconds
        if total.isFinite, total > 0 { self.length = total }
      }
    }
    endToken = NotificationCenter.default.addObserver(forName: AVPlayerItem.didPlayToEndTimeNotification,
                                                      object: playerItem, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated { self?.isPlaying = false }
    }
  }

  private func playerWaiting(_ waiting: Bool) {
    waitingCheck?.cancel()
    guard waiting else {
      isWaiting = false
      return
    }
    // A wait shorter than a second is not worth a word.
    waitingCheck = Task {
      try? await Task.sleep(for: .seconds(1))
      if !Task.isCancelled { isWaiting = true }
    }
  }

  private func playbackStopped() {
    player.pause()
    isPlaying = false
    isWaiting = false
    waitingCheck?.cancel()
    phase = .stopped
  }

  /// Starts again from where it stopped.
  func retry() {
    let at = seconds
    phase = .reading
    Task {
      await route(startAt: Moment(seconds: at, kind: .scene, text: ""))
    }
  }

  private func removeObservers() {
    if let timeToken { player.removeTimeObserver(timeToken) }
    timeToken = nil
    if let endToken { NotificationCenter.default.removeObserver(endToken) }
    endToken = nil
    if let failToken { NotificationCenter.default.removeObserver(failToken) }
    failToken = nil
    statusObservation = nil
    controlObservation = nil
    waitingCheck?.cancel()
  }

  func stop() {
    player.pause()
    downloadTask?.cancel()
    removeObservers()
  }

  func play() {
    if length > 0, seconds >= length - 0.05 { seek(to: 0) }
    player.play()
    isPlaying = true
  }

  func pause() {
    player.pause()
    isPlaying = false
  }

  func toggle() { isPlaying ? pause() : play() }

  func seek(to target: TimeInterval) {
    seconds = max(0, min(length, target))
    player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
  }

  /// The moment the picture is at: the last one reached.
  var activeMoment: Moment? { moments.last { $0.seconds <= seconds + 0.05 } }

  func skip(by delta: TimeInterval) { seek(to: seconds + delta) }

  func beginScrub() { scrubbing = true }

  func endScrub() { scrubbing = false }

  func jump(to moment: Moment) {
    if phase == .playable {
      seek(to: moment.seconds)
    } else {
      seconds = moment.seconds
      Task { still = await clips.still(of: item, at: moment.seconds) }
    }
  }

  func startDownload() {
    download = .running(nil)
    downloadTask = Task {
      do {
        var file: URL?
        for try await event in clips.download(item) {
          switch event {
          case .progress(let progress): download = .running(progress)
          case .finished(let url): file = url
          }
        }
        guard !Task.isCancelled else { return }
        if case .notPlayable = phase {
          download = file.map(Download.finished) ?? .failed
          return
        }
        // Playable once it is on the phone, from exactly where the picture was.
        let at = seconds
        download = nil
        phase = .reading
        await route(startAt: Moment(seconds: at, kind: .scene, text: ""))
      } catch {
        guard !Task.isCancelled else { return }
        download = .failed
      }
    }
  }

  func cancelDownload() {
    downloadTask?.cancel()
    download = nil
  }
}

private struct PlayerSurface: UIViewRepresentable {
  let player: AVPlayer

  final class SurfaceView: UIView {
    override class var layerClass: AnyClass { AVPlayerLayer.self }
    var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
  }

  func makeUIView(context: Context) -> SurfaceView {
    let view = SurfaceView()
    view.backgroundColor = .black
    view.playerLayer.videoGravity = .resizeAspect
    view.playerLayer.player = player
    return view
  }

  func updateUIView(_ view: SurfaceView, context: Context) {}
}

struct ClipScreen: View {
  let entry: GridEntry
  let onClose: () -> Void

  @Environment(AppModel.self) private var model
  @Environment(\.verticalSizeClass) private var verticalSize
  @State private var clip: ClipModel?

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()
      if let clip {
        ClipBody(clip: clip, entry: entry, compactHeight: verticalSize == .compact, onClose: onClose)
      }
    }
    .task(id: entry.id) {
      let new = ClipModel(item: entry.item, clips: model.services.clips)
      clip = new
      await new.load(startAt: entry.moment)
    }
    .onDisappear { clip?.stop() }
  }
}

private struct ClipBody: View {
  let clip: ClipModel
  let entry: GridEntry
  let compactHeight: Bool
  let onClose: () -> Void

  @Environment(AppModel.self) private var model
  @Environment(\.dynamicTypeSize) private var typeSize
  @State private var controlsShown = true

  var body: some View {
    Group {
      if compactHeight {
        // The picture sits behind, so it can fill the screen while the controls keep to the safe area.
        VStack(spacing: 0) {
          if controlsShown {
            topBar
            Spacer(minLength: 0)
            below
              .padding(Theme.inset)
              .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(.black.opacity(0.55)))
          }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background {
          picture
            .ignoresSafeArea()
            .contentShape(Rectangle())
            .onTapGesture { controlsShown.toggle() }
        }
      } else if typeSize.isAccessibilitySize {
        // Large type does not fit beside a flexible picture, so everything scrolls as one column.
        VStack(spacing: 0) {
          topBar.padding(.horizontal, 12).padding(.top, 4)
          ScrollView {
            VStack(spacing: 0) {
              picture.frame(height: 220)
              below.padding(.horizontal, 16).padding(.top, 12)
              momentRows
            }
          }
        }
      } else {
        VStack(spacing: 0) {
          topBar.padding(.horizontal, 12).padding(.top, 4)
          picture.frame(maxHeight: .infinity)
          below.padding(.horizontal, 16).padding(.top, 12)
          momentsList
        }
      }
    }
  }

  private var topBar: some View {
    HStack {
      Button(action: onClose) { Image(systemName: "xmark") }
        .buttonStyle(ChromeIconStyle())
        .accessibilityLabel("Close")
      Spacer()
    }
  }

  @ViewBuilder
  private var picture: some View {
    ZStack {
      if clip.phase == .playable {
        PlayerSurface(player: clip.player)
          .accessibilityLabel(entry.accessibilityName)
      } else {
        posterImage
      }
    }
  }

  private var posterImage: some View {
    Color.black.overlay {
      if let image = clip.still ?? model.services.images.cachedImage(for: entry.item.id) {
        Image(uiImage: image).resizable().scaledToFit()
      }
    }
  }

  @ViewBuilder
  private var below: some View {
    switch clip.phase {
    case .reading:
      ProgressLine(verb: "Reading the clip", fraction: nil)
    case .playable:
      VStack(spacing: 8) {
        if clip.isWaiting { ProgressLine(verb: "Waiting for the clip", fraction: nil) }
        ClipTrack(clip: clip)
        transport
      }
    case .tooBig(let needed, let available, let bytes):
      fallback(title: "This clip is too big to play over this connection.",
               line: "It needs about \(needed) Mbit/s. This connection has about \(available).",
               button: "Download first · \(ProgressWords.size(bytes))")
    case .notPlayable(let name, let bytes):
      fallback(title: "This phone can't play \(name) files.",
               line: "Its moments are shown as stills.",
               button: "Download the file · \(ProgressWords.size(bytes))")
    case .unavailable(let failure):
      Text(FailureCopy.line(for: failure, sample: model.connection.isSample))
        .font(.footnote)
        .foregroundStyle(Theme.title)
        .frame(maxWidth: .infinity, alignment: .leading)
    case .stopped:
      VStack(alignment: .leading, spacing: 12) {
        Text(FailureCopy.clipStopped).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title)
        Button("Try again") { clip.retry() }.buttonStyle(SecondaryButtonStyle())
      }
    }
  }

  private func fallback(title: String, line: String, button: String) -> some View {
    VStack(alignment: .leading, spacing: 12) {
      if !clip.moments.isEmpty { ClipTrack(clip: clip) }
      VStack(alignment: .leading, spacing: 4) {
        Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title)
        Text(line).font(.footnote).foregroundStyle(Theme.body)
      }
      .fixedSize(horizontal: false, vertical: true)
      switch clip.download {
      case .running(let progress):
        ProgressLine(verb: "Downloading",
                     fraction: progress.map { $0.totalBytes > 0 ? Double($0.doneBytes) / Double($0.totalBytes) : 0 },
                     detail: progress.map { ProgressWords.amounts($0.doneBytes, of: $0.totalBytes) },
                     key: "clip-\(entry.id)", onCancel: { clip.cancelDownload() })
      case .failed:
        ProgressLine(verb: "Downloading", fraction: nil,
                     outcome: .failed("The download stopped.", canRetry: true),
                     onRetry: { clip.startDownload() })
      case .finished(let url):
        ShareLink(item: url) { Label("Share or save the file", systemImage: "square.and.arrow.up") }
          .buttonStyle(PrimaryButtonStyle())
      case nil:
        Button(button) { clip.startDownload() }
          .buttonStyle(PrimaryButtonStyle())
      }
    }
  }

  private func transportIcon(_ name: String) -> some View {
    Image(systemName: name)
      .frame(width: 44, height: 44)
      .contentShape(Rectangle())
  }

  private var transport: some View {
    HStack(spacing: 28) {
      Button { clip.skip(by: -10) } label: { transportIcon("gobackward.10") }
        .accessibilityLabel("Back 10 seconds")
        .accessibilityShowsLargeContentViewer()
      Button { clip.toggle() } label: {
        Image(systemName: clip.isPlaying ? "pause.fill" : "play.fill").font(.system(size: 30))
          .frame(width: 44, height: 44)
          .contentShape(Rectangle())
      }
      .accessibilityLabel(clip.isPlaying ? "Pause" : "Play")
      .accessibilityShowsLargeContentViewer()
      Button { clip.skip(by: 10) } label: { transportIcon("goforward.10") }
        .accessibilityLabel("Forward 10 seconds")
        .accessibilityShowsLargeContentViewer()
    }
    .font(.system(size: 22))
    .foregroundStyle(Theme.title)
    .frame(minHeight: 44)
    .buttonStyle(.plain)
  }

  private var momentRows: some View {
    LazyVStack(spacing: 0) {
      ForEach(clip.moments) { moment in
        Button {
          clip.jump(to: moment)
        } label: {
          HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(String.clockFrom(moment.seconds))
              .font(.footnote.monospacedDigit())
              .foregroundStyle(clip.activeMoment == moment ? Theme.accent : Theme.body)
            Text(moment.text)
              .font(.subheadline)
              .foregroundStyle(Theme.title)
              .multilineTextAlignment(.leading)
              .frame(maxWidth: .infinity, alignment: .leading)
          }
          .padding(.horizontal, 16)
          .frame(minHeight: 44)
          .contentShape(Rectangle())
        }
        .buttonStyle(TileButtonStyle())
      }
    }
    .padding(.vertical, 8)
  }

  @ViewBuilder
  private var momentsList: some View {
    if !clip.moments.isEmpty && !compactHeight {
      ScrollView { momentRows }.frame(maxHeight: 220)
    }
  }
}

/// A 3 pt line with a tick at each moment. A touch near a tick lands on it.
private struct ClipTrack: View {
  let clip: ClipModel
  @State private var dragging = false

  var body: some View {
    let progress = clip.length > 0 ? min(1, clip.seconds / clip.length) : 0
    VStack(spacing: 4) {
      GeometryReader { geo in
        let width = geo.size.width
        ZStack(alignment: .leading) {
          Capsule().fill(Theme.title.opacity(0.18)).frame(height: 3)
          Capsule().fill(Theme.accent).frame(width: max(3, width * progress), height: 3)
          ForEach(clip.moments) { moment in
            Capsule()
              .fill(Theme.title)
              .frame(width: 2, height: 11)
              .offset(x: clip.length > 0 ? width * moment.seconds / clip.length - 1 : 0)
          }
          Circle()
            .fill(.white)
            .frame(width: dragging ? 18 : 12, height: dragging ? 18 : 12)
            .offset(x: width * progress - (dragging ? 9 : 6))
        }
        .frame(height: 44)
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0).onChanged { value in
          if !dragging {
            dragging = true
            clip.beginScrub()
          }
          clip.seek(to: snapped(value.location.x, width: width))
        }.onEnded { value in
          let target = snapped(value.location.x, width: width)
          clip.seek(to: target)
          dragging = false
          clip.endScrub()
          // Without a player the picture is a still, so it lands on a moment.
          if clip.phase != .playable,
             let nearest = clip.moments.min(by: { abs($0.seconds - target) < abs($1.seconds - target) }) {
            clip.jump(to: nearest)
          }
        })
      }
      .frame(height: 44)
      .accessibilityElement()
      .accessibilityLabel("Position")
      .accessibilityValue(String.clockFrom(clip.seconds))
      .accessibilityAdjustableAction { direction in
        clip.skip(by: direction == .increment ? 10 : -10)
      }
      HStack {
        Text(String.clockFrom(clip.seconds))
        Spacer()
        Text(String.clockFrom(clip.length))
      }
      .font(.caption.monospacedDigit())
      .foregroundStyle(Theme.body)
    }
  }

  private func snapped(_ x: CGFloat, width: CGFloat) -> TimeInterval {
    guard width > 0, clip.length > 0 else { return 0 }
    let target = max(0, min(1, x / width)) * clip.length
    let reach = 12 / width * clip.length
    return clip.moments.map(\.seconds).min { abs($0 - target) < abs($1 - target) }
      .flatMap { abs($0 - target) <= reach ? $0 : nil } ?? target
  }
}
