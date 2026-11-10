// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §7, §8.3, §8.4: the paired library's connection and catalogue sync.
// `current` is known at once from a stored value; the Keychain, the files and the network are only
// ever touched on the link queue.
import BackgroundTasks
import CMobileLink
import Foundation
import MobileKit
import MobileLinkSupport
import Network
import UIKit

private let copyTaskIdentifier = "io.github.longtimeno-c.mediaviewer.mobile.first-copy"

public final class LinkConnection: ConnectionService, CatalogueSync, @unchecked Sendable {
  private let lock = NSLock()
  private var snapshot: ConnectionSnapshot
  private var continuations: [UUID: AsyncStream<ConnectionSnapshot>.Continuation] = [:]
  private var handle: LinkHandle?
  private var record: PairingRecord?
  // Until the first answer from the path monitor the network is treated as mobile data (§8.4).
  private var pathKnown = false
  private var pathExpensive = true
  private var pathSignature = ""
  private var routeEpoch = 0
  private var connectGeneration = 0
  private var forgotten = false
  private var copyCancel: LinkCancel?
  // Everything that writes into the library: Forget cancels it and waits before deleting.
  private let work = TrackedWork()
  private var continued: AnyObject?  // BGContinuedProcessingTask, on iOS 26
  private var opened: Task<Void, Never>?

  private let monitor = NWPathMonitor()
  private let defaults = UserDefaults.standard
  private static let pairedKey = "link.paired"
  private static let catalogueKey = "link.catalogue"

  public let continuesAfterLeaving: Bool

  public init() {
    if #available(iOS 26.0, *) { continuesAfterLeaving = true } else { continuesAfterLeaving = false }
    // What launch paints first comes from a stored value: no Keychain, no files on the main thread.
    let paired = defaults.bool(forKey: Self.pairedKey)
    snapshot = paired
      ? ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false,
                           catalogue: Self.catalogue(from: defaults.object(forKey: Self.catalogueKey) as? [String: Any]),
                           cacheBytes: 0)
      : .noLibrary
    opened = Task.detached(priority: .userInitiated) { [weak self] in await self?.openStored() }
    monitor.pathUpdateHandler = { [weak self] path in self?.pathChanged(path) }
    monitor.start(queue: DispatchQueue(label: "mediaviewer.link.path"))
    NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { [weak self] _ in
      guard let self else { return }
      Task { await self.retry() }  // keeps a live link; races only when there is none
    }
  }

  deinit { monitor.cancel() }

  private func openStored() async {
    let opened: (PairingRecord, LinkHandle)? = await onLinkQueue {
      guard let record = PairingRecord.load(), let device = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }) else { return nil }
      LinkFolders.prepare()
      guard let handle = LinkHandle.open(record: record, deviceKey: device, libraryDir: LinkFolders.library, cacheDir: LinkFolders.thumbs) else {
        return nil
      }
      return (record, handle)
    }
    if let (record, handle) = opened {
      lock.withLock {
        self.record = record
        self.handle = handle
      }
      if !current.hasLibrary { publish { $0 = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0) } }
      // Not awaited: connecting waits for this open to finish, so awaiting it here would deadlock.
      Task { await self.retry() }
    } else if current.hasLibrary {
      publish { $0 = .noLibrary }  // the Keychain no longer has it (restored to another phone)
    }
  }

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

  /// The link once launch has opened it (or found there is none).
  func awaitLink() async -> LinkHandle? {
    await lock.withLock({ opened })?.value
    return link
  }

  /// Bumped on every connect and network change: anything measured on the old route is stale.
  var epoch: Int { lock.withLock { routeEpoch } }

  /// Whether catalogue bytes may move now without the person's say-so.
  private var mayCopyQuietly: Bool { lock.withLock { pathKnown && !pathExpensive } }

  public var current: ConnectionSnapshot { lock.withLock { snapshot } }

  public func updates() -> AsyncStream<ConnectionSnapshot> {
    let id = UUID()
    return AsyncStream { continuation in
      lock.withLock { continuations[id] = continuation }
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
    defaults.set(value.hasLibrary, forKey: Self.pairedKey)
    if let info = value.catalogue {
      defaults.set(["updatedAt": info.updatedAt.timeIntervalSince1970, "itemCount": info.itemCount], forKey: Self.catalogueKey)
    } else {
      defaults.removeObject(forKey: Self.catalogueKey)
    }
    list.forEach { $0.yield(value) }
  }

  private static func catalogue(from stored: [String: Any]?) -> CatalogueInfo? {
    guard let stored, let at = stored["updatedAt"] as? TimeInterval, let count = stored["itemCount"] as? Int else { return nil }
    return CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: at), itemCount: count)
  }

  // MARK: The network

  private func pathChanged(_ path: NWPath) {
    let signature = "\(path.status)|" + path.availableInterfaces.map { "\($0.type)-\($0.name)" }.joined(separator: ",")
    let (first, changed) = lock.withLock { () -> (Bool, Bool) in
      let first = !pathKnown
      let changed = !first && signature != pathSignature
      pathKnown = true
      pathExpensive = path.isExpensive
      pathSignature = signature
      if changed { routeEpoch += 1 }
      return (first, changed)
    }
    guard path.status == .satisfied else { return }
    if first {
      Task { await self.retry() }
    } else if changed {
      Task { await self.reconnect() }  // a new network: race again (§7.3); reads finish on the old link
    }
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

    // Pairing with the Mac itself waits for nearby sync (M4b): the code names only the service.
    continuation.yield(.skipped(.mac))

    continuation.yield(.working(.synology))
    let deviceName = await deviceName()
    let deviceKey: Data = try await onLinkQueue {
      guard let key = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }) else { throw PairingFailure.codeNotReadable }
      return key
    }
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
    let handle: LinkHandle = try await onLinkQueue {
      guard record.save() else { throw PairingFailure.codeNotReadable }
      LinkFolders.prepare()  // again: a Forget removed them, with their backup exclusion
      guard let handle = LinkHandle.open(record: record, deviceKey: deviceKey, libraryDir: LinkFolders.library, cacheDir: LinkFolders.thumbs) else {
        throw PairingFailure.codeNotReadable
      }
      return handle
    }
    lock.withLock {
      self.record = record
      self.handle = handle
      self.forgotten = false
    }
    work.reopen()
    publish { $0 = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0) }
    continuation.yield(.passed(.synology))

    continuation.yield(.working(.outsideRoute))
    continuation.yield(.outsideRoute(await OutsideRoute.test(publicName: record.publicName)))
    await retry()
  }

  private func deviceName() async -> String { await MainActor.run { UIDevice.current.name } }

  // MARK: Connection

  public func retry() async { await connect(replacing: false) }

  private func reconnect() async { await connect(replacing: true) }

  private func connect(replacing: Bool) async {
    guard let link = await awaitLink(), !lock.withLock({ forgotten }) else { return }
    let generation = lock.withLock {
      connectGeneration += 1
      if replacing { routeEpoch += 1 }
      return connectGeneration
    }
    let (status, report) = await onLinkQueue {
      var report = mv_route_report()
      let status = replacing ? mv_link_reconnect(link.raw, nil, &report) : mv_link_connect(link.raw, nil, &report)
      return (status, report)
    }
    guard lock.withLock({ generation == connectGeneration && !forgotten }) else { return }  // a newer one is running
    let info = await catalogueNow(link: link)
    let cache = Int64(mv_thumb_bytes_used(link.raw))
    publish {
      $0 = ConnectionSnapshot(hasLibrary: true, synology: Self.state(status, report), macNearby: false,
                              catalogue: info ?? $0.catalogue, cacheBytes: cache)
    }
    if status == 0 { work.run(priority: .utility) { [weak self] in await self?.quietSync(link) } }
  }

  /// A newer generation, on Wi-Fi only: the catalogue the person is using stays until it is ready.
  private func quietSync(_ link: LinkHandle) async {
    guard current.catalogue != nil, mayCopyQuietly else { return }
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
    let (done, _) = await copy(link: link, cancel: nil, allowExpensive: false, progress: nil)
    if done == 0, let info = await catalogueNow(link: link) { publishCatalogue(info) }
  }

  public func forget() async {
    // Stop first, wait, then delete: a copy still running (or waiting for Wi-Fi) must not write
    // into a wiped library, and must not keep Forget waiting.
    let (link, cancel) = lock.withLock { () -> (LinkHandle?, LinkCancel?) in
      forgotten = true
      let out = (handle, copyCancel)
      handle = nil
      record = nil
      return out
    }
    cancel?.fire()
    if let link { mv_link_shut_down(link.raw) }
    await work.stopAndWait()
    await onLinkQueue {
      mv_library_remove(LinkFolders.library.path)
      try? FileManager.default.removeItem(at: LinkFolders.thumbs)
      try? FileManager.default.removeItem(at: LinkFolders.originals)
      try? FileManager.default.removeItem(at: LinkFolders.clips)
      PairingRecord.clear()
      DeviceKey.clear()
    }
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
    case 5: return "The pairing code was not accepted"
    default: return "Did not answer"
    }
  }

  // MARK: Copying

  /// One mv_sync_copy. `allowExpensive`: the person said mobile data is fine. The core asks before
  /// each file and with each chunk, so bytes stop the moment the phone leaves Wi-Fi.
  private func copy(link: LinkHandle, cancel: LinkCancel?, allowExpensive: Bool,
                    progress: ((UInt64, UInt64) -> Void)?) async -> (Int32, mv_sync_report) {
    let box = CopyCallbacks(progress: progress) { [weak self] in
      guard let self else { return false }
      return self.lock.withLock { !self.forgotten && self.pathKnown && (allowExpensive || !self.pathExpensive) }
    }
    return await onLinkQueue {
      var report = mv_sync_report()
      let context = Unmanaged.passRetained(box)
      let status = mv_sync_copy(link.raw, cancel?.raw, { ctx in
        guard let ctx else { return 0 }
        return Unmanaged<CopyCallbacks>.fromOpaque(ctx).takeUnretainedValue().allowed() ? 1 : 0
      }, { ctx, done, total in
        guard let ctx else { return }
        Unmanaged<CopyCallbacks>.fromOpaque(ctx).takeUnretainedValue().report(done, total)
      }, context.toOpaque(), &report)
      context.release()
      return (status, report)
    }
  }

  public func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = work.run { [self] in
        do {
          try await runCopy(overMobileData: overMobileData, continuation: continuation)
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      if task == nil { continuation.finish(throwing: CancellationError()) }  // forgotten meanwhile
      continuation.onTermination = { _ in task?.cancel() }
    }
  }

  private func runCopy(overMobileData: Bool, continuation: AsyncThrowingStream<CopyEvent, Error>.Continuation) async throws {
    guard let link = await awaitLink(), !lock.withLock({ forgotten }) else { throw CopyFailure.synologyUnreachable }
    let status = await onLinkQueue { () -> Int32 in
      var report = mv_route_report()
      return mv_link_connect(link.raw, nil, &report)  // keeps a live link
    }
    guard status == 0 else { throw CopyFailure.synologyUnreachable }
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

    if continuesAfterLeaving { submitContinuedTask() }
    // However this ends (done, failed, cancelled while waiting for Wi-Fi), the system's
    // continued task hears it, or it would never complete.
    var succeeded = false
    defer {
      finishContinued(success: succeeded)
      lock.withLock { copyCancel = nil }
    }
    var started = false
    while true {
      // Size first; nothing moves over mobile data unless the person asked (§8.4).
      try await CopyGate.wait(
        mayMove: { [weak self] in overMobileData || (self?.mayCopyQuietly ?? false) },
        stopped: { [weak self] in self?.lock.withLock { self?.forgotten ?? true } ?? true },
        waiting: { continuation.yield(.waitingForWiFi(totalBytes: Int64(check.total_bytes))) })
      let cancel = LinkCancel()
      lock.withLock { copyCancel = cancel }
      if check.done_bytes > 0 || started {
        continuation.yield(.resuming(doneBytes: Int64(check.done_bytes), totalBytes: Int64(check.total_bytes)))
      } else {
        continuation.yield(.copying(doneBytes: 0, totalBytes: Int64(check.total_bytes)))
      }
      started = true
      let (status, report) = await withTaskCancellationHandler {
        await copy(link: link, cancel: cancel, allowExpensive: overMobileData) { [weak self] done, total in
          continuation.yield(.copying(doneBytes: Int64(done), totalBytes: Int64(total)))
          self?.reportContinued(done: done, total: total)
        }
      } onCancel: { cancel.fire() }
      switch status {
      case 0, 15:
        succeeded = true
        if let info = await catalogueNow(link: link) { publishCatalogue(info) }
        return
      case 19: continue  // left Wi-Fi: wait for it, then pick up where it stopped
      case 11:
        throw CancellationError()
      case 12:
        throw CopyFailure.catalogueRejected
      case 13:
        throw CopyFailure.notEnoughSpace(neededBytes: Int64(report.needed_bytes))
      default:
        throw CopyFailure.synologyUnreachable
      }
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
      guard let self else { return }
      self.lock.withLock { self.copyCancel }?.fire()
      self.lock.withLock { self.continued = nil }
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

/// The outside test (§2.2): a connection to the public name over mobile data only. An iPad with no
/// mobile data, or Wi-Fi only right now, says it skipped the test rather than testing Wi-Fi.
enum OutsideRoute {
  static func test(publicName: String) async -> OutsideRouteResult {
    // No public name: the service was set up for its own network only.
    guard let (host, port) = split(publicName) else { return .reachableOnlyOnItsOwnNetwork }
    guard await hasCellular() else { return .skippedNoMobileData }
    return await withCheckedContinuation { continuation in
      let parameters = NWParameters.tcp
      parameters.requiredInterfaceType = .cellular
      let connection = NWConnection(host: NWEndpoint.Host(host), port: port, using: parameters)
      let once = OnceFlag()
      let queue = DispatchQueue(label: "mediaviewer.link.outside")
      connection.stateUpdateHandler = { state in
        switch state {
        case .ready:
          if once.take() { connection.cancel(); continuation.resume(returning: .reachable) }
        case .failed, .waiting:
          if once.take() { connection.cancel(); continuation.resume(returning: .reachableOnlyOnItsOwnNetwork) }
        default: break
        }
      }
      connection.start(queue: queue)
      queue.asyncAfter(deadline: .now() + 6) {
        if once.take() { connection.cancel(); continuation.resume(returning: .reachableOnlyOnItsOwnNetwork) }
      }
    }
  }

  private static func hasCellular() async -> Bool {
    await withCheckedContinuation { continuation in
      let monitor = NWPathMonitor(requiredInterfaceType: .cellular)
      let queue = DispatchQueue(label: "mediaviewer.link.cellular")
      let once = OnceFlag()
      monitor.pathUpdateHandler = { path in
        if once.take() { monitor.cancel(); continuation.resume(returning: path.status == .satisfied) }
      }
      monitor.start(queue: queue)
      queue.asyncAfter(deadline: .now() + 1) {
        if once.take() { monitor.cancel(); continuation.resume(returning: false) }
      }
    }
  }

  private static func split(_ text: String) -> (String, NWEndpoint.Port)? {
    guard let colon = text.lastIndex(of: ":"), let port = NWEndpoint.Port(String(text[text.index(after: colon)...])) else { return nil }
    var host = String(text[..<colon])
    if host.hasPrefix("[") && host.hasSuffix("]") { host = String(host.dropFirst().dropLast()) }
    return host.isEmpty ? nil : (host, port)
  }
}

private final class OnceFlag: @unchecked Sendable {
  private let lock = NSLock()
  private var taken = false
  func take() -> Bool { lock.withLock { defer { taken = true }; return !taken } }
}


private final class CopyCallbacks: @unchecked Sendable {
  private let progress: ((UInt64, UInt64) -> Void)?
  let allowed: () -> Bool
  private var last: UInt64 = 0

  init(progress: ((UInt64, UInt64) -> Void)?, allowed: @escaping () -> Bool) {
    self.progress = progress
    self.allowed = allowed
  }

  func report(_ done: UInt64, _ total: UInt64) {
    // A few a second is enough for a progress line.
    guard let progress, done == total || done &- last > 512 * 1024 else { return }
    last = done
    progress(done, total)
  }
}

extension NSLock {
  func withLock<T>(_ body: () -> T) -> T {
    lock()
    defer { unlock() }
    return body()
  }
}
