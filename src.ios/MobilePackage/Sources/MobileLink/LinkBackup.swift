// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup, release 1.1 (docs/plans/mobile.md §10b): Photos, newest first, to the backup
// folder on the NAS while the app is open, through src/mobile/backup.* (mv_backup_* in
// mv_mobile_link.h). One original at a time is exported from PhotoKit to a scratch file, downloaded
// from iCloud first if need be, sent and removed, so the phone never holds more than the largest
// file. With the sample library the "NAS" is "Sample NAS on this iPhone", a folder in the app's own
// storage: nothing is sent anywhere, and Forget removes it.
import AVFoundation
import CMobileLink
import Foundation
import ImageIO
import MobileKit
import Network
import Photos
import UIKit

public final class LinkBackup: BackupService, @unchecked Sendable {
  private let connection: LinkConnection
  private let lock = NSLock()
  private var snapshot: BackupSnapshot
  private var listeners: [UUID: AsyncStream<BackupSnapshot>.Continuation] = [:]
  private var handle: OpaquePointer?          // mv_backup, for `handleLibrary`
  private var handleLink: LinkHandle?         // kept alive while `handle` is open
  private var handleLibrary = ""
  private var running = false
  private var cancel: LinkCancel?
  private var localIDs: [String: String] = [:]  // ledger key -> Photos local identifier, from the last listing
  private var onWiFi = false
  private var pathKnown = false
  private let monitor = NWPathMonitor()
  // Concurrent: a run blocks one thread for as long as it lasts, while the screen still asks for folders.
  private let queue = DispatchQueue(label: "mediaviewer.backup", qos: .utility, attributes: .concurrent)
  private let defaults = UserDefaults.standard

  private enum Key {
    static let folderID = "backup.folderID", folderLabel = "backup.folderLabel", layout = "backup.layout"
    static let automatic = "backup.automatic", allowsSleep = "backup.allowsSleep", library = "backup.library"
  }

  /// Application Support, excluded from iCloud backup (§8.3): the ledger and the sample "NAS".
  static let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    .appendingPathComponent("backup", isDirectory: true)
  static var ledger: URL { root.appendingPathComponent("ledger.db") }
  static var sampleNAS: URL { root.appendingPathComponent("sample-nas", isDirectory: true) }
  static let scratch = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
    .appendingPathComponent("backup-scratch", isDirectory: true)

  public init(connection: LinkConnection) {
    self.connection = connection
    var s = BackupSnapshot()
    s.layout = BackupLayout(rawValue: defaults.integer(forKey: Key.layout)) ?? .yearMonth
    s.automatic = defaults.object(forKey: Key.automatic) as? Bool ?? true
    s.allowsSleep = defaults.bool(forKey: Key.allowsSleep)
    // A folder belongs to the library it was chosen for.
    if defaults.string(forKey: Key.library) == LinkConnection.storedLibraryID,
       let id = defaults.string(forKey: Key.folderID), let label = defaults.string(forKey: Key.folderLabel) {
      s.folder = BackupFolder(id: id, label: label, writable: true)
    }
    s.isSample = connection.current.isSample
    snapshot = s
    connection.setBeforeRemoval { [weak self] in await self?.removeForForget() }
    monitor.pathUpdateHandler = { [weak self] path in self?.pathChanged(path) }
    monitor.start(queue: DispatchQueue(label: "mediaviewer.backup.path"))
    let center = NotificationCenter.default
    center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: nil) { [weak self] _ in
      self?.startIfAutomatic()
    }
    // Leaving the app stops the copy; the service keeps the unfinished file to go on from (§10b).
    center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { [weak self] _ in
      self?.stop()
    }
    Task.detached { [weak self] in
      guard let self else { return }
      for await snap in connection.updates() {
        self.change { $0.isSample = snap.isSample }
        if !snap.hasLibrary { self.stop() }
      }
    }
  }

  deinit { monitor.cancel() }

  // MARK: BackupService

  public var current: BackupSnapshot { lock.withLock { snapshot } }

  public func updates() -> AsyncStream<BackupSnapshot> {
    AsyncStream { continuation in
      let id = UUID()
      lock.withLock { listeners[id] = continuation }
      continuation.yield(current)
      continuation.onTermination = { [weak self] _ in
        guard let self else { return }
        self.lock.withLock { self.listeners[id] = nil }
      }
    }
  }

  public func folders() async throws -> [BackupFolder] {
    guard let b = await openHandle() else { throw BackupState.unreachable }
    return try await onQueue {
      final class Box { var list: [BackupFolder] = [] }
      let box = Box()
      let status = mv_backup_folders(b, { ctx, id, label, writable in
        guard let ctx, let id, let label else { return }
        Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue().list.append(
          BackupFolder(id: String(cString: id), label: String(cString: label), writable: writable != 0))
      }, Unmanaged.passUnretained(box).toOpaque())
      switch status {
      case Int32(MV_LINK_OK.rawValue): return box.list
      case Int32(MV_LINK_TOO_OLD.rawValue): throw BackupState.serviceTooOld
      default: throw BackupState.unreachable
      }
    }
  }

  public func requestPhotosAccess() async -> Bool {
    let status = await PHPhotoLibrary.requestAuthorization(for: .readWrite)
    change { $0.limitedAccess = status == .limited }
    return status == .authorized || status == .limited
  }

  public func choose(folder: BackupFolder, layout: BackupLayout) async {
    stop()
    defaults.set(folder.id, forKey: Key.folderID)
    defaults.set(folder.label, forKey: Key.folderLabel)
    defaults.set(layout.rawValue, forKey: Key.layout)
    defaults.set(connection.libraryID, forKey: Key.library)
    change {
      $0.folder = folder
      $0.layout = layout
      $0.state = .off
    }
    start(overMobileData: false)
  }

  public func setAutomatic(_ on: Bool) {
    defaults.set(on, forKey: Key.automatic)
    change { $0.automatic = on }
    if on { startIfAutomatic() }
  }

  public func setAllowsSleep(_ on: Bool) {
    defaults.set(on, forKey: Key.allowsSleep)
    change { $0.allowsSleep = on }
    updateIdleTimer()
  }

  public func start(overMobileData: Bool) {
    let go: Bool = lock.withLock {
      guard !running, snapshot.folder != nil else { return false }
      running = true
      return true
    }
    guard go else { return }
    Task.detached(priority: .utility) { [weak self] in
      await self?.run(overMobileData: overMobileData)
      self?.lock.withLock { self?.running = false }
      self?.updateIdleTimer()
    }
  }

  public func stop() {
    lock.withLock { cancel }?.fire()
  }

  public func notBackedUp() async -> [String] {
    guard let b = lock.withLock({ handle }) else { return [] }
    let map = lock.withLock { localIDs }
    return await onQueue {
      final class Box { var keys: [String] = [] }
      let box = Box()
      _ = mv_backup_remaining(b, { ctx, key in
        guard let ctx, let key else { return }
        Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue().keys.append(String(cString: key))
      }, Unmanaged.passUnretained(box).toOpaque())
      return box.keys.compactMap { map[$0] }
    }
  }

  public func thumbnail(ofPhoto id: String, side: CGFloat) async -> UIImage? {
    guard let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else { return nil }
    let options = PHImageRequestOptions()
    options.deliveryMode = .opportunistic
    options.isNetworkAccessAllowed = false
    options.resizeMode = .fast
    let scale = await MainActor.run { UIScreen.main.scale }
    return await withCheckedContinuation { c in
      var resumed = false
      PHImageManager.default().requestImage(for: asset, targetSize: CGSize(width: side * scale, height: side * scale),
                                            contentMode: .aspectFill, options: options) { image, info in
        let degraded = (info?[PHImageResultIsDegradedKey] as? Bool) ?? false
        guard !degraded, !resumed else { return }
        resumed = true
        c.resume(returning: image)
      }
    }
  }

  // MARK: The run

  private func startIfAutomatic() {
    let (automatic, folder) = lock.withLock { (snapshot.automatic, snapshot.folder) }
    guard automatic, folder != nil else { return }
    start(overMobileData: false)
  }

  private func run(overMobileData: Bool) async {
    guard let folder = current.folder else { return }
    let sample = connection.current.isSample
    // Never over mobile data unless asked (§10b). The sample's "NAS" is on this phone: no network at all.
    if !sample && !overMobileData && !lock.withLock({ onWiFi }) {
      change { $0.state = .pausedOffWiFi }
      return
    }
    let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
    var allowed = status == .authorized || status == .limited
    if status == .notDetermined { allowed = await requestPhotosAccess() }
    guard allowed else {
      change { $0.state = .noPhotosAccess }
      return
    }
    change {
      $0.limitedAccess = PHPhotoLibrary.authorizationStatus(for: .readWrite) == .limited
      $0.state = .checking
      $0.failures = []
    }
    guard let b = await openHandle() else {
      change { $0.state = .unreachable }
      return
    }
    let cancel = LinkCancel()
    lock.withLock { self.cancel = cancel }
    updateIdleTimer()
    let layout = current.layout
    let final: Int32 = await onQueue { [self] in
      let total = listPhotos(into: b)
      change { $0.total = total }
      let ctx = Unmanaged.passUnretained(self).toOpaque()
      var state: Int32 = 8  // unreachable, unless the run says otherwise
      let status = folder.id.withCString { id in
        mv_backup_run(b, id, Int32(layout.rawValue), cancel.raw, { ctx, localID, index, path, cancel, ymd in
          guard let ctx, let localID, let path else { return Int32(MV_LINK_INVALID.rawValue) }
          let me = Unmanaged<LinkBackup>.fromOpaque(ctx).takeUnretainedValue()
          return me.export(localID: String(cString: localID), index: Int(index), to: String(cString: path), cancel: cancel, ymd: ymd)
        }, { ctx, status in
          guard let ctx, let status else { return }
          Unmanaged<LinkBackup>.fromOpaque(ctx).takeUnretainedValue().report(status.pointee)
        }, ctx, &state)
      }
      return status == Int32(MV_LINK_OK.rawValue) ? state : 8
    }
    lock.withLock { self.cancel = nil }
    var failures: [BackupFailureRow] = []
    _ = await onQueue {
      final class Box { var rows: [BackupFailureRow] = [] }
      let box = Box()
      mv_backup_failures(b, { ctx, key, name, why in
        guard let ctx, let key, let name, let why else { return }
        let reason: String
        switch String(cString: why) {
        case "gone": reason = "No longer in Photos"
        case "export": reason = "Couldn't be read from Photos"
        case "hash": reason = "Arrived damaged twice"
        default: reason = "Couldn't be copied"
        }
        Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue().rows.append(
          BackupFailureRow(id: String(cString: key) + "/" + String(cString: name), name: String(cString: name), reason: reason))
      }, Unmanaged.passUnretained(box).toOpaque())
      failures = box.rows
      return 0
    }
    let space = await onQueue { () -> BackupSpace? in
      var s = mv_backup_space()
      guard folder.id.withCString({ mv_backup_space_of(b, $0, &s) }) == Int32(MV_LINK_OK.rawValue) else { return nil }
      return BackupSpace(limitBytes: Int64(s.limit), usedBytes: Int64(s.used), roomBytes: Int64(s.room))
    }
    change {
      $0.state = Self.state(final)
      $0.currentName = nil
      $0.failures = failures
      if let space { $0.space = space }
    }
  }

  /// The person's Photos, newest first, as the core's items. Returns how many.
  private func listPhotos(into b: OpaquePointer) -> Int {
    mv_backup_clear_items(b)
    let options = PHFetchOptions()
    options.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
    options.includeAssetSourceTypes = [.typeUserLibrary]
    let assets = PHAsset.fetchAssets(with: options)
    var ids: [String] = []
    ids.reserveCapacity(assets.count)
    assets.enumerateObjects { asset, _, _ in ids.append(asset.localIdentifier) }
    // §10b: PHCloudIdentifier where iCloud Photos gives one, since a local id is per device.
    var cloud: [String: String] = [:]
    for (local, result) in PHPhotoLibrary.shared().cloudIdentifierMappings(forLocalIdentifiers: ids) {
      if case .success(let id) = result { cloud[local] = id.stringValue }
    }
    var keys: [String: String] = [:]
    var count = 0
    let calendar = Calendar.current
    assets.enumerateObjects { asset, _, _ in
      let resources = Self.originals(of: asset)
      guard !resources.isEmpty else { return }
      let key = cloud[asset.localIdentifier] ?? "local:" + asset.localIdentifier
      let date = calendar.dateComponents([.year, .month, .day], from: asset.creationDate ?? Date())
      let json = "[" + resources.map { "{\"kind\":\"\($0.kind)\",\"name\":\(Self.jsonString($0.name))}" }.joined(separator: ",") + "]"
      let added = key.withCString { k in
        asset.localIdentifier.withCString { l in
          json.withCString { j in mv_backup_add_item(b, k, l, Int32(date.year ?? 1970), Int32(date.month ?? 1), Int32(date.day ?? 1), j) }
        }
      }
      if added == Int32(MV_LINK_OK.rawValue) {
        keys[key] = asset.localIdentifier
        count += 1
      }
    }
    lock.withLock { localIDs = keys }
    return count
  }

  /// The originals as shot (§10b): the photo or video, a Live Photo's video, a RAW. Not Photos' edits.
  private static func originals(of asset: PHAsset) -> [(kind: String, name: String, resource: PHAssetResource)] {
    var out: [(String, String, PHAssetResource)] = []
    var used = Set<String>()
    for r in PHAssetResource.assetResources(for: asset) {
      let kind: String
      switch r.type {
      case .photo: kind = "photo"
      case .video: kind = "video"
      case .pairedVideo: kind = "live_video"
      case .alternatePhoto: kind = "raw"
      default: continue
      }
      // A name the service would refuse (a path, a hidden name) is made plain.
      var name = r.originalFilename.replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "\\", with: "_")
      while name.hasPrefix(".") { name.removeFirst() }
      if name.isEmpty { name = "\(kind).bin" }
      if name.utf8.count > 200 { name = String(name.suffix(120)) }
      guard used.insert(kind + "/" + name).inserted else { continue }
      out.append((kind, name, r))
    }
    return out
  }

  private static func jsonString(_ s: String) -> String {
    let data = (try? JSONSerialization.data(withJSONObject: [s])) ?? Data("[\"\"]".utf8)
    let text = String(decoding: data, as: UTF8.self)
    return String(text.dropFirst().dropLast())
  }

  /// The core's run thread: one original to `path`, from iCloud if need be, then its capture date.
  private func export(localID: String, index: Int, to path: String, cancel: OpaquePointer?, ymd: UnsafeMutablePointer<Int32>?) -> Int32 {
    guard let asset = PHAsset.fetchAssets(withLocalIdentifiers: [localID], options: nil).firstObject else {
      return Int32(MV_LINK_NOT_FOUND.rawValue)
    }
    let resources = Self.originals(of: asset)
    guard index < resources.count else { return Int32(MV_LINK_NOT_FOUND.rawValue) }
    let url = URL(fileURLWithPath: path)
    try? FileManager.default.removeItem(at: url)
    guard FileManager.default.createFile(atPath: path, contents: nil), let out = try? FileHandle(forWritingTo: url) else {
      return Int32(MV_LINK_INTERNAL.rawValue)
    }
    let options = PHAssetResourceRequestOptions()
    options.isNetworkAccessAllowed = true  // iCloud-only originals: downloaded one at a time, then dropped
    let done = DispatchSemaphore(value: 0)
    var failed: Error?
    var stopped = false
    let manager = PHAssetResourceManager.default()
    let request = manager.requestData(for: resources[index].resource, options: options, dataReceivedHandler: { data in
      if stopped { return }
      if mv_backup_cancelled(cancel) != 0 { stopped = true; return }
      do { try out.write(contentsOf: data) } catch { failed = error; stopped = true }
    }, completionHandler: { error in
      if let error { failed = error }
      done.signal()
    })
    while done.wait(timeout: .now() + .milliseconds(200)) == .timedOut {
      if mv_backup_cancelled(cancel) != 0 || stopped {
        manager.cancelDataRequest(request)
        _ = done.wait(timeout: .now() + .seconds(5))
        break
      }
    }
    try? out.close()
    if mv_backup_cancelled(cancel) != 0 { return Int32(MV_LINK_CANCELLED.rawValue) }
    if failed != nil || stopped { return Int32(MV_LINK_INTERNAL.rawValue) }
    if let ymd, let date = Self.captureDate(of: url, kind: resources[index].kind) {
      ymd[0] = Int32(date.0); ymd[1] = Int32(date.1); ymd[2] = Int32(date.2)
    }
    return Int32(MV_LINK_OK.rawValue)
  }

  /// §10b: the date where it was taken, which does not change with the phone's time zone. A photo's
  /// EXIF DateTimeOriginal is that local time already; a video's QuickTime creation date carries its offset.
  private static func captureDate(of url: URL, kind: String) -> (Int, Int, Int)? {
    func parse(_ text: String) -> (Int, Int, Int)? {
      let digits = text.prefix(10).split(whereSeparator: { $0 == ":" || $0 == "-" }).compactMap { Int($0) }
      guard digits.count == 3, (1900...9999).contains(digits[0]), (1...12).contains(digits[1]), (1...31).contains(digits[2]) else { return nil }
      return (digits[0], digits[1], digits[2])
    }
    if kind == "photo" || kind == "raw" {
      guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
            let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
            let exif = props[kCGImagePropertyExifDictionary] as? [CFString: Any],
            let original = exif[kCGImagePropertyExifDateTimeOriginal] as? String else { return nil }
      return parse(original)
    }
    let asset = AVURLAsset(url: url)
    let wait = DispatchSemaphore(value: 0)
    var found: (Int, Int, Int)?
    Task.detached {
      if let items = try? await asset.load(.metadata) {
        for item in items where item.identifier == .quickTimeMetadataCreationDate {
          if let text = try? await item.load(.stringValue) { found = parse(text) }
        }
      }
      wait.signal()
    }
    _ = wait.wait(timeout: .now() + .seconds(5))
    return found
  }

  private func report(_ s: mv_backup_status) {
    let name = withUnsafeBytes(of: s.current) { raw -> String in
      guard let base = raw.baseAddress else { return "" }
      return String(cString: base.assumingMemoryBound(to: CChar.self))
    }
    change {
      $0.state = Self.state(s.state)
      $0.total = Int(s.total)
      $0.backedUp = Int(s.backed_up)
      $0.currentFraction = s.current_fraction
      $0.currentName = name.isEmpty ? nil : name
    }
  }

  private static func state(_ raw: Int32) -> BackupState {
    switch raw {
    case 1: .checking
    case 2: .running
    case 3: .done
    case 4: .pausedOffWiFi
    case 5: .folderNotAllowed
    case 6: .limitReached
    case 7: .nasFull
    case 8: .unreachable
    case 9: .serviceTooOld
    case 10: .folderGone
    case 11: .noPhotosAccess
    case 12: .done
    default: .off
    }
  }

  // MARK: Plumbing

  /// The core's backup handle for the paired library, opened once per library.
  private func openHandle() async -> OpaquePointer? {
    guard let link = await connection.awaitLink() else { return nil }
    let library = connection.libraryID
    if let open = lock.withLock({ handleLibrary == library && handleLink === link ? handle : nil }) { return open }
    let sample = connection.current.isSample
    return await onQueue { [self] in
      for url in [Self.root, Self.scratch] {
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true,
                                                 attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
        var u = url
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? u.setResourceValues(values)
      }
      let opened: OpaquePointer? = Self.ledger.path.withCString { ledger in
        Self.scratch.path.withCString { scratch in
          library.withCString { id in
            Self.sampleNAS.path.withCString { nas in
              var c = mv_backup_config(ledger_path: ledger, scratch_dir: scratch, library_id: id, sample_folder: sample ? nas : nil)
              return mv_backup_open(link.raw, &c)
            }
          }
        }
      }
      let old: OpaquePointer? = lock.withLock {
        let previous = handle
        handle = opened
        handleLink = link
        handleLibrary = library
        return previous
      }
      if let old { mv_backup_close(old) }
      return opened
    }
  }

  /// Forget (and a new pairing over this library): stop, wait, remove the ledger and the sample folder.
  private func removeForForget() async {
    stop()
    while lock.withLock({ running }) { try? await Task.sleep(for: .milliseconds(50)) }
    let old: OpaquePointer? = lock.withLock {
      let h = handle
      handle = nil
      handleLink = nil
      handleLibrary = ""
      localIDs = [:]
      return h
    }
    await onQueue {
      if let old { mv_backup_close(old) }
      Self.sampleNAS.path.withCString { nas in Self.ledger.path.withCString { mv_backup_remove_local(nas, $0) } }
      try? FileManager.default.removeItem(at: Self.scratch)
    }
    for key in [Key.folderID, Key.folderLabel, Key.library] { defaults.removeObject(forKey: key) }
    change {
      $0 = BackupSnapshot(layout: $0.layout, automatic: $0.automatic, allowsSleep: $0.allowsSleep)
    }
  }

  private func pathChanged(_ path: NWPath) {
    let wifi = path.status == .satisfied && !path.isExpensive && !path.isConstrained
    let was = lock.withLock { () -> Bool in
      let was = onWiFi
      onWiFi = wifi
      pathKnown = true
      return was
    }
    if wifi && !was {
      if current.state == .pausedOffWiFi { change { $0.state = .off } }
      startIfAutomatic()
    } else if !wifi && was && !connection.current.isSample {
      stop()  // never over mobile data unless asked; it goes on from here on Wi-Fi
      change { if $0.state == .running || $0.state == .checking { $0.state = .pausedOffWiFi } }
    }
  }

  private func updateIdleTimer() {
    let awake = lock.withLock { running && !snapshot.allowsSleep }
    Task { @MainActor in UIApplication.shared.isIdleTimerDisabled = awake }
  }

  private func change(_ body: (inout BackupSnapshot) -> Void) {
    let (value, list) = lock.withLock { () -> (BackupSnapshot, [AsyncStream<BackupSnapshot>.Continuation]) in
      body(&snapshot)
      return (snapshot, Array(listeners.values))
    }
    list.forEach { $0.yield(value) }
  }

  private func onQueue<T>(_ body: @escaping @Sendable () -> T) async -> T {
    await withCheckedContinuation { c in queue.async { c.resume(returning: body()) } }
  }

  private func onQueue<T>(_ body: @escaping @Sendable () throws -> T) async throws -> T {
    try await withCheckedThrowingContinuation { c in
      queue.async {
        do { c.resume(returning: try body()) } catch { c.resume(throwing: error) }
      }
    }
  }
}
