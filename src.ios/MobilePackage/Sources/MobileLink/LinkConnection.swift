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
  // Under `lock`, as every write of it is: the core is told the network and the setting at once.
  private var handle: LinkHandle? { didSet { if let handle { Self.applyUpgrade(handle, mobileData: pathExpensive) } } }
  private var record: PairingRecord?
  // Until the first answer from the path monitor the network is treated as mobile data (§8.4).
  private var pathKnown = false
  private var pathExpensive = true
  private var pathSignature = ""
  private var routeEpoch = 0
  private var connectGeneration = 0
  private var forgotten = false
  /// The local copy was dropped for an access that ended (dropLocalCopy); cleared by a connection that gets in.
  private var localCopyDropped = false
  /// Bumped whenever the library is removed (Forget, or a pairing over it): work begun for the
  /// library before cannot touch the one after.
  private var libraryEpoch = 0
  /// The sample guest's end a connect is already scheduled for.
  private var sampleEndScheduled: Double? = nil
  /// File mode (docs/plans/phone-in-the-app.md §4): the last check found this library's folders but
  /// no catalogue. Stamped on every snapshot with no catalogue, as the library's id is.
  private var fileModeNow = false
  private var copyCancel: LinkCancel?
  // Sound search's download (§9): its state as published, one run at a time, the person's say-so
  // for mobile data (until the download ends), and its cancel for Forget.
  private var soundState: SoundSearchState = .notNeeded
  private var soundRunning = false
  private var soundAgain = false  // asked while a run was going: run once more when it ends
  private var soundOverMobileData = false
  private var soundCancel: LinkCancel?
  // One first copy at a time: a new one (Copy over mobile data) waits for the old one's defer.
  private let oneCopy = OneAtATime()
  // Everything that writes into the library: Forget cancels it and waits before deleting.
  private let work = TrackedWork()
  private let continued = ContinuedCopy()  // the BGContinuedProcessingTask's bookkeeping (iOS 26)
  private var opened: Task<Void, Never>?
  /// Each write into the library's folders runs under it (`whileLive`); Forget closes it before it
  /// removes them. Opened for each handle the library is opened with.
  private let writes = WriteGate()
  /// Phone backup (1.1): stopped, and its ledger and the sample library's on-phone folder removed,
  /// before Forget (or a new pairing over this library) removes the rest.
  private var beforeRemoval: (@Sendable () async -> Void)?
  func setBeforeRemoval(_ body: @escaping @Sendable () async -> Void) { lock.withLock { beforeRemoval = body } }

  private let monitor = NWPathMonitor()
  private let defaults = UserDefaults.standard
  private static let pairedKey = "link.paired"
  private static let catalogueKey = "link.catalogue"
  private static let libraryKey = "link.libraryID"
  private static let sampleKey = "link.sample"
  /// The last word was "your access ended": launch opens on the ended card, not a first copy.
  private static let endedKey = "link.accessEnded"
  private static let nameKey = "link.libraryName"
  private static let fileModeKey = "link.fileMode"

  /// The paired library's id as last published; "" with none or before the first publish. A plain
  /// stored value, cheap enough for launch.
  public static var storedLibraryID: String { UserDefaults.standard.string(forKey: libraryKey) ?? "" }

  public let continuesAfterLeaving: Bool

  public init() {
    if #available(iOS 26.0, *) { continuesAfterLeaving = true } else { continuesAfterLeaving = false }
    // What launch paints first comes from a stored value: no Keychain, no files on the main thread.
    let paired = defaults.bool(forKey: Self.pairedKey)
    var first = paired
      ? ConnectionSnapshot(hasLibrary: true, synology: defaults.bool(forKey: Self.endedKey) ? .accessEnded : .unreachable([]),
                           macNearby: false,
                           catalogue: Self.catalogue(from: defaults.object(forKey: Self.catalogueKey) as? [String: Any]),
                           cacheBytes: 0)
      : .noLibrary
    if paired {
      first.libraryID = Self.storedLibraryID
      first.isSample = defaults.bool(forKey: Self.sampleKey)
      first.libraryName = defaults.string(forKey: Self.nameKey) ?? ""
      first.fileMode = first.catalogue == nil && defaults.bool(forKey: Self.fileModeKey)
    }
    fileModeNow = first.fileMode
    snapshot = first
    opened = Task.detached(priority: .userInitiated) { [weak self] in await self?.openStored() }
    monitor.pathUpdateHandler = { [weak self] path in self?.pathChanged(path) }
    monitor.start(queue: DispatchQueue(label: "mediaviewer.link.path"))
    // Settings' "Faster direct connection on mobile data" reaches the core when it is switched.
    NotificationCenter.default.addObserver(forName: UserDefaults.didChangeNotification, object: nil, queue: nil) { [weak self] _ in
      self?.settingsChanged()
    }
    NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { [weak self] _ in
      guard let self else { return }
      Task { await self.retry() }  // keeps a live link; races only when there is none
    }
  }

  deinit { monitor.cancel() }

  private func openStored() async {
    // A Forget while the stored library opens wins: it is not adopted afterwards.
    let era = writes.era
    let opened: (PairingRecord, LinkHandle)? = await onLinkQueue {
      guard let record = PairingRecord.load(), let device = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }) else { return nil }
      LinkFolders.prepare()
      LinkFolders.keepOnlyThumbs(of: record.libraryID)
      guard let handle = LinkHandle.open(record: record, deviceKey: device, libraryDir: LinkFolders.library,
                                         cacheDir: LinkFolders.thumbs(for: record.libraryID)) else {
        return nil
      }
      return (record, handle)
    }
    if let (record, handle) = opened {
      // Adopted, and its writes allowed, in one step, and only if nothing was forgotten or paired
      // meanwhile.
      let adopted = lock.withLock { () -> Bool in
        guard !forgotten, self.handle == nil, writes.open(for: handle, ifNotClosedSince: era) else { return false }
        self.record = record
        self.handle = handle
        return true
      }
      guard adopted else {
        mv_link_shut_down(handle.raw)  // waits for its own writers; then it is dropped
        if lock.withLock({ forgotten && self.handle == nil }) {
          // Forget ran while this opened, and its removal may have run first: what the open made
          // (the library folder, the caches) goes the same way. No pairing can start meanwhile:
          // pairing waits for this open to finish.
          await onLinkQueue { Self.removeLibraryFiles() }
        }
        return
      }
      // Stamps the id on what launch showed, for an install from before the id was stored.
      if current.libraryID != record.libraryID { publish { _ in } }
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

  /// A write into this phone's folders for `link`'s library (moving in an original, creating a
  /// downloaded clip): run only while that library is still the paired one, and finished before
  /// Forget removes the folders. Keep it short: a create or a move, never a download (the bytes go
  /// into a file already open, which a removal does not bring back). nil when refused.
  func whileLive<T>(_ link: LinkHandle, _ body: () throws -> T) rethrows -> T? {
    // The gate, and inside it the connection's own word on which library is live, as before.
    let ran: T?? = try writes.whileLive(link) { () throws -> T? in
      guard lock.withLock({ handle === link && !forgotten }) else { return nil }
      return try body()
    }
    return ran ?? nil
  }

  /// Bumped on every connect and network change: anything measured on the old route is stale.
  var epoch: Int { lock.withLock { routeEpoch } }

  /// Whether catalogue bytes may move now without the person's say-so.
  private var mayCopyQuietly: Bool { lock.withLock { pathKnown && !pathExpensive } }

  public var current: ConnectionSnapshot { lock.withLock { snapshot } }

  /// For the app, which links MobileLink only: notices the paired library changing.
  public typealias LibraryChange = LibraryWatch

  /// The paired library's id; "" with none. Keys the pictures in memory (`ScopedCache`).
  public var libraryID: String { lock.withLock { record?.libraryID ?? "" } }

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

  /// `from`: the handle the news is about. Checked under the same lock as the change, so news from
  /// a library replaced meanwhile (another pairing, Forget) is dropped, never stamped as the new one.
  private func publish(from link: LinkHandle? = nil, generation: Int? = nil,
                       _ change: (inout ConnectionSnapshot) -> Void) {
    let published: (ConnectionSnapshot, [AsyncStream<ConnectionSnapshot>.Continuation])? = lock.withLock {
      if let link, handle !== link { return nil }
      if let generation, generation != connectGeneration || forgotten { return nil }
      var next = snapshot
      change(&next)
      // Until the stored record is read at launch, the id launch stamped stands.
      next.libraryID = next.hasLibrary ? (record?.libraryID ?? snapshot.libraryID) : ""
      next.isSample = next.hasLibrary && (record.map(\.isSample) ?? snapshot.isSample)
      next.libraryName = next.hasLibrary ? (record?.libraryName ?? snapshot.libraryName) : ""
      next.soundSearch = next.hasLibrary ? soundState : .notNeeded
      next.accessUntil = next.hasLibrary ? record?.accessUntil.map { Date(timeIntervalSince1970: $0) } : nil
      // A catalogue ends file mode, and so does having no library.
      if !next.hasLibrary || next.catalogue != nil { fileModeNow = false }
      next.fileMode = fileModeNow
      snapshot = next
      return (next, Array(continuations.values))
    }
    guard let (value, list) = published else { return }
    defaults.set(value.hasLibrary, forKey: Self.pairedKey)
    defaults.set(value.libraryID, forKey: Self.libraryKey)
    defaults.set(value.isSample, forKey: Self.sampleKey)
    defaults.set(value.hasLibrary && value.synology == .accessEnded, forKey: Self.endedKey)
    defaults.set(value.libraryName, forKey: Self.nameKey)
    defaults.set(value.fileMode, forKey: Self.fileModeKey)
    if let info = value.catalogue {
      defaults.set(["updatedAt": info.updatedAt.timeIntervalSince1970, "itemCount": info.itemCount,
                    "epoch": NSNumber(value: info.epoch)], forKey: Self.catalogueKey)
    } else {
      defaults.removeObject(forKey: Self.catalogueKey)
    }
    list.forEach { $0.yield(value) }
  }

  private static func catalogue(from stored: [String: Any]?) -> CatalogueInfo? {
    guard let stored, let at = stored["updatedAt"] as? TimeInterval, let count = stored["itemCount"] as? Int else { return nil }
    let epoch = (stored["epoch"] as? NSNumber)?.uint64Value ?? 0
    return CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: at), itemCount: count, epoch: epoch)
  }

  // MARK: The network

  /// A direct path after the relay (docs/plans/nas-relay.md §9): on Wi-Fi always, on mobile data
  /// as Settings says. The core enforces it; this only tells it. Never blocks.
  private static func applyUpgrade(_ handle: LinkHandle, mobileData: Bool) {
    mv_link_set_upgrade(handle.raw, mobileData ? 1 : 0, LinkPreferences.directOnMobileDataEnabled() ? 1 : 0)
  }

  private func settingsChanged() {
    lock.withLock {
      if let handle { Self.applyUpgrade(handle, mobileData: pathExpensive) }
    }
  }

  private func pathChanged(_ path: NWPath) {
    let signature = "\(path.status)|" + path.availableInterfaces.map { "\($0.type)-\($0.name)" }.joined(separator: ",")
    let (first, changed) = lock.withLock { () -> (Bool, Bool) in
      let first = !pathKnown
      let changed = !first && signature != pathSignature
      pathKnown = true
      pathExpensive = path.isExpensive
      if let handle { Self.applyUpgrade(handle, mobileData: path.isExpensive) }
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
      return LibraryIdentity(name: linkString(info.library_name), fingerprint: linkString(info.fingerprint),
                             isSample: info.sample != 0,
                             guestFor: info.grant_for_s > 0 ? TimeInterval(info.grant_for_s) : nil)
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

    if parsed.sample != 0 {
      try await runSamplePairing(parsed, continuation: continuation)
      return
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

    // The code was good. A library already on this phone goes now, all of it, as Forget would:
    // its copies stop and its files go before the new one is written (the person agreed to this).
    _ = await awaitLink()  // launch's open first: it may still adopt, or clean up after, the stored library
    if lock.withLock({ self.record != nil || self.handle != nil }) {
      await removeLibrary(keepingDeviceKey: true)
    }

    let record = PairingRecord(libraryID: linkString(parsed.library_id), libraryName: linkString(parsed.library_name),
                               fingerprint: linkString(parsed.fingerprint), publisherKey: linkBytes(parsed.publisher_key).hexString,
                               local: linkString(parsed.local), publicName: linkString(parsed.public_name),
                               identity: linkBytes(parsed.identity).hexString, accessKey: linkBytes(parsed.access_key).hexString,
                               deviceName: deviceName, relay: linkString(parsed.relay))
    let handle: LinkHandle = try await onLinkQueue {
      guard record.save() else { throw PairingFailure.codeNotReadable }
      LinkFolders.prepare()  // again: a Forget removed them, with their backup exclusion
      LinkFolders.keepOnlyThumbs(of: record.libraryID)
      guard let handle = LinkHandle.open(record: record, deviceKey: deviceKey, libraryDir: LinkFolders.library,
                                         cacheDir: LinkFolders.thumbs(for: record.libraryID)) else {
        throw PairingFailure.codeNotReadable
      }
      return handle
    }
    lock.withLock {
      self.record = record
      self.handle = handle
      self.forgotten = false
      fileModeNow = false
      connectGeneration += 1  // the last library's connect, still running, publishes nothing
      writes.open(for: handle)  // with the handle, under the same lock
    }
    work.reopen()
    publish { $0 = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0) }
    continuation.yield(.passed(.synology))

    continuation.yield(.working(.outsideRoute))
    continuation.yield(.outsideRoute(await OutsideRoute.test(publicName: record.publicName)))
    await retry()
  }

  /// The sample library (§11.1): no computer, no service and nothing to redeem. Its folder must
  /// answer before anything on the phone changes; then it is stored and opened as any library is,
  /// and the same sync copies, checks and installs its signed catalogue.
  private func runSamplePairing(_ parsed: mv_pair_info,
                                continuation: AsyncThrowingStream<PairingEvent, Error>.Continuation) async throws {
    continuation.yield(.skipped(.mac))
    continuation.yield(.working(.synology))
    let probeDir = FileManager.default.temporaryDirectory.appendingPathComponent("sample-probe-\(UUID().uuidString)")
    var probe = PairingRecord(libraryID: linkString(parsed.library_id), libraryName: linkString(parsed.library_name),
                              fingerprint: linkString(parsed.fingerprint), publisherKey: linkBytes(parsed.publisher_key).hexString,
                              local: "", publicName: "", identity: "", accessKey: "", deviceName: await deviceName(), sample: true)
    // GUEST: the sample library as a guest, its time counted from now (§8.2's demo; no NAS).
    if parsed.grant_for_s > 0 { probe.accessUntil = Date().timeIntervalSince1970 + Double(parsed.grant_for_s) }
    let (status, report) = await withLinkCancel { cancel in
      await onLinkQueue { () -> (Int32, mv_route_report) in
        defer { try? FileManager.default.removeItem(at: probeDir) }
        guard let handle = LinkHandle.open(record: probe, deviceKey: Data(), libraryDir: probeDir.appendingPathComponent("library"),
                                           cacheDir: probeDir.appendingPathComponent("thumbs")) else {
          return (Int32(MV_LINK_INTERNAL.rawValue), mv_route_report())
        }
        var report = mv_route_report()
        return (mv_link_connect(handle.raw, cancel.raw, &report), report)
      }
    }
    switch status {
    case 0: break
    case 11: throw CancellationError()
    case 12: throw PairingFailure.catalogueRejected  // answered, but not signed by the sample's key
    default: throw PairingFailure.synologyUnreachable(Self.failures(of: report))
    }

    // As with any pairing, the library already here goes first, all of it.
    _ = await awaitLink()  // launch's open first: it may still adopt, or clean up after, the stored library
    if lock.withLock({ self.record != nil || self.handle != nil }) {
      await removeLibrary(keepingDeviceKey: true)
    }
    let record = probe
    let handle: LinkHandle = try await onLinkQueue {
      guard record.save() else { throw PairingFailure.codeNotReadable }
      LinkFolders.prepare()
      LinkFolders.keepOnlyThumbs(of: record.libraryID)
      guard let handle = LinkHandle.open(record: record, deviceKey: Data(), libraryDir: LinkFolders.library,
                                         cacheDir: LinkFolders.thumbs(for: record.libraryID)) else {
        throw PairingFailure.codeNotReadable
      }
      return handle
    }
    lock.withLock {
      self.record = record
      self.handle = handle
      self.forgotten = false
      fileModeNow = false
      connectGeneration += 1
      writes.open(for: handle)  // with the handle, under the same lock
    }
    work.reopen()
    publish { $0 = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0) }
    continuation.yield(.passed(.synology))
    // Always over the internet: there is no route of its own to test.
    continuation.yield(.skipped(.outsideRoute))
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
    // The sample library as a guest, past its end: no request to GitHub (the phone is its NAS).
    let standInEnded = sampleGuestEnded()
    let (fromLink, report, grant) = await onLinkQueue { () -> (Int32, mv_route_report, Double?) in
      var report = mv_route_report()
      if standInEnded { return (Int32(MV_LINK_ACCESS_ENDED.rawValue), report, nil) }
      let status = replacing ? mv_link_reconnect(link.raw, nil, &report) : mv_link_connect(link.raw, nil, &report)
      var expires: Int64 = 0
      var scoped: Int32 = 0
      // -1: the live link closed meanwhile, so nothing is known and the stored end stands.
      let known = status == 0 ? mv_link_grant(link.raw, &expires, &scoped) : -1
      if known < 0 { return (status, report, Double.nan) }
      return (status, report, known == 1 && expires > 0 ? Double(expires) : nil)
    }
    guard lock.withLock({ generation == connectGeneration && !forgotten }) else { return }  // a newer one is running
    var answered = fromLink
    // The sample library has no NAS to end a guest's time: in sample mode only, the phone stands
    // in for it, and the end it recorded at pairing is the NAS's "expired" (owner 2026-11-12:
    // every phone feature demoable in SAMPLE).
    let sampleEnd = lock.withLock { record?.isSample == true ? record?.accessUntil : nil }
    if sampleGuestEnded() { answered = Int32(MV_LINK_ACCESS_ENDED.rawValue) }
    if let sampleEnd, answered == 0 { scheduleSampleEnd(at: sampleEnd) }
    var status = answered
    if status == 0, sampleEnd == nil, grant.map({ !$0.isNaN }) ?? true { noteAccess(until: grant) }
    // §8.2: the NAS's "expired" shows the end and deletes the copy; the phone's own clock past
    // the end the NAS gave only says so, and only when no route answered at all.
    var unreachable = false
    if case .unreachable = Self.state(answered, report) { unreachable = true }
    let end = AccessEnd.decide(serviceSaidEnded: answered == Int32(MV_LINK_ACCESS_ENDED.rawValue),
                               noRouteAnswered: answered != 0 && unreachable,
                               storedEnd: lock.withLock { record?.accessUntil }, now: Date().timeIntervalSince1970,
                               wasEnded: current.synology == .accessEnded)
    if end.showEnded { status = Int32(MV_LINK_ACCESS_ENDED.rawValue) }
    if end.dropLocalCopy { await dropLocalCopy(of: link) }
    let info = await catalogueNow(link: link)
    let cache = Self.cacheBytes(link)
    // Checked again as it publishes: a pairing during the read above makes this news stale.
    publish(from: link, generation: generation) {
      $0 = ConnectionSnapshot(hasLibrary: true, synology: Self.state(status, report), macNearby: false,
                              catalogue: info ?? $0.catalogue, cacheBytes: cache)
    }
    // Not for a library replaced while this connect ran (another pairing, Forget).
    if status == 0, lock.withLock({ generation == connectGeneration && handle === link && !forgotten }) {
      work.run(priority: .utility) { [weak self] in await self?.quietSync(link) }
    }
    // Sound search comes from the project's release, not the NAS: whatever the NAS said.
    work.run(priority: .utility) { [weak self] in await self?.soundSync(link) }
  }

  /// What this library keeps on the phone: the JPEG-512 tiles and sound search's model.
  private static func cacheBytes(_ link: LinkHandle) -> Int64 {
    Int64(mv_thumb_bytes_used(link.raw)) + Int64(mv_sound_bytes(LinkFolders.library.path))
  }

  /// The sample guest's end (code GUEST) has come, on this phone's clock.
  private func sampleGuestEnded() -> Bool {
    lock.withLock {
      AccessEnd.sampleGuestEnded(isSample: record?.isSample == true, accessUntil: record?.accessUntil,
                                 now: Date().timeIntervalSince1970)
    }
  }

  /// In sample-guest mode, a connect at the end itself, so the demo ends while it is watched, as a
  /// NAS's watch would cut the session. One at a time; a Forget or a pairing makes it a no-op.
  private func scheduleSampleEnd(at end: Double) {
    let epoch = lock.withLock { () -> Int? in
      guard sampleEndScheduled != end else { return nil }
      sampleEndScheduled = end
      return libraryEpoch
    }
    guard let epoch else { return }
    Task { [weak self] in
      let wait = max(0, end - Date().timeIntervalSince1970) + 1
      try? await Task.sleep(for: .seconds(wait))
      guard let self, self.lock.withLock({ self.libraryEpoch == epoch && !self.forgotten }) else { return }
      await self.retry()
    }
  }

  /// The service's word on this phone's time limit, kept with the pairing.
  private func noteAccess(until end: Double?) {
    let changed = lock.withLock { () -> Bool in
      guard var r = record, r.accessUntil != end else { return false }
      r.accessUntil = end
      record = r
      _ = r.save()
      return true
    }
    if changed { publish { _ in } }
    lock.withLock { localCopyDropped = false }
  }

  /// A time-limited phone whose access ended keeps nothing of the library but its pairing
  /// (call C6): the catalogue, the tiles, the originals and the clips go, and an extension
  /// fetches them again. Once per ending: a "Try again" that is still refused deletes nothing more.
  private func dropLocalCopy(of expected: LinkHandle) async {
    // Only for the library whose connection was refused, and only while it is still the one here:
    // a Forget or a pairing meanwhile (libraryEpoch) leaves everything to that.
    let started = lock.withLock { () -> (epoch: Int, id: String, cancel: LinkCancel?)? in
      guard !localCopyDropped, !forgotten, let r = record, handle === expected else { return nil }
      localCopyDropped = true
      handle = nil
      return (libraryEpoch, r.libraryID, copyCancel)
    }
    guard let started else { return }
    writes.close()
    started.cancel?.fire()
    lock.withLock { soundCancel }?.fire()  // sound search's download writes into the library too
    mv_link_shut_down(expected.raw)
    await work.stopAndWait()
    let reopened: LinkHandle? = await onLinkQueue {
      // The pairing record is read and written on this queue too, so this is the library still
      // stored, or nothing is touched.
      guard let device = DeviceKey.loadOrMake(new: { mv_device_key_new($0, $1) }),
            AccessEnd.dropIfStill(started.id, stored: { PairingRecord.load()?.libraryID }, remove: { Self.removeLibraryFiles() }),
            let record = PairingRecord.load() else { return nil }
      LinkFolders.prepare()
      LinkFolders.keepOnlyThumbs(of: record.libraryID)
      return LinkHandle.open(record: record, deviceKey: device, libraryDir: LinkFolders.library,
                             cacheDir: LinkFolders.thumbs(for: record.libraryID))
    }
    let adopted = lock.withLock { () -> Bool in
      guard let reopened, !forgotten, handle == nil, libraryEpoch == started.epoch else { return false }
      handle = reopened
      writes.open(for: reopened)
      connectGeneration += 1  // a connect begun on the old handle publishes nothing
      return true
    }
    guard adopted, let reopened else {
      if let reopened { mv_link_shut_down(reopened.raw) }
      return
    }
    work.reopen()
    publish(from: reopened) {
      $0 = ConnectionSnapshot(hasLibrary: true, synology: .accessEnded, macNearby: false, catalogue: nil, cacheBytes: 0)
    }
  }

  /// A newer generation, on Wi-Fi only: the catalogue the person is using stays until it is ready.
  /// In file mode the same check is how the first catalogue arrives (phone-in-the-app.md §4).
  private func quietSync(_ link: LinkHandle) async {
    // The check is a few kilobytes and always allowed (§8.4): on mobile data too, so a library
    // can learn it has a catalogue, or that it was refused. Only the copy waits for Wi-Fi.
    guard current.catalogue != nil || current.fileMode, !sampleGuestEnded() else { return }
    let report = await onLinkQueue { () -> mv_sync_report in
      var report = mv_sync_report()
      _ = mv_sync_check(link.raw, &report)
      return report
    }
    setFileMode(afterCheck: report.outcome, from: link)
    if report.outcome == 12 {  // not signed by the key this phone pinned: say so (§11.3)
      publish(from: link) { $0 = ConnectionSnapshot(hasLibrary: $0.hasLibrary, synology: .publisherChanged, macNearby: false, catalogue: $0.catalogue, cacheBytes: $0.cacheBytes) }
      return
    }
    guard report.outcome == 16, mayCopyQuietly else { return }
    let (done, _) = await copy(link: link, cancel: nil, allowExpensive: false, progress: nil)
    if done == 0, let info = await catalogueNow(link: link) { publishCatalogue(info, from: link) }
  }

  public func forget() async { await removeLibrary(keepingDeviceKey: false) }

  /// Forget's work, also run for the library already paired when another one is paired over it:
  /// nothing of the old library may outlive the new pairing. The device key stays only then.
  private func removeLibrary(keepingDeviceKey: Bool) async {
    // Stop first, wait, then delete: a copy still running (or waiting for Wi-Fi) must not write
    // into a wiped library, and must not keep Forget waiting.
    let (link, cancel, soundCancel) = lock.withLock { () -> (LinkHandle?, LinkCancel?, LinkCancel?) in
      forgotten = true
      let out = (handle, copyCancel, self.soundCancel)
      libraryEpoch += 1
      localCopyDropped = false
      sampleEndScheduled = nil
      fileModeNow = false
      handle = nil
      record = nil
      soundState = .notNeeded
      soundOverMobileData = false
      return out
    }
    // A write already past its check finishes now; every later one is refused.
    writes.close()
    cancel?.fire()
    soundCancel?.fire()
    // Returns once the core's own writers (tiles, the sample's verified copies) are done.
    if let backup = lock.withLock({ beforeRemoval }) { await backup() }
    if let link { mv_link_shut_down(link.raw) }
    await work.stopAndWait()
    await onLinkQueue {
      Self.removeLibraryFiles()
      PairingRecord.clear()
      if !keepingDeviceKey { DeviceKey.clear() }
    }
    publish { $0 = .noLibrary }
  }

  /// Everything a library leaves in this phone's folders. Link queue only; after the handles that
  /// write there are shut down.
  private static func removeLibraryFiles() {
    mv_library_remove(LinkFolders.library.path)
    try? FileManager.default.removeItem(at: LinkFolders.thumbs)
    try? FileManager.default.removeItem(at: LinkFolders.originals)
    try? FileManager.default.removeItem(at: LinkFolders.clips)
    try? FileManager.default.removeItem(at: LinkFolders.files)
  }

  private func catalogueNow(link: LinkHandle) async -> CatalogueInfo? {
    await onLinkQueue {
      var info = mv_catalogue_info()
      guard mv_catalogue_read(link.raw, &info) == 0, info.present != 0 else { return nil }
      return CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: TimeInterval(info.created_unix)),
                           itemCount: Int(info.asset_count), epoch: info.epoch)
    }
  }

  private static func state(_ status: Int32, _ report: mv_route_report) -> SynologyState {
    switch status {
    case 0: return .reachable(address: linkString(report.address), overInternet: report.over_internet != 0,
                              relayed: report.relayed != 0)
    case 7: return .serviceTooOld
    case 5: return .deviceRevoked
    case 20: return .accessEnded
    case 12: return .publisherChanged  // the sample library's list: not signed by the pinned key
    default:
      // A route that answered "not on the list" is the service speaking for the library: this
      // phone was removed, whatever the other routes did.
      if ended(in: report) { return .accessEnded }
      if revoked(in: report) { return .deviceRevoked }
      return .unreachable(failures(of: report))
    }
  }

  private static func ended(in report: mv_route_report) -> Bool {
    withUnsafeBytes(of: report.failures) { raw in
      let list = raw.bindMemory(to: mv_route_failure.self)
      return (0..<Int(report.failure_count)).contains { list[$0].problem == 6 }
    }
  }

  private static func revoked(in report: mv_route_report) -> Bool {
    withUnsafeBytes(of: report.failures) { raw in
      let list = raw.bindMemory(to: mv_route_failure.self)
      return (0..<Int(report.failure_count)).contains { list[$0].problem == 2 }
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
          try await oneCopy.run { try await self.runCopy(overMobileData: overMobileData, continuation: continuation) }
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
    // An ended access copies nothing: a NAS refuses it, and the sample guest's stand-in does too.
    // The connect that says so on the card runs apart from this copy, never awaited here: it
    // stops and waits for tracked work, this copy among it.
    if sampleGuestEnded() {
      Task { [weak self] in await self?.retry() }
      throw CopyFailure.accessEnded
    }
    let status = await onLinkQueue { () -> Int32 in
      var report = mv_route_report()
      return mv_link_connect(link.raw, nil, &report)  // keeps a live link
    }
    if status == Int32(MV_LINK_ACCESS_ENDED.rawValue) {
      Task { [weak self] in await self?.retry() }
      throw CopyFailure.accessEnded
    }
    guard status == 0 else { throw CopyFailure.synologyUnreachable }
    let check = await onLinkQueue { () -> mv_sync_report in
      var report = mv_sync_report()
      _ = mv_sync_check(link.raw, &report)
      return report
    }
    switch check.outcome {
    case 15, 14:
      if let info = await catalogueNow(link: link) { publishCatalogue(info, from: link) }
      return
    case FileModeRule.noCatalogue:
      // The folders are there and no catalogue is: the library opens in file mode.
      setFileMode(afterCheck: check.outcome, from: link)
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
    var mine: LinkCancel?
    defer {
      finishContinued(success: succeeded)
      // Only this copy's own cancel: a later copy's is never dropped.
      lock.withLock { if copyCancel != nil && copyCancel === mine { copyCancel = nil } }
    }
    var started = false
    while true {
      // Size first; nothing moves over mobile data unless the person asked (§8.4).
      try await CopyGate.wait(
        mayMove: { [weak self] in overMobileData || (self?.mayCopyQuietly ?? false) },
        stopped: { [weak self] in
          guard let self else { return true }
          return self.lock.withLock { self.forgotten } || self.continued.isExpired
        },
        waiting: { continuation.yield(.waitingForWiFi(totalBytes: Int64(check.total_bytes))) })
      let cancel = LinkCancel()
      lock.withLock { copyCancel = cancel }
      mine = cancel
      // An expiry between the gate and here fired the previous cancel, or none: never lose it.
      if continued.isExpired { throw CancellationError() }
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
        if let info = await catalogueNow(link: link) { publishCatalogue(info, from: link) }
        return
      case 19: continue  // left Wi-Fi: wait for it, then pick up where it stopped
      case FileModeRule.noCatalogue:  // gone from the share since the check
        succeeded = true
        setFileMode(afterCheck: status, from: link)
        return
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

  /// File mode from what a catalogue check said, for `link`'s library only.
  private func setFileMode(afterCheck outcome: Int32, from link: LinkHandle) {
    let changed = lock.withLock { () -> Bool in
      guard handle === link, !forgotten else { return false }
      let next = FileModeRule.fileMode(afterCheck: outcome, hasCatalogue: snapshot.catalogue != nil, wasFileMode: fileModeNow)
      guard next != fileModeNow else { return false }
      fileModeNow = next
      return true
    }
    if changed { publish(from: link) { _ in } }
  }

  /// Work on a library that has since been replaced (another pairing, or Forget) says nothing.
  private func publishCatalogue(_ info: CatalogueInfo, from link: LinkHandle) {
    publish(from: link) { $0 = ConnectionSnapshot(hasLibrary: $0.hasLibrary, synology: $0.synology, macNearby: false, catalogue: info, cacheBytes: $0.cacheBytes) }
    // A first catalogue, or one whose clips were heard by another model: sound search follows it.
    work.run(priority: .utility) { [weak self] in await self?.soundSync(link) }
  }

  // MARK: Sound search (§9)

  /// Where sound search's model comes from. A Debug build may be pointed at a test folder
  /// (`MV_SOUND_BASE`); a release reads only the core's.
  static var soundBase: String {
    #if DEBUG
    if let override = ProcessInfo.processInfo.environment["MV_SOUND_BASE"], !override.isEmpty {
      return override.hasSuffix("/") ? override : override + "/"
    }
    #endif
    return String(cString: mv_sound_base())
  }

  public func downloadSoundSearch(overMobileData: Bool) async {
    lock.withLock {
      if overMobileData { soundOverMobileData = true }
      // The person asked: a damaged download or a missing file is tried again.
      if case .unavailable = soundState { soundState = .notNeeded }
    }
    guard let link = await awaitLink() else { return }
    work.run(priority: .userInitiated) { [weak self] in await self?.soundSync(link) }
  }

  private func setSound(_ state: SoundSearchState, from link: LinkHandle, cacheBytes: Int64? = nil) {
    let changed = lock.withLock { () -> Bool in
      guard handle === link, !forgotten else { return false }
      soundState = state
      return true
    }
    guard changed else { return }
    publish(from: link) { snapshot in
      if let cacheBytes {
        snapshot = ConnectionSnapshot(hasLibrary: snapshot.hasLibrary, synology: snapshot.synology, macNearby: snapshot.macNearby,
                                      catalogue: snapshot.catalogue, cacheBytes: cacheBytes)
      }
    }
  }

  /// Whether this library's clips want the phone's sound model, and the download if so: on Wi-Fi,
  /// or on mobile data once the person allowed it. One at a time; a later call while one runs
  /// finds it running and leaves it.
  private func soundSync(_ link: LinkHandle) async {
    let start = lock.withLock { () -> Bool in
      guard !forgotten, handle === link else { return false }
      if soundRunning {
        soundAgain = true
        return false
      }
      // Gone or damaged: not again on every connection (152 MB each time) until the next launch or
      // the person's Try again.
      if case .unavailable(let problem) = soundState, problem == .gone || problem == .rejected { return false }
      soundRunning = true
      return true
    }
    guard start else { return }
    defer {
      let again = lock.withLock { () -> Bool in
        soundRunning = false
        defer { soundAgain = false }
        return soundAgain && !forgotten && handle === link
      }
      if again { work.run(priority: .utility) { [weak self] in await self?.soundSync(link) } }
    }
    let library = LinkFolders.library.path
    let wanted = await onLinkQueue { () -> Bool in
      var info = mv_catalogue_info()
      guard mv_catalogue_read(link.raw, &info) == 0, info.present != 0 else { return false }
      return mv_sound_wanted(linkString(info.dir)) != 0
    }
    guard wanted else { return setSound(.notNeeded, from: link) }
    if mv_sound_ready(library) != 0 { return setSound(.ready, from: link) }
    let total = Int64(mv_sound_total_bytes())
    let have = min(Int64(mv_sound_bytes(library)), total)
    let overMobileData = lock.withLock { soundOverMobileData }
    guard overMobileData || mayCopyQuietly else { return setSound(.waitingForWiFi(totalBytes: total - have), from: link) }

    let cancel = LinkCancel()
    lock.withLock { soundCancel = cancel }
    defer { lock.withLock { if soundCancel === cancel { soundCancel = nil } } }
    if lock.withLock({ forgotten }) { return }
    setSound(.downloading(doneBytes: have, totalBytes: total), from: link)
    // A percent at a time: each publish is a snapshot for every screen and a defaults write.
    let shown = SoundPercent()
    let box = CopyCallbacks(progress: { [weak self] done, all in
      guard shown.changed(done: done, total: all) else { return }
      self?.setSound(.downloading(doneBytes: Int64(done), totalBytes: Int64(all)), from: link)
    }) { [weak self] in
      guard let self else { return false }
      return self.lock.withLock { !self.forgotten && self.pathKnown && (overMobileData || !self.pathExpensive) }
    }
    let base = Self.soundBase
    let status: Int32 = await withTaskCancellationHandler {
      await withCheckedContinuation { continuation in
        soundQueue.async {
          let context = Unmanaged.passRetained(box)
          let status = base.withCString { base in
            mv_sound_fetch(library, base, SampleFetch.callback, SampleFetch.context, cancel.raw, { ctx in
              guard let ctx else { return 0 }
              return Unmanaged<CopyCallbacks>.fromOpaque(ctx).takeUnretainedValue().allowed() ? 1 : 0
            }, { ctx, done, total in
              guard let ctx else { return }
              Unmanaged<CopyCallbacks>.fromOpaque(ctx).takeUnretainedValue().report(done, total)
            }, context.toOpaque())
          }
          context.release()
          continuation.resume(returning: status)
        }
      }
    } onCancel: { cancel.fire() }
    switch status {
    case 0:
      lock.withLock { soundOverMobileData = false }
      setSound(.ready, from: link, cacheBytes: Self.cacheBytes(link))
    case 19:  // left Wi-Fi: the next connection on Wi-Fi picks up where it stopped
      setSound(.waitingForWiFi(totalBytes: total - min(Int64(mv_sound_bytes(library)), total)), from: link,
               cacheBytes: Self.cacheBytes(link))
    case 11:  // Forget (which says nothing), or the work stopping: picked up on the next connection
      setSound(.waitingForWiFi(totalBytes: total - min(Int64(mv_sound_bytes(library)), total)), from: link,
               cacheBytes: Self.cacheBytes(link))
    case 9: setSound(.unavailable(.gone), from: link, cacheBytes: Self.cacheBytes(link))
    case 12: setSound(.unavailable(.rejected), from: link, cacheBytes: Self.cacheBytes(link))
    case 13: setSound(.unavailable(.notEnoughSpace), from: link, cacheBytes: Self.cacheBytes(link))
    default: setSound(.unavailable(.offline), from: link, cacheBytes: Self.cacheBytes(link))
    }
  }

  // MARK: Continued processing (iOS 26)

  private func submitContinuedTask() {
    guard #available(iOS 26.0, *) else { return }
    let request = BGContinuedProcessingTaskRequest(identifier: copyTaskIdentifier, title: "Copying your library",
                                                   subtitle: "MediaViewer is bringing your catalogue to this phone.")
    request.strategy = .fail
    // Refused (the Simulator, or one already running): the copy still runs while the app is open.
    continued.submit { try BGTaskScheduler.shared.submit(request) }
  }

  @available(iOS 26.0, *)
  fileprivate func adopt(_ task: BGContinuedProcessingTask) {
    task.expirationHandler = { [weak self] in
      guard let self else { return }
      // The copy ends (a transfer by its cancel, the Wi-Fi wait by the expired flag), and its defer
      // completes the task: Apple's rule, completed even when expired.
      self.continued.expire()
      self.lock.withLock { self.copyCancel }?.fire()
    }
    continued.adopt(SystemContinuedTask(task))
  }

  private func reportContinued(done: UInt64, total: UInt64) {
    continued.progress(done: Int64(done), total: Int64(total))
  }

  private func finishContinued(success: Bool) {
    continued.finished(success: success)
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

@available(iOS 26.0, *)
private final class SystemContinuedTask: ContinuedTaskHandle {
  private let task: BGContinuedProcessingTask
  init(_ task: BGContinuedProcessingTask) { self.task = task }
  func complete(success: Bool) { task.setTaskCompleted(success: success) }
  func progress(done: Int64, total: Int64) {
    task.progress.totalUnitCount = total
    task.progress.completedUnitCount = done
  }
}

private final class OnceFlag: @unchecked Sendable {
  private let lock = NSLock()
  private var taken = false
  func take() -> Bool { lock.withLock { defer { taken = true }; return !taken } }
}


/// Sound search's download blocks a thread for minutes: never the link's own queue.
private let soundQueue = DispatchQueue(label: "mediaviewer.link.sound", qos: .utility)

/// The download's progress, one whole percent at a time. Called on the download's thread only.
private final class SoundPercent: @unchecked Sendable {
  private var last = -1
  func changed(done: UInt64, total: UInt64) -> Bool {
    let percent = total == 0 ? 100 : Int(done * 100 / total)
    defer { last = percent }
    return percent != last
  }
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
