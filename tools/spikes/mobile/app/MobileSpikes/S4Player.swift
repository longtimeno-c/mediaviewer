// M1 spike S4 (throwaway): AVPlayer over AVAssetResourceLoaderDelegate on a custom scheme, reading
// byte ranges from a source the app controls, the way the real app will read over a link.
import AVFoundation
import Darwin
import SwiftUI
import UniformTypeIdentifiers

protocol ByteSource: AnyObject {
  var name: String { get }
  var contentType: String { get }
  func length() async throws -> Int64
  func read(offset: Int64, length: Int) async throws -> Data
}

/// A clip in Documents, read with pread.
final class FileSource: ByteSource {
  let url: URL
  private let fd: Int32
  init(url: URL) throws {
    self.url = url
    fd = open(url.path, O_RDONLY)
    if fd < 0 { throw NSError(domain: "S4", code: Int(errno)) }
  }
  deinit { close(fd) }
  var name: String { "file" }
  var contentType: String { UTType(filenameExtension: url.pathExtension)?.identifier ?? "public.mpeg-4" }
  func length() async throws -> Int64 { Int64(lseek(fd, 0, SEEK_END)) }
  func read(offset: Int64, length: Int) async throws -> Data {
    var data = Data(count: length)
    let n = data.withUnsafeMutableBytes { pread(fd, $0.baseAddress, length, off_t(offset)) }
    if n < 0 { throw NSError(domain: "S4", code: Int(errno)) }
    data.count = n
    return data
  }
}

/// nas/range_server.py, through the LAN or the real port forward. Stands in for the link.
final class HTTPSource: ByteSource {
  let url: URL
  private let session: URLSession
  private let lock = NSLock()
  private var knownLength: Task<Int64, Error>?
  init(url: URL) {
    self.url = url
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    session = URLSession(configuration: config)
  }
  var name: String { "http" }
  var contentType: String { UTType(filenameExtension: url.pathExtension)?.identifier ?? "public.mpeg-4" }

  private func fetch(_ range: String) async throws -> (Data, HTTPURLResponse) {
    var request = URLRequest(url: url)
    request.setValue(range, forHTTPHeaderField: "Range")
    let (data, response) = try await session.data(for: request)
    guard let http = response as? HTTPURLResponse, http.statusCode == 206 else {
      throw NSError(domain: "S4", code: (response as? HTTPURLResponse)?.statusCode ?? -1,
                    userInfo: [NSLocalizedDescriptionKey: "expected 206 from the range server"])
    }
    return (data, http)
  }

  /// Asked once: the loader wants it for every request, and a round trip per seek would be ours.
  func length() async throws -> Int64 {
    let task: Task<Int64, Error> = lock.withLock {
      if let knownLength { return knownLength }
      let started = Task { try await self.fetchLength() }
      knownLength = started
      return started
    }
    do {
      return try await task.value
    } catch {
      lock.withLock { knownLength = nil }
      throw error
    }
  }

  private func fetchLength() async throws -> Int64 {
    let (_, http) = try await fetch("bytes=0-0")
    guard let total = http.value(forHTTPHeaderField: "Content-Range")?.split(separator: "/").last,
      let n = Int64(total)
    else { throw NSError(domain: "S4", code: -2) }
    return n
  }

  func read(offset: Int64, length: Int) async throws -> Data {
    try await fetch("bytes=\(offset)-\(offset + Int64(length) - 1)").0
  }
}

/// What the player asked for, and how long the source took to answer.
final class LoaderStats {
  private let lock = NSLock()
  private var requests = 0
  private var cancelled = 0
  private var bytes: Int64 = 0
  private var reads: [Double] = []
  private var largest: Int64 = 0

  func request(length: Int64) { lock.withLock { requests += 1; largest = max(largest, length) } }
  func cancel() { lock.withLock { cancelled += 1 } }
  func read(ms: Double, bytes n: Int) { lock.withLock { reads.append(ms); bytes += Int64(n) } }
  func snapshot() -> [String: Any] {
    lock.withLock {
      [
        "loader_requests": requests,
        "loader_cancelled": cancelled,
        "bytes_served": bytes,
        "largest_request": largest,
        "read_count": reads.count,
        "read_p50_ms": Measure.rounded(Measure.percentile(reads, 0.5)),
        "read_p95_ms": Measure.rounded(Measure.percentile(reads, 0.95)),
      ]
    }
  }
}

final class RangeLoader: NSObject, AVAssetResourceLoaderDelegate {
  static let scheme = "mvspike"
  static let chunk = 512 * 1024

  let source: ByteSource
  let stats = LoaderStats()
  private let lock = NSLock()
  private var tasks: [ObjectIdentifier: Task<Void, Never>] = [:]

  init(source: ByteSource) { self.source = source }

  func resourceLoader(_ loader: AVAssetResourceLoader,
                      shouldWaitForLoadingOfRequestedResource request: AVAssetResourceLoadingRequest) -> Bool {
    let key = ObjectIdentifier(request)
    let task = Task.detached(priority: .userInitiated) { [self] in
      await serve(request)
      lock.withLock { _ = tasks.removeValue(forKey: key) }
    }
    lock.withLock { tasks[key] = task }
    return true
  }

  func resourceLoader(_ loader: AVAssetResourceLoader, didCancel request: AVAssetResourceLoadingRequest) {
    stats.cancel()
    lock.withLock { tasks[ObjectIdentifier(request)]?.cancel() }
  }

  private func serve(_ request: AVAssetResourceLoadingRequest) async {
    do {
      let total = try await source.length()
      if let info = request.contentInformationRequest {
        info.contentType = source.contentType
        info.contentLength = total
        info.isByteRangeAccessSupported = true
      }
      guard let data = request.dataRequest else {
        request.finishLoading()
        return
      }
      var offset = data.currentOffset != 0 ? data.currentOffset : data.requestedOffset
      let end = data.requestsAllDataToEndOfResource
        ? total : min(total, data.requestedOffset + Int64(data.requestedLength))
      stats.request(length: end - offset)
      while offset < end {
        if Task.isCancelled || request.isCancelled { return }
        let n = Int(min(Int64(Self.chunk), end - offset))
        let t0 = Measure.nowMs()
        let bytes = try await source.read(offset: offset, length: n)
        stats.read(ms: Measure.nowMs() - t0, bytes: bytes.count)
        if bytes.isEmpty { break }
        if Task.isCancelled || request.isCancelled { return }
        data.respond(with: bytes)
        offset += Int64(bytes.count)
      }
      request.finishLoading()
    } catch {
      if !request.isCancelled { request.finishLoading(with: error) }
    }
  }
}

/// Plays a clip through the loader, runs a fixed seek script and reports.
@MainActor
final class S4Run: ObservableObject {
  /// Long enough to see a link that starts well and then cannot keep up.
  static let sustainSeconds = 60.0

  @Published var player: AVPlayer?
  @Published var layerReady = false
  private var loader: RangeLoader?
  private var observers: [NSKeyValueObservation] = []

  /// The first .mp4 / .mov / .m4v in Documents.
  nonisolated static func localClip() -> URL? {
    let files = (try? FileManager.default.contentsOfDirectory(at: Report.documents, includingPropertiesForKeys: nil)) ?? []
    return files.sorted { $0.lastPathComponent < $1.lastPathComponent }
      .first { ["mp4", "mov", "m4v"].contains($0.pathExtension.lowercased()) }
  }

  private func wait(timeoutMs: Double, until: () -> Bool) async -> Double? {
    let t0 = Measure.nowMs()
    while Measure.nowMs() - t0 < timeoutMs {
      if until() { return Measure.nowMs() - t0 }
      try? await Task.sleep(nanoseconds: 5_000_000)
    }
    return nil
  }

  private func seek(_ player: AVPlayer, to time: CMTime, timeoutMs: Double) async -> Bool? {
    final class Answer { var finished: Bool? }
    let answer = Answer()
    player.seek(to: time, toleranceBefore: .zero, toleranceAfter: .zero) { finished in
      DispatchQueue.main.async { answer.finished = finished }
    }
    _ = await wait(timeoutMs: timeoutMs) { answer.finished != nil }
    return answer.finished
  }

  func run(source: ByteSource, trigger: String) async {
    let report = Report.shared
    report.enter("s4.\(source.name)")
    defer { report.leave() }

    let loader = RangeLoader(source: source)
    self.loader = loader
    let url = URL(string: "\(RangeLoader.scheme)://clip/test")!
    let asset = AVURLAsset(url: url)
    asset.resourceLoader.setDelegate(loader, queue: DispatchQueue(label: "m1.s4.loader"))
    let item = AVPlayerItem(asset: asset)
    let player = AVPlayer(playerItem: item)
    player.automaticallyWaitsToMinimizeStalling = true
    player.actionAtItemEnd = .none

    var stalls = 0
    let stallToken = NotificationCenter.default.addObserver(
      forName: .AVPlayerItemPlaybackStalled, object: item, queue: .main) { _ in stalls += 1 }
    defer { NotificationCenter.default.removeObserver(stallToken) }

    layerReady = false
    self.player = player
    let t0 = Measure.nowMs()
    player.play()

    guard let readyMs = await wait(timeoutMs: 30_000, until: { item.status != .unknown }), item.status == .readyToPlay else {
      report.emit("s4", ["source": source.name, "error": "\(item.error.map { "\($0)" } ?? "not ready in 30 s")"]
        .merging(loader.stats.snapshot()) { a, _ in a })
      self.player = nil
      return
    }
    let firstFrame = await wait(timeoutMs: 30_000) { self.layerReady }
    let firstFrameMs = firstFrame.map { $0 + readyMs } ?? -1

    // Play with no seeks first, while nothing is buffered beyond what the start needed. A stall
    // notification only fires when playback runs dry, so this also counts time spent waiting.
    let duration = item.duration.seconds
    let plannedSustain = duration.isFinite ? min(Self.sustainSeconds, max(0, duration - 2)) : 0
    let sustainStart = Measure.nowMs()
    let sustainFrom = player.currentTime().seconds
    var sampledAt = sustainStart
    var waitingMs = 0.0
    var waitingEvents = 0
    var wasWaiting = false
    while Measure.nowMs() - sustainStart < plannedSustain * 1000 {
      try? await Task.sleep(nanoseconds: 50_000_000)
      let now = Measure.nowMs()
      let waiting = player.timeControlStatus == .waitingToPlayAtSpecifiedRate
      if waiting {
        waitingMs += now - sampledAt
        if !wasWaiting { waitingEvents += 1 }
      }
      wasWaiting = waiting
      sampledAt = now
    }
    let sustainWallS = (Measure.nowMs() - sustainStart) / 1000
    let sustainPlayedS = player.currentTime().seconds - sustainFrom
    let sustainStalls = stalls
    let clipBytes = try? await source.length()

    var seeks: [[String: Any]] = []
    let script: [Double] = [0.5, 0.9, 0.1, 0.75, 0.25, 0.98, 0.0]
    if duration.isFinite && duration > 2 {
      for fraction in script {
        let target = CMTime(seconds: duration * fraction, preferredTimescale: 600)
        let start = Measure.nowMs()
        // Bounded, so a seek AVFoundation never answers is a result rather than a hang.
        let finished = await seek(player, to: target, timeoutMs: 20_000)
        let doneMs = Measure.nowMs() - start
        let landed = player.currentTime().seconds
        player.play()  // a seek near the end runs off it, and that pauses the player
        let moved = await wait(timeoutMs: 15_000) {
          player.timeControlStatus == .playing && player.currentTime().seconds > landed + 0.05
        }
        seeks.append([
          "fraction": fraction,
          "finished": finished ?? false,
          "timed_out": finished == nil,
          "seek_ms": Measure.rounded(doneMs),
          "seek_to_playing_ms": moved.map { Measure.rounded(doneMs + $0) } ?? -1,
        ])
        _ = await wait(timeoutMs: 1_000) { false }
      }
    }
    let seekTimes = seeks.compactMap { $0["seek_to_playing_ms"] as? Double }.filter { $0 >= 0 }

    var fields: [String: Any] = [
      "trigger": trigger,
      "source": source.name,
      "duration_s": Measure.rounded(duration),
      "ready_ms": Measure.rounded(readyMs),
      "first_frame_ms": Measure.rounded(firstFrameMs),
      "total_ms": Measure.rounded(Measure.nowMs() - t0),
      "seeks": seeks,
      "seeks_ok": seekTimes.count,
      "seek_to_playing_p50_ms": Measure.rounded(Measure.percentile(seekTimes, 0.5)),
      "seek_to_playing_max_ms": Measure.rounded(seekTimes.max() ?? -1),
      "stalls": stalls,
      "sustain_planned_s": Measure.rounded(plannedSustain),
      "sustain_limited_by_clip": plannedSustain < Self.sustainSeconds,
      "sustain_wall_s": Measure.rounded(sustainWallS),
      "sustain_played_s": Measure.rounded(sustainPlayedS),
      "sustain_waiting_ms": Measure.rounded(waitingMs),
      "sustain_waiting_events": waitingEvents,
      "sustain_stalls": sustainStalls,
      "clip_mbit": clipBytes.map { duration > 0 ? Measure.rounded(Double($0) * 8 / duration / 1e6) : -1 } ?? -1,
      "item_error": item.error.map { "\($0)" } ?? "",
    ]
    fields.merge(loader.stats.snapshot()) { a, _ in a }
    report.emit("s4", fields)
    player.pause()
    self.player = nil
    self.loader = nil
  }
}

struct PlayerLayerView: UIViewRepresentable {
  let player: AVPlayer
  let onReady: () -> Void

  final class View: UIView {
    override class var layerClass: AnyClass { AVPlayerLayer.self }
    var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
    var observation: NSKeyValueObservation?
  }

  func makeUIView(context: Context) -> View {
    let view = View()
    view.playerLayer.player = player
    view.playerLayer.videoGravity = .resizeAspect
    view.observation = view.playerLayer.observe(\.isReadyForDisplay, options: [.initial, .new]) { layer, _ in
      if layer.isReadyForDisplay { DispatchQueue.main.async { onReady() } }
    }
    return view
  }

  func updateUIView(_ view: View, context: Context) { view.playerLayer.player = player }
}
