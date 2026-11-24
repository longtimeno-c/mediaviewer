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
  /// Held at double speed (a finger on either side of the picture), until it lifts.
  private(set) var isFast = false
  private(set) var seconds: TimeInterval = 0
  private(set) var length: TimeInterval
  private(set) var still: UIImage?
  private(set) var download: Download?
  /// The player has been waiting for data for more than a second.
  private(set) var isWaiting = false
  /// The server converts this clip as it plays (docs/plans/phone-clip-convert.md): a wait says so.
  private(set) var isConverted = false

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
  /// Where the newest seek is going. Until it lands the player still reports the old time, which
  /// must not pull the bar back.
  private var seekTarget: TimeInterval?
  private var seekGeneration = 0
  /// The page was left: a load still on its way must not start the player.
  private var ended = false
  /// The clip's copy on the phone, once downloaded: played from here, never asked of the server again.
  private var localFile: URL?

  init(item: MediaItem, clips: ClipService) {
    self.item = item
    self.clips = clips
    length = item.duration ?? 0
  }

  func load(startAt: Moment?) async {
    try? AVAudioSession.sharedInstance().setCategory(.playback)
    moments = await clips.moments(for: item)
    guard !ended else { return }  // the page was left while it was asked for
    await route(startAt: startAt)
    if phase != .playable, !ended {
      seconds = startAt?.seconds ?? 0
      still = await clips.still(of: item, at: seconds)
    }
  }

  private func route(startAt: Moment?) async {
    if let localFile { return await open(.stream(AVURLAsset(url: localFile)), startAt: startAt) }
    await open(await clips.route(for: item), startAt: startAt)
  }

  private func open(_ route: ClipRoute, startAt: Moment?) async {
    guard !ended else { return }
    if case .converted = route { isConverted = true }
    switch route {
    case .stream(let asset), .converted(let asset):
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
        guard let self, !self.scrubbing, self.seekTarget == nil else { return }
        self.seconds = time.seconds
        let total = playerItem.duration.seconds
        if total.isFinite, total > 0 { self.length = total }
      }
    }
    endToken = NotificationCenter.default.addObserver(forName: AVPlayerItem.didPlayToEndTimeNotification,
                                                      object: playerItem, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated {
        self?.isPlaying = false
        self?.isFast = false
      }
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
    ended = true
    isFast = false
    isPlaying = false
    player.pause()
    downloadTask?.cancel()
    removeObservers()
  }

  func play() {
    guard !ended else { return }
    if length > 0, seconds >= length - 0.05 { seek(to: 0) }
    player.play()
    isPlaying = true
  }

  /// Double speed while held; back to normal on release. A paused clip starts playing fast, as a
  /// hold on a short video does elsewhere.
  func setFast(_ on: Bool) {
    guard phase == .playable, !ended, on != isFast else { return }
    isFast = on
    if on {
      if !isPlaying { play() }
      player.rate = 2
    } else if isPlaying {
      player.rate = 1
    }
  }

  func pause() {
    isFast = false
    player.pause()
    isPlaying = false
  }

  func toggle() { isPlaying ? pause() : play() }

  /// A frame-exact seek over the network decodes from the keyframe before it, seconds on a long
  /// clip: half a second either way lands on a nearby keyframe at once. A newer seek replaces one
  /// still on its way (AVPlayer ends the older with `false`). A length not known yet (0) is no
  /// limit: clamping to it sent every skip back to the start.
  func seek(to target: TimeInterval) {
    let clamped = max(0, length > 0 ? min(length, target) : target)
    seconds = clamped
    seekTarget = clamped
    seekGeneration += 1
    let mine = seekGeneration
    let slack = CMTime(seconds: 0.5, preferredTimescale: 600)
    player.seek(to: CMTime(seconds: clamped, preferredTimescale: 600), toleranceBefore: slack, toleranceAfter: slack) { [weak self] _ in
      DispatchQueue.main.async {
        guard let self, self.seekGeneration == mine else { return }
        self.seekTarget = nil
      }
    }
  }

  /// The moment the picture is at: the last one reached.
  var activeMoment: Moment? { moments.last { $0.seconds <= seconds + 0.05 } }

  /// From where the last skip was going, so quick taps add up rather than restart from the old time.
  func skip(by delta: TimeInterval) { seek(to: (seekTarget ?? seconds) + delta) }

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
        // Playable once it is on the phone, from exactly where the picture was. Played from the
        // copy itself: asking the server again weighs the same clip against the same slow
        // connection and lands back on "too big".
        guard let file else {
          download = .failed
          return
        }
        let at = seconds
        localFile = file
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

/// What a clip's own stream says about it, for Details: read from the clip already open, so it
/// costs the server nothing more. A clip the server converts reports the conversion, not the file.
struct VideoFacts: Equatable, Sendable {
  var resolution: String?
  var frameRate: String?
  var codec: String?
  var dynamicRange: String?
  var bitRate: String?
  var audio: String?
  var converted = false
}

extension ClipModel {
  func videoFacts() async -> VideoFacts? {
    guard phase == .playable, let asset = player.currentItem?.asset else { return nil }
    var facts = VideoFacts(converted: isConverted)
    if let video = try? await asset.loadTracks(withMediaType: .video).first {
      if let (size, transform) = try? await video.load(.naturalSize, .preferredTransform) {
        let shown = size.applying(transform)
        let w = Int(abs(shown.width).rounded()), h = Int(abs(shown.height).rounded())
        if w > 0, h > 0 { facts.resolution = "\(w) × \(h)" + Self.resolutionName(w, h) }
      }
      if let rate = try? await video.load(.nominalFrameRate), rate > 0 {
        // 29.97 stays as it is; 14.9997 from a stream's rounding is 15.
        facts.frameRate = abs(rate - rate.rounded()) < 0.01 ? "\(Int(rate.rounded())) fps" : String(format: "%.2f fps", rate)
      }
      if let formats = try? await video.load(.formatDescriptions), let first = formats.first {
        facts.codec = Self.codecName(CMFormatDescriptionGetMediaSubType(first))
      }
      if let traits = try? await video.load(.mediaCharacteristics), traits.contains(.containsHDRVideo) {
        facts.dynamicRange = "HDR"
      }
      if let bits = try? await video.load(.estimatedDataRate), bits > 0 {
        facts.bitRate = String(format: "%.1f Mbit/s", Double(bits) / 1_000_000)
      }
    }
    if let audio = try? await asset.loadTracks(withMediaType: .audio).first,
       let formats = try? await audio.load(.formatDescriptions), let first = formats.first {
      var parts = [Self.codecName(CMFormatDescriptionGetMediaSubType(first))].compactMap { $0 }
      if let basic = CMAudioFormatDescriptionGetStreamBasicDescription(first)?.pointee {
        let channels = Int(basic.mChannelsPerFrame)
        parts.append(channels == 1 ? "mono" : channels == 2 ? "stereo" : "\(channels) channels")
        if basic.mSampleRate > 0 { parts.append(String(format: "%.1f kHz", basic.mSampleRate / 1000)) }
      }
      if !parts.isEmpty { facts.audio = parts.joined(separator: " · ") }
    }
    return facts
  }

  private static func resolutionName(_ w: Int, _ h: Int) -> String {
    switch min(w, h) {
    case 2160...: return " (4K)"
    case 1440..<2160: return " (1440p)"
    case 1080..<1440: return " (1080p)"
    case 720..<1080: return " (720p)"
    default: return ""
    }
  }

  private static func codecName(_ code: FourCharCode) -> String? {
    switch code {
    case kCMVideoCodecType_HEVC, kCMVideoCodecType_HEVCWithAlpha: return "HEVC"
    case kCMVideoCodecType_H264: return "H.264"
    case kCMVideoCodecType_AppleProRes422, kCMVideoCodecType_AppleProRes422HQ, kCMVideoCodecType_AppleProRes422LT,
         kCMVideoCodecType_AppleProRes422Proxy, kCMVideoCodecType_AppleProRes4444, kCMVideoCodecType_AppleProRes4444XQ:
      return "ProRes"
    case kCMVideoCodecType_DolbyVisionHEVC: return "HEVC (Dolby Vision)"
    case kCMVideoCodecType_VP9: return "VP9"
    case kCMVideoCodecType_AV1: return "AV1"
    case kCMVideoCodecType_MPEG2Video: return "MPEG-2"
    case kAudioFormatMPEG4AAC, kAudioFormatMPEG4AAC_HE, kAudioFormatMPEG4AAC_HE_V2: return "AAC"
    case kAudioFormatAppleLossless: return "ALAC"
    case kAudioFormatLinearPCM: return "PCM"
    case kAudioFormatAC3: return "AC-3"
    case kAudioFormatEnhancedAC3: return "E-AC-3"
    case kAudioFormatOpus: return "Opus"
    case kAudioFormatMPEGLayer3: return "MP3"
    case kAudioFormatFLAC: return "FLAC"
    default: return nil
    }
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

/// The clip open in the viewer: one at a time, on its own page of the pager, so a swipe moves to
/// the next item as it does from a photo, and the viewer's controls and filmstrip stay. Owned by
/// the viewer, not the page: a page laid out again never restarts the clip.
@MainActor
@Observable
final class ClipLive {
  /// The page whose clip is open.
  private(set) var index: Int?
  private(set) var entryID: String?
  private(set) var clip: ClipModel?
  /// Room the viewer's controls take at the top and bottom (the safe area when they are hidden).
  var insets = EdgeInsets()
  var chromeShown = true
  /// Where the position bar is on screen: a drag that starts on it scrubs, never pages.
  var scrubFrame = CGRect.null
  /// Where the picture is on screen: a press held on either side of it plays at double speed.
  var pictureFrame = CGRect.null
  @ObservationIgnored private var loading: Task<Void, Never>?

  func open(_ entry: GridEntry, at index: Int, clips: ClipService) {
    close()
    let clip = ClipModel(item: entry.item, clips: clips)
    self.clip = clip
    self.index = index
    entryID = entry.id
    loading = Task { await clip.load(startAt: entry.moment) }
  }

  func close() {
    loading?.cancel()
    loading = nil
    clip?.stop()
    clip = nil
    index = nil
    entryID = nil
    scrubFrame = .null
    pictureFrame = .null
  }
}

/// The open clip's page: the player, its position bar and buttons, and its moments.
struct ClipLivePage: View {
  let entry: GridEntry
  let live: ClipLive
  let transfer: OriginalTransfer
  let onTap: () -> Void
  @Environment(\.verticalSizeClass) private var verticalSize

  var body: some View {
    ZStack {
      Color.black
      if let clip = live.clip, live.entryID == entry.id {
        ClipBody(clip: clip, entry: entry, transfer: transfer, compactHeight: verticalSize == .compact,
                 live: live, onTapPicture: onTap)
      }
    }
  }
}

private struct ClipBody: View {
  let clip: ClipModel
  let entry: GridEntry
  let transfer: OriginalTransfer
  let compactHeight: Bool
  let live: ClipLive
  /// Shows and hides the viewer's controls, as a tap on a photo does.
  let onTapPicture: () -> Void

  @Environment(AppModel.self) private var model
  @Environment(\.dynamicTypeSize) private var typeSize

  var body: some View {
    Group {
      if compactHeight {
        // The picture fills the screen; its controls show and hide with the viewer's.
        VStack(spacing: 0) {
          Spacer(minLength: 0)
          if live.chromeShown {
            below
              .padding(Theme.inset)
              .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(.black.opacity(0.55)))
          }
        }
        .padding(.leading, live.insets.leading + 12)
        .padding(.trailing, live.insets.trailing + 12)
        .padding(.top, live.insets.top)
        .padding(.bottom, live.insets.bottom + 8)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background { picture }
      } else if typeSize.isAccessibilitySize {
        // Large type does not fit beside a flexible picture, so everything scrolls as one column.
        ScrollView {
          VStack(spacing: 0) {
            picture.frame(height: 220)
            below.padding(.horizontal, 16).padding(.top, 12)
            momentRows
          }
        }
        .padding(.top, live.insets.top)
        .padding(.bottom, live.insets.bottom)
      } else {
        // Full screen (the viewer's controls hidden): the picture takes all but the position bar
        // and the buttons; the moments come back with the controls.
        VStack(spacing: 0) {
          picture.frame(maxHeight: .infinity)
          below.padding(.horizontal, 16).padding(.top, live.chromeShown ? 12 : 4)
          if live.chromeShown { momentsList }
        }
        .padding(.top, live.insets.top)
        .padding(.bottom, live.insets.bottom)
      }
    }
    .animation(.easeOut(duration: 0.15), value: live.insets)
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
    .contentShape(Rectangle())
    .onTapGesture(perform: onTapPicture)
    // Either side, held, plays at double speed (the pager's press, PagerController): it needs
    // to know where the picture is.
    .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { live.pictureFrame = $0 }
    .onDisappear { live.pictureFrame = .null }
    .overlay(alignment: .top) {
      if clip.isFast {
        Label("2×", systemImage: "forward.fill")
          .font(.footnote.weight(.semibold))
          .foregroundStyle(.white)
          .padding(.horizontal, 10)
          .padding(.vertical, 5)
          .background(Capsule().fill(.black.opacity(0.55)))
          .padding(.top, 12)
          .transition(.opacity)
          .accessibilityHidden(true)
      }
    }
    .animation(.easeOut(duration: 0.15), value: clip.isFast)
    .accessibilityAction(named: clip.isFast ? "Normal speed" : "Double speed") { clip.setFast(!clip.isFast) }
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
        if clip.isWaiting { ProgressLine(verb: clip.isConverted ? FailureCopy.clipConverting : "Waiting for the clip", fraction: nil) }
        ClipTrack(clip: clip, live: live)
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
      if !clip.moments.isEmpty { ClipTrack(clip: clip, live: live) }
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
    ZStack {
      transportButtons
      HStack {
        Spacer(minLength: 0)
        Button(action: onTapPicture) {
          transportIcon(live.chromeShown ? "arrow.up.left.and.arrow.down.right" : "arrow.down.right.and.arrow.up.left")
            .font(.system(size: 17))
        }
        .accessibilityLabel(live.chromeShown ? "Full screen" : "Exit full screen")
        .accessibilityShowsLargeContentViewer()
      }
    }
    .foregroundStyle(Theme.title)
    .buttonStyle(.plain)
  }

  private var transportButtons: some View {
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
  /// Told where the bar is, so the pager leaves a drag on it alone.
  var live: ClipLive? = nil
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
      .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { live?.scrubFrame = $0 }
      .onDisappear { live?.scrubFrame = .null }
      .accessibilityElement()
      .accessibilityLabel("Position")
      .accessibilityValue(String.clockFrom(clip.seconds))
      .accessibilityAdjustableAction { direction in
        clip.skip(by: direction == .increment ? 10 : -10)
      }
      HStack {
        Text(String.clockFrom(clip.seconds))
        Spacer()
        Text(clip.length > 0 ? String.clockFrom(clip.length) : "–:––")
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
