// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §7, §8.3, §8.4: the paired library's connection and catalogue sync.
// `current` is known at once; everything that waits is on the link queue.
import BackgroundTasks
import CMobileLink
import Foundation
import MobileKit
import Network
import UIKit

private let copyTaskIdentifier = "io.github.longtimeno-c.mediaviewer.mobile.first-copy"

public final class LinkConnection: ConnectionService, CatalogueSync, @unchecked Sendable {
  private let lock = NSLock()
  private var snapshot: ConnectionSnapshot
  private var continuations: [UUID: AsyncStream<ConnectionSnapshot>.Continuation] = [:]
  private var handle: LinkHandle?
  private var record: PairingRecord?
  private var pathExpensive = false
  private var connectGeneration = 0
  private var continued: AnyObject?  // BGContinuedProcessingTask, on iOS 26

  private let monitor = NWPathMonitor()
  private let defaults = UserDefaults.standard

  public let continuesAfterLeaving: Bool

  public init() {
    if #available(iOS 26.0, *) { continuesAfterLeaving = true } else { continuesAfterLeaving = false }
    let stored = defaults.object(forKey: Self.catalogueKey) as? [String: Any]
    let record = PairingRecord.load()
    self.record = record
    snapshot = record == nil
      ? .noLibrary
      : ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false,
                           catalogue: Self.catalogue(from: stored), cacheBytes: 0)
    if let record, let device = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }) {
      handle = LinkHandle.open(record: record, deviceKey: device, libraryDir: LinkFolders.library,
                               cacheDir: LinkFolders.thumbs)
    }
    monitor.pathUpdateHandler = { [weak self] path in
      guard let self else { return }
      self.lock.withLock { self.pathExpensive = path.isExpensive }
      Task { await self.retry() }  // a new network: race the routes again (§7.3)
    }
    monitor.start(queue: DispatchQueue(label: "mediaviewer.link.path"))
    NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { [weak self] _ in
      guard let self else { return }
      Task { await self.retry() }
    }
    Task { await self.retry() }
  }

  deinit { monitor.cancel() }

  /// Must run before the app finishes launching (Apple's rule for task registration).
  public static func registerBackgroundWork() {
    guard #available(iOS 26.0, *) else { return }
    _ = BGTaskScheduler.shared.register(forTaskWithIdentifier: copyTaskIdentifier, using: nil) { task in
      guard let task = task as? BGContinuedProcessingTask else { task.setTaskCompleted(success: false); return }
      LinkConnection.shared?.adopt(task)
    }
  }

  /// The service the app runs with; the background handler reaches it through this.
  public nonisolated(unsafe) static weak var shared: LinkConnection?
  public func becomeShared() { LinkConnection.shared = self }

  var link: LinkHandle? { lock.withLock { handle } }
  var hasPairing: Bool { lock.withLock { record != nil } }

  public var current: ConnectionSnapshot { lock.withLock { snapshot } }

  public func updates() -> AsyncStream<ConnectionSnapshot> {
    let id = UUID()
    return AsyncStream { continuation in
      lock.withLock {
        continuations[id] = continuation
      }
      continuation.yield(current)
      continuation.onTermination = { [weak self] _ in
        guard let self else { return }
        self.lock.withLock { self.continuations[id] = nil }
      }
    }
  }

  private func publish(_ change: (inout ConnectionSnapshot) -> Void) {
    let (value, list): (ConnectionSnapshot, [AsyncStream<ConnectionSnapshot>.Continuation]) = lock.withLock {
      var next = snapshot
      change(&next)
      snapshot = next
      return (next, Array(continuations.values))
    }
    if let info = value.catalogue {
      defaults.set(["updatedAt": info.updatedAt.timeIntervalSince1970, "itemCount": info.itemCount], forKey: Self.catalogueKey)
    } else {
      defaults.removeObject(forKey: Self.catalogueKey)
    }
    list.forEach { $0.yield(value) }
  }

  private static let catalogueKey = "link.catalogue"

  private static func catalogue(from stored: [String: Any]?) -> CatalogueInfo? {
    guard let stored, let at = stored["updatedAt"] as? TimeInterval, let count = stored["itemCount"] as? Int else { return nil }
    return CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: at), itemCount: count)
  }

  // MARK: Identify and pair

  public func identify(code: String) async throws -> LibraryIdentity {
    try await onLinkQueue {
      var info = mv_pair_info()
      let status = code.withCString { mv_pair_parse($0, Int64(Date().timeIntervalSince1970), &info) }
      if status == 2 { throw PairingFailure.codeExpired }
      guard status == 0 else { throw PairingFailure.codeNotReadable }
      return LibraryIdentity(name: linkString(info.library_name), fingerprint: linkString(info.fingerprint))
    }
  }

  public func pair(code: String, confirmed: LibraryIdentity) -> AsyncThrowingStream<PairingEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          try await runPairing(code: code, confirmed: confirmed, continuation: continuation)
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  private func runPairing(code: String, confirmed: LibraryIdentity,
                          continuation: AsyncThrowingStream<PairingEvent, Error>.Continuation) async throws {
    let parsed: mv_pair_info = try await onLinkQueue {
      var info = mv_pair_info()
      let status = code.withCString { mv_pair_parse($0, Int64(Date().timeIntervalSince1970), &info) }
      if status == 2 { throw PairingFailure.codeExpired }
      guard status == 0 else { throw PairingFailure.codeNotReadable }
      return info
    }
    // What the person compared with the desktop must be what this code says (§7.4).
    guard linkString(parsed.library_name) == confirmed.name, linkString(parsed.fingerprint) == confirmed.fingerprint else {
      throw PairingFailure.codeNotReadable
    }

    continuation.yield(.working(.mac))
    continuation.yield(.passed(.mac))

    continuation.yield(.working(.synology))
    guard let deviceKey = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }) else { throw PairingFailure.codeNotReadable }
    let deviceName = await deviceName()
    let (status, report) = await withLinkCancel { cancel in
      await onLinkQueue {
        var report = mv_route_report()
        var info = parsed
        let status = deviceKey.withUnsafeBytes {
          mv_pair_redeem(&info, $0.bindMemory(to: UInt8.self).baseAddress, deviceName, cancel.raw, &report)
        }
        return (status, report)
      }
    }
    switch status {
    case 0: break
    case 2, 6: throw PairingFailure.codeExpired
    case 7: throw PairingFailure.serviceTooOld
    case 11: throw CancellationError()
    default: throw PairingFailure.synologyUnreachable(Self.failures(of: report))
    }

    let record = PairingRecord(libraryID: linkString(parsed.library_id), libraryName: linkString(parsed.library_name),
                               fingerprint: linkString(parsed.fingerprint), publisherKey: linkBytes(parsed.publisher_key).hexString,
                               local: linkString(parsed.local), publicName: linkString(parsed.public_name),
                               identity: linkBytes(parsed.identity).hexString, accessKey: linkBytes(parsed.access_key).hexString,
                               deviceName: deviceName)
    guard record.save() else { throw PairingFailure.codeNotReadable }
    guard let handle = LinkHandle.open(record: record, deviceKey: deviceKey, libraryDir: LinkFolders.library, cacheDir: LinkFolders.thumbs) else {
      throw PairingFailure.codeNotReadable
    }
    lock.withLock {
      self.record = record
      self.handle = handle
    }
    publish { $0 = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0) }
    continuation.yield(.passed(.synology))

    continuation.yield(.working(.outsideRoute))
    let outside = await onLinkQueue {
      var info = parsed
      return deviceKey.withUnsafeBytes { mv_pair_public_answers(&info, $0.bindMemory(to: UInt8.self).baseAddress, deviceName) }
    }
    continuation.yield(.outsideRoute(outside != 0 ? .reachable : .reachableOnlyOnItsOwnNetwork))
    continuation.yield(.passed(.outsideRoute))
    await retry()
  }

  private func deviceName() async -> String { await MainActor.run { UIDevice.current.name } }

  // MARK: Connection

  public func retry() async {
    guard let link = link else { return }
    let generation = lock.withLock {
      connectGeneration += 1
      return connectGeneration
    }
    let (status, report) = await onLinkQueue {
      var report = mv_route_report()
      let status = mv_link_connect(link.raw, nil, &report)
      return (status, report)
    }
    guard lock.withLock({ generation == connectGeneration }) else { return }  // a newer one is running
    let info = await catalogueNow(link: link)
    let cache = Int64(mv_thumb_bytes_used(link.raw))
    publish {
      $0 = ConnectionSnapshot(hasLibrary: true, synology: Self.state(status, report), macNearby: false,
                              catalogue: info ?? $0.catalogue, cacheBytes: cache)
    }
    if status == 0 { Task { await self.quietSync(link) } }
  }

  /// A newer generation, on Wi-Fi only: the catalogue the person is using stays until it is ready.
  private func quietSync(_ link: LinkHandle) async {
    guard current.catalogue != nil, !lock.withLock({ pathExpensive }) else { return }
    let report = await onLinkQueue { () -> mv_sync_report in
      var report = mv_sync_report()
      _ = mv_sync_check(link.raw, &report)
      return report
    }
    if report.outcome == 12 {  // not signed by the key this phone pinned: say so (§11.3)
      publish { $0 = ConnectionSnapshot(hasLibrary: $0.hasLibrary, synology: .publisherChanged, macNearby: false, catalogue: $0.catalogue, cacheBytes: $0.cacheBytes) }
      return
    }
    guard report.outcome == 16 else { return }
    let done = await onLinkQueue { () -> Int32 in
      var r = mv_sync_report()
      return mv_sync_copy(link.raw, nil, nil, nil, &r)
    }
    if done == 0, let info = await catalogueNow(link: link) {
      publish { $0 = ConnectionSnapshot(hasLibrary: $0.hasLibrary, synology: $0.synology, macNearby: false, catalogue: info, cacheBytes: $0.cacheBytes) }
    }
  }

  public func forget() async {
    let link = lock.withLock { () -> LinkHandle? in
      let h = handle
      handle = nil
      record = nil
      return h
    }
    await onLinkQueue {
      if let link { mv_link_disconnect(link.raw) }
      mv_library_remove(LinkFolders.library.path)
      try? FileManager.default.removeItem(at: LinkFolders.thumbs)
      try? FileManager.default.removeItem(at: LinkFolders.originals)
      try? FileManager.default.removeItem(at: LinkFolders.clips)
    }
    PairingRecord.clear()
    DeviceKey.clear()
    publish { $0 = .noLibrary }
  }

  private func catalogueNow(link: LinkHandle) async -> CatalogueInfo? {
    await onLinkQueue {
      var info = mv_catalogue_info()
      guard mv_catalogue_read(link.raw, &info) == 0, info.present != 0 else { return nil }
      return CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: TimeInterval(info.created_unix)),
                           itemCount: Int(info.asset_count))
    }
  }

  private static func state(_ status: Int32, _ report: mv_route_report) -> SynologyState {
    switch status {
    case 0: return .reachable(address: linkString(report.address), overInternet: report.over_internet != 0)
    case 7: return .serviceTooOld
    default: return .unreachable(failures(of: report))
    }
  }

  private static func failures(of report: mv_route_report) -> [AddressFailure] {
    withUnsafeBytes(of: report.failures) { raw in
      let list = raw.bindMemory(to: mv_route_failure.self)
      return (0..<Int(report.failure_count)).map { i in
        let f = list[i]
        return AddressFailure(address: linkString(f.address), reason: reasonText(f.problem))
      }
    }
  }

  private static func reasonText(_ problem: Int32) -> String {
    switch problem {
    case 1: return "A different service answered"
    case 2: return "This phone is no longer paired"
    case 3: return "The service needs updating"
    case 4: return "Too many phones are connected"
    default: return "Did not answer"
    }
  }

  // MARK: First copy

  public func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          try await runCopy(overMobileData: overMobileData, continuation: continuation)
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  private func runCopy(overMobileData: Bool, continuation: AsyncThrowingStream<CopyEvent, Error>.Continuation) async throws {
    guard let link = link else { throw CopyFailure.synologyUnreachable }
    // The link may be new, or gone since the last network change.
    if mv_link_is_connected(link.raw) == 0 {
      let status = await onLinkQueue { () -> Int32 in
        var report = mv_route_report()
        return mv_link_connect(link.raw, nil, &report)
      }
      guard status == 0 else { throw CopyFailure.synologyUnreachable }
    }
    let check = await onLinkQueue { () -> mv_sync_report in
      var report = mv_sync_report()
      _ = mv_sync_check(link.raw, &report)
      return report
    }
    switch check.outcome {
    case 15, 14:
      if let info = await catalogueNow(link: link) { publishCatalogue(info) }
      return
    case 16: break
    case 12: throw CopyFailure.catalogueRejected
    case 13: throw CopyFailure.notEnoughSpace(neededBytes: Int64(check.needed_bytes))
    default: throw CopyFailure.synologyUnreachable
    }

    // Size first; nothing moves over mobile data unless the person asked (§8.4).
    if lock.withLock({ pathExpensive }) && !overMobileData {
      continuation.yield(.waitingForWiFi(totalBytes: Int64(check.total_bytes)))
      while lock.withLock({ pathExpensive }) {
        try Task.checkCancellation()
        try await Task.sleep(for: .milliseconds(500))
      }
    }

    if check.done_bytes > 0 {
      continuation.yield(.resuming(doneBytes: Int64(check.done_bytes), totalBytes: Int64(check.total_bytes)))
    } else {
      continuation.yield(.copying(doneBytes: 0, totalBytes: Int64(check.total_bytes)))
    }

    let wantsContinued = continuesAfterLeaving
    if wantsContinued { submitContinuedTask() }

    let progressBox = CopyProgress { [weak self] done, total in
      continuation.yield(.copying(doneBytes: Int64(done), totalBytes: Int64(total)))
      self?.reportContinued(done: done, total: total)
    }
    let (status, report) = await withLinkCancel { cancel in
      await onLinkQueue {
        var report = mv_sync_report()
        let box = Unmanaged.passRetained(progressBox)
        let status = mv_sync_copy(link.raw, cancel.raw, { ctx, done, total in
          guard let ctx else { return }
          Unmanaged<CopyProgress>.fromOpaque(ctx).takeUnretainedValue().report(done, total)
        }, box.toOpaque(), &report)
        box.release()
        return (status, report)
      }
    }
    finishContinued(success: status == 0)
    switch status {
    case 0:
      if let info = await catalogueNow(link: link) { publishCatalogue(info) }
    case 11: throw CancellationError()
    case 12: throw CopyFailure.catalogueRejected
    case 13: throw CopyFailure.notEnoughSpace(neededBytes: Int64(report.needed_bytes))
    default: throw CopyFailure.synologyUnreachable
    }
  }

  private func publishCatalogue(_ info: CatalogueInfo) {
    publish { $0 = ConnectionSnapshot(hasLibrary: $0.hasLibrary, synology: $0.synology, macNearby: false, catalogue: info, cacheBytes: $0.cacheBytes) }
  }

  // MARK: Continued processing (iOS 26)

  private func submitContinuedTask() {
    guard #available(iOS 26.0, *) else { return }
    let request = BGContinuedProcessingTaskRequest(identifier: copyTaskIdentifier, title: "Copying your library",
                                                   subtitle: "MediaViewer is bringing your catalogue to this phone.")
    request.strategy = .fail
    // Refused where the system does not allow it (the Simulator, or one already running): the
    // copy still runs while the app is open.
    try? BGTaskScheduler.shared.submit(request)
  }

  @available(iOS 26.0, *)
  fileprivate func adopt(_ task: BGContinuedProcessingTask) {
    lock.withLock { continued = task }
    task.expirationHandler = { [weak self] in
      self?.lock.withLock { self?.continued = nil }
    }
  }

  private func reportContinued(done: UInt64, total: UInt64) {
    guard #available(iOS 26.0, *) else { return }
    guard let task = lock.withLock({ continued as? BGContinuedProcessingTask }) else { return }
    task.progress.totalUnitCount = Int64(total)
    task.progress.completedUnitCount = Int64(done)
  }

  private func finishContinued(success: Bool) {
    guard #available(iOS 26.0, *) else { return }
    let task = lock.withLock { () -> BGContinuedProcessingTask? in
      defer { continued = nil }
      return continued as? BGContinuedProcessingTask
    }
    task?.setTaskCompleted(success: success)
  }
}

private final class CopyProgress: @unchecked Sendable {
  private let body: (UInt64, UInt64) -> Void
  private var last: UInt64 = 0

  init(_ body: @escaping (UInt64, UInt64) -> Void) { self.body = body }

  func report(_ done: UInt64, _ total: UInt64) {
    // A few a second is enough for a progress line.
    guard done == total || done &- last > 512 * 1024 else { return }
    last = done
    body(done, total)
  }
}

extension NSLock {
  func withLock<T>(_ body: () -> T) -> T {
    lock()
    defer { unlock() }
    return body()
  }
}
