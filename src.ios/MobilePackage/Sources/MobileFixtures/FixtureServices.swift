// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins behind MobileKit's protocols. One FixtureWorld holds what each state asks for.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import AVFoundation
import MobileKit
import UIKit

final class FixtureWorld: @unchecked Sendable {
  enum CopyMode { case running, waitingForWiFi, fails, resuming, noSpace }
  enum ImageMode { case fast, slowOriginal, slowUnknownSize, slowNetwork, fails(MediaFailure) }
  enum SearchMode { case normal, neverAnswers, modelLoading }
  enum ClipMode { case stream, tooBig, notPlayable, fails(MediaFailure), stopsPlaying, stalls }

  var snapshot: ConnectionSnapshot
  var copyMode = CopyMode.running
  var imageMode = ImageMode.fast
  var searchMode = SearchMode.normal
  var capability = SearchCapability.ready
  var copyContinuesAfterLeaving = true
  /// Set when the app is left, so a stopped copy says so on return.
  var copyWasLeft = false
  var copyIsAway = false
  var modelReady = false
  var clipMode = ClipMode.stream
  var hasPeople = true
  var libraryEmpty = false
  var libraryFails = false
  /// How many reads of the people or a person fail before one works.
  var peopleFailures = 0
  var identifyFailures = 0
  var personFailures = 0
  var foldersFail = false
  /// Save to Photos is refused, as when the person said no to adding to Photos.
  var photosRefused = false
  var searchFails = false
  /// Stand-in backup: nil runs nothing, otherwise how many ticks it runs for.
  var transferTicks: Int?
  /// The Backup screen's state, when a fixture opens it.
  var backupState: BackupState?
  var backupSample = false
  var backupNoFolder = false
  var recents = ["boats at sunset", "mountains by a lake", "birthday cake", "receipt from the hardware shop"]
  var downloadedClips: Set<MediaID> = []
  private var listeners: [UUID: AsyncStream<ConnectionSnapshot>.Continuation] = [:]

  init(snapshot: ConnectionSnapshot) { self.snapshot = snapshot }

  func publish(_ new: ConnectionSnapshot) {
    snapshot = new
    listeners.values.forEach { $0.yield(new) }
  }

  func updates() -> AsyncStream<ConnectionSnapshot> {
    AsyncStream { continuation in
      let id = UUID()
      listeners[id] = continuation
      continuation.yield(snapshot)
    }
  }

  static var referenceNow: Date { Date() }

  static func connected(source: CatalogueInfo.Source = .synology, age: TimeInterval = 3 * 3600) -> ConnectionSnapshot {
    ConnectionSnapshot(hasLibrary: true,
                       synology: .reachable(address: "home-nas.synology.me", overInternet: true),
                       macNearby: false,
                       catalogue: CatalogueInfo(source: source, updatedAt: Date().addingTimeInterval(-age), itemCount: FixtureLibrary.itemCount, epoch: 1),
                       cacheBytes: 1_240_000_000)
  }

  func services() -> Services {
    let images = FixtureImages(world: self)
    return Services(library: FixtureStore(world: self), images: images, search: FixtureSearch(world: self),
                    connection: FixtureConnection(world: self), sync: FixtureSync(world: self),
                    clips: FixtureClips(world: self), saver: FixtureSaver(world: self), backup: FixtureBackup(world: self))
  }
}

enum FixtureLibrary {
  static func seed(_ id: MediaID) -> UInt64 { UInt64(id.raw.dropFirst(5)) ?? 0 }

  static let itemCount = 20_000

  /// Twenty thousand items over about six years. The first read happens off the main thread.
  static let items: [MediaItem] = {
    var rng = SeededGenerator(seed: 2026)
    let start = Date(timeIntervalSinceNow: -2 * 3600)
    let shapes: [(Int, Int)] = [(4032, 3024), (3024, 4032), (6000, 4000), (4000, 6000), (5712, 4284)]
    var when = start
    return (0..<itemCount).map { i in
      when = when.addingTimeInterval(-Double(rng.range(1800, 16_000)))
      let isClip = i % 7 == 3
      let shape = shapes[Int(rng.next() % UInt64(shapes.count))]
      return MediaItem(id: MediaID("item-\(i)"), kind: isClip ? .clip : .photo, capturedAt: when,
                       pixelWidth: isClip ? 3840 : shape.0, pixelHeight: isClip ? 2160 : shape.1,
                       duration: isClip ? 24 : nil,
                       fileName: isClip ? "MVI_\(1000 + i).MP4" : "IMG_\(1000 + i).HEIC")
    }
  }()

  static let everything = ArrayMediaList(items)

  /// The shape of a real NAS: media / iphone / 2026 / 10, with "media" as the indexed root.
  private static let sources = ["iphone", "camera"]

  private static let dateParts: [(year: Int, month: Int)] = {
    let calendar = Calendar.current
    return items.map {
      let c = calendar.dateComponents([.year, .month], from: $0.capturedAt)
      return (c.year ?? 0, c.month ?? 0)
    }
  }()

  /// The items under a folder, including its subfolders.
  static func items(under folderID: String) -> [MediaItem] {
    let path = folderID.split(separator: "/").map(String.init)
    if path[0] == "iceland road trip" { return stride(from: 1, to: items.count, by: 6).map { items[$0] } }
    guard let source = sources.firstIndex(of: path[0]) else { return [] }
    let year = path.count > 1 ? Int(path[1]) : nil
    let month = path.count > 2 ? Int(path[2]) : nil
    return items.indices.filter { i in
      i % 2 == source && (year == nil || dateParts[i].year == year) && (month == nil || dateParts[i].month == month)
    }.map { items[$0] }
  }

  /// One pass over the library, however many folders it makes.
  static let folders: [String: [Folder]] = {
    var counts: [String: Int] = [:]
    var years: [String: Set<Int>] = [:]
    var months: [String: Set<Int>] = [:]
    for i in items.indices {
      let source = sources[i % 2], part = dateParts[i]
      counts[source, default: 0] += 1
      counts["\(source)/\(part.year)", default: 0] += 1
      counts["\(source)/\(part.year)/\(String(format: "%02d", part.month))", default: 0] += 1
      years[source, default: []].insert(part.year)
      months["\(source)/\(part.year)", default: []].insert(part.month)
    }
    var tree: [String: [Folder]] = [:]
    tree[""] = sources.map { Folder(id: $0, name: $0, itemCount: counts[$0] ?? 0, hasSubfolders: true) } + [
      Folder(id: "iceland road trip", name: "iceland road trip", itemCount: items.count / 6, hasSubfolders: false),
      Folder(id: "new camera card", name: "new camera card", itemCount: 0, hasSubfolders: false),
    ]
    for source in sources {
      tree[source] = (years[source] ?? []).sorted(by: >).map { year in
        let id = "\(source)/\(year)"
        return Folder(id: id, name: String(year), itemCount: counts[id] ?? 0, hasSubfolders: true)
      }
      for year in years[source] ?? [] {
        let parent = "\(source)/\(year)"
        tree[parent] = (months[parent] ?? []).sorted(by: >).map { month in
          let name = String(format: "%02d", month), id = "\(parent)/\(name)"
          return Folder(id: id, name: name, itemCount: counts[id] ?? 0, hasSubfolders: false)
        }
      }
    }
    return tree
  }()

  static let people: [Person] = [("Maya", 412), ("Theo", 305), ("Grandma Ruth", 96), ("Sam", 73), ("Priya", 58), ("Jonas", 21)]
    .enumerated().map { i, person in
      Person(id: "person-\(i)", name: person.0, itemCount: person.1, face: items[i * 13 + 2])
    }

  static func place(_ seed: Int) -> String? { seed % 4 == 3 ? nil : places[seed % places.count] }
  static func camera(_ seed: Int) -> (String, String, String) { cameras[seed % cameras.count] }

  static let places = ["Vík, Iceland", "Lisbon, Portugal", "Bergen, Norway", "Kyoto, Japan", "Brighton, England"]
  static let cameras: [(String, String, String)] = [
    ("Sony α7 IV", "FE 24-70mm f/2.8 GM II", "1/320 s · f/5.6 · ISO 200"),
    ("Fujifilm X-T5", "XF 33mm f/1.4", "1/1000 s · f/2 · ISO 400"),
    ("iPhone 16 Pro", "Main camera 24 mm", "1/120 s · f/1.8 · ISO 80"),
  ]

  static func moments(for item: MediaItem) -> [Moment] {
    guard item.kind == .clip else { return [] }
    return [
      Moment(seconds: 3, kind: .scene, text: "Walking along the harbour"),
      Moment(seconds: 9, kind: .speech, text: "“So we drove up to the lake that morning.”"),
      Moment(seconds: 15, kind: .wordsOnScreen, text: "Sign reading “Ferry to the islands”"),
      Moment(seconds: 21, kind: .speech, text: "“Look at the boats, they're all coming in.”"),
    ]
  }
}

struct FixtureStore: LibraryStore {
  let world: FixtureWorld

  func list(in scope: LibraryScope) async throws -> any MediaList {
    if world.libraryFails { throw MediaFailure.synologyUnreachable }
    guard !world.libraryEmpty else { return ArrayMediaList([]) }
    switch scope {
    case .everything:
      return FixtureLibrary.everything
    case .folder(let folder):
      if world.foldersFail { throw MediaFailure.synologyUnreachable }
      return ArrayMediaList(FixtureLibrary.items(under: folder.id))
    case .person(let person):
      if world.personFailures > 0 {
        world.personFailures -= 1
        throw MediaFailure.synologyUnreachable
      }
      return ArrayMediaList(Array(FixtureLibrary.items.enumerated().filter { $0.offset % 9 == person.id.count % 9 }.map(\.element).prefix(person.itemCount)))
    }
  }

  func folders(under parent: Folder?) async throws -> [Folder] {
    FixtureLibrary.folders[parent?.id ?? ""] ?? []
  }

  func people() async throws -> [Person] {
    if world.peopleFailures > 0 {
      world.peopleFailures -= 1
      throw MediaFailure.synologyUnreachable
    }
    return world.hasPeople ? FixtureLibrary.people : []
  }

  func details(for item: MediaItem) async throws -> MediaDetails {
    let seed = Int(FixtureLibrary.seed(item.id))
    let camera = FixtureLibrary.camera(seed)
    return MediaDetails(place: FixtureLibrary.place(seed),
                        camera: camera.0, lens: camera.1, exposure: item.kind == .photo ? camera.2 : nil,
                        folderPath: "/volume1/photo/\(item.capturedAt.formatted(.dateTime.year()))",
                        formatName: item.kind == .photo ? "HEIC" : "HEVC in MP4",
                        fileBytes: item.kind == .photo ? 4_300_000 + Int64(seed % 40) * 100_000 : 480_000_000)
  }
}

final class FixtureImages: ImageLadder, @unchecked Sendable {
  private let world: FixtureWorld
  private let tiny = NSCache<NSString, UIImage>()
  private let thumbs = NSCache<NSString, UIImage>()
  private let originals = NSCache<NSString, UIImage>()
  private static let peopleFaces = Set(FixtureLibrary.people.map(\.face.id))

  init(world: FixtureWorld) {
    self.world = world
    thumbs.countLimit = 600
    tiny.countLimit = 4000
    originals.countLimit = 8
  }

  private func draw(_ item: MediaItem, longest: CGFloat) -> UIImage {
    let aspect = CGFloat(item.aspect)
    let size = aspect >= 1 ? CGSize(width: longest, height: (longest / aspect).rounded())
                           : CGSize(width: (longest * aspect).rounded(), height: longest)
    let seed = FixtureLibrary.seed(item.id)
    // A person's face is that item's own picture, so it sharpens into the same photo.
    return Self.peopleFaces.contains(item.id) ? FixtureArt.portrait(seed: seed, size: size)
                                              : FixtureArt.image(seed: seed, size: size)
  }

  private func item(_ id: MediaID) -> MediaItem? {
    guard let index = Int(id.raw.dropFirst(5)), FixtureLibrary.items.indices.contains(index) else { return nil }
    return FixtureLibrary.items[index]
  }

  func cachedImage(for id: MediaID) -> UIImage? {
    let key = id.raw as NSString
    if case .slowNetwork = world.imageMode {
      // A slow connection: only tiny.pack is on the phone until the ladder climbs.
      if let image = originals.object(forKey: key) ?? tiny.object(forKey: key) { return image }
    } else if let image = originals.object(forKey: key) ?? thumbs.object(forKey: key) ?? tiny.object(forKey: key) {
      return image
    }
    guard let item = item(id) else { return nil }
    let image = draw(item, longest: 28)
    tiny.setObject(image, forKey: key)
    return image
  }

  func thumbnail(for item: MediaItem) async -> UIImage? {
    let key = item.id.raw as NSString
    if case .slowNetwork = world.imageMode {
      guard (try? await Task.sleep(for: .seconds(2.5))) != nil else { return nil }
    }
    if let image = thumbs.object(forKey: key) { return image }
    // A tile scrolled past before its turn draws nothing.
    guard !Task.isCancelled else { return nil }
    let image = draw(item, longest: 420)
    thumbs.setObject(image, forKey: key)
    return image
  }

  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) { [self] in
        do {
          if let thumb = await thumbnail(for: item) { continuation.yield(.image(thumb, isOriginal: false)) }
          switch world.imageMode {
          case .fast:
            try await Task.sleep(for: .milliseconds(350))
          case .slowOriginal:
            let total: Int64 = 12_400_000
            for step in 0...120 {
              continuation.yield(.loading(doneBytes: total * Int64(step) / 120, totalBytes: total))
              try await Task.sleep(for: .milliseconds(250))
            }
          case .slowUnknownSize:
            for _ in 0...120 {
              continuation.yield(.loading(doneBytes: 0, totalBytes: nil))
              try await Task.sleep(for: .milliseconds(250))
            }
          case .slowNetwork:
            let total: Int64 = 12_400_000
            for step in 0...24 {
              continuation.yield(.loading(doneBytes: total * Int64(step) / 24, totalBytes: total))
              try await Task.sleep(for: .milliseconds(250))
            }
          case .fails(let failure):
            try await Task.sleep(for: .milliseconds(300))
            throw failure
          }
          let longest = min(2048, max(fittingPixels.width, fittingPixels.height))
          let full = draw(item, longest: longest)
          originals.setObject(full, forKey: item.id.raw as NSString)
          continuation.yield(.image(full, isOriginal: true))
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    for item in items {
      Task.detached(priority: .utility) { [self] in _ = await thumbnail(for: item) }
    }
  }
}

struct FixtureSearch: SearchService {
  let world: FixtureWorld

  func capability() async -> SearchCapability { world.capability }
  func recents() async -> [String] { world.recents }

  func remember(_ query: String) async {
    world.recents.removeAll { $0 == query }
    world.recents.insert(query, at: 0)
  }

  func forget(_ query: String) async { world.recents.removeAll { $0 == query } }
  func forgetAll() async { world.recents = [] }

  func isWarmingUp() async -> Bool {
    if case .modelLoading = world.searchMode { return !world.modelReady }
    return false
  }

  func filterOptions() async -> SearchFilterOptions {
    let calendar = Calendar.current
    let now = Date()
    let year = calendar.component(.year, from: now)
    var dates: [SearchDateRange] = []
    if let month = calendar.dateInterval(of: .month, for: now) {
      dates.append(SearchDateRange(id: "month", label: "This month", from: month.start, to: month.end))
    }
    for y in stride(from: year, through: year - 3, by: -1) {
      if let start = calendar.date(from: DateComponents(year: y)), let end = calendar.date(from: DateComponents(year: y + 1)) {
        dates.append(SearchDateRange(id: String(y), label: String(y), from: start, to: end))
      }
    }
    return SearchFilterOptions(people: world.hasPeople ? FixtureLibrary.people : [], places: FixtureLibrary.places,
                               dates: dates, cameras: FixtureLibrary.cameras.map(\.0))
  }

  private func passes(_ offset: Int, _ item: MediaItem, _ filters: SearchFilters) -> Bool {
    if let person = filters.person, offset % 9 != person.id.count % 9 { return false }
    if let place = filters.place, FixtureLibrary.place(offset) != place { return false }
    if let camera = filters.camera, FixtureLibrary.camera(offset).0 != camera { return false }
    if let kind = filters.kind, item.kind != kind { return false }
    if let date = filters.date, item.capturedAt < date.from || item.capturedAt >= date.to { return false }
    return true
  }

  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] {
    if world.searchFails { throw MediaFailure.synologyUnreachable }
    if case .neverAnswers = world.searchMode {
      try await Task.sleep(for: .seconds(3600))
    }
    if case .modelLoading = world.searchMode, !world.modelReady {
      try await Task.sleep(for: .seconds(6))
      world.modelReady = true
    }
    try await Task.sleep(for: .milliseconds(60))
    let q = query.lowercased()
    if q.contains("zzz") || q.contains("nothing") { return [] }
    let words: [(String, FixtureScene)] = [
      ("boat", .boats), ("sunset", .boats), ("mountain", .mountains), ("lake", .lake), ("forest", .forest),
      ("tree", .forest), ("city", .city), ("night", .city), ("dune", .dunes), ("desert", .dunes), ("snow", .snowRoad),
    ]
    let wanted = Set(words.filter { q.contains($0.0) }.map(\.1))
    let matches = FixtureLibrary.items.enumerated().filter { offset, item in
      let words = wanted.isEmpty ? (q.isEmpty || (offset &+ q.count) % 11 == 0) : wanted.contains(FixtureArt.scene(for: UInt64(offset)))
      return words && passes(offset, item, filters)
    }.map(\.element).prefix(240)
    return matches.map { item in
      SearchHit(item: item, moment: item.kind == .clip ? FixtureLibrary.moments(for: item)[1] : nil)
    }
  }
}

struct FixtureConnection: ConnectionService {
  let world: FixtureWorld

  var current: ConnectionSnapshot { world.snapshot }
  func updates() -> AsyncStream<ConnectionSnapshot> { world.updates() }

  static let triedAddresses = [
    AddressFailure(address: "home-nas.synology.me", reason: "timed out after 8 seconds"),
    AddressFailure(address: "192.168.1.20", reason: "not on this network"),
  ]

  func identify(code: String) async throws -> LibraryIdentity {
    try await Task.sleep(for: .milliseconds(200))
    if code == "fixture:expired" { throw PairingFailure.codeExpired }
    if code == "fixture:unreadable" { throw PairingFailure.codeNotReadable }
    if code == "fixture:identify-fails-once", world.identifyFailures > 0 {
      world.identifyFailures -= 1
      throw PairingFailure.synologyUnreachable(FixtureConnection.triedAddresses)
    }
    if code == "fixture:guest" { return LibraryIdentity(name: "Home library", fingerprint: "7F3A 92C1", guestFor: 86_400) }
    return LibraryIdentity(name: "Home library", fingerprint: "7F3A 92C1")
  }

  func pair(code: String, confirmed: LibraryIdentity) -> AsyncThrowingStream<PairingEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        do {
          let pause: Duration = code == "fixture:slow" ? .seconds(3600) : .milliseconds(600)
          continuation.yield(.working(.mac))
          try await Task.sleep(for: .milliseconds(500))
          continuation.yield(.passed(.mac))
          continuation.yield(.working(.synology))
          try await Task.sleep(for: pause)
          if code == "fixture:unreachable" { throw PairingFailure.synologyUnreachable(Self.triedAddresses) }
          if code == "fixture:too-old" { throw PairingFailure.serviceTooOld }
          continuation.yield(.passed(.synology))
          continuation.yield(.working(.outsideRoute))
          try await Task.sleep(for: .milliseconds(600))
          switch code {
          case "fixture:own-network": continuation.yield(.outsideRoute(.reachableOnlyOnItsOwnNetwork))
          case "fixture:no-mobile-data": continuation.yield(.outsideRoute(.skippedNoMobileData))
          default: continuation.yield(.outsideRoute(.reachable))
          }
          world.publish(FixtureWorld.connected())
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  func retry() async {
    try? await Task.sleep(for: .milliseconds(400))
    world.publish(FixtureWorld.connected())
  }

  func forget() async {
    world.recents = []
    world.publish(.noLibrary)
  }
}

struct FixtureSync: CatalogueSync {
  let world: FixtureWorld

  var continuesAfterLeaving: Bool { world.copyContinuesAfterLeaving }

  func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        do {
          let total: Int64 = 161_000_000
          if case .noSpace = world.copyMode {
            try await Task.sleep(for: .milliseconds(400))
            throw CopyFailure.notEnoughSpace(neededBytes: total)
          }
          if case .waitingForWiFi = world.copyMode, !overMobileData {
            continuation.yield(.waitingForWiFi(totalBytes: total))
            try await Task.sleep(for: .seconds(3600))
          }
          var done: Int64 = 0
          if case .resuming = world.copyMode {
            done = 60_000_000
            continuation.yield(.resuming(doneBytes: done, totalBytes: total))
            try await Task.sleep(for: .seconds(3))
          }
          while done < total {
            // Before iOS 26, leaving stops the copy; coming back picks up from the last byte.
            if !world.copyContinuesAfterLeaving, world.copyWasLeft {
              while world.copyIsAway { try await Task.sleep(for: .milliseconds(200)) }
              world.copyWasLeft = false
              continuation.yield(.resuming(doneBytes: done, totalBytes: total))
              try await Task.sleep(for: .seconds(2))
            }
            done = min(total, done + total / 120)
            continuation.yield(.copying(doneBytes: done, totalBytes: total))
            try await Task.sleep(for: .milliseconds(250))
            if case .fails = world.copyMode, done > total / 3 {
              throw CopyFailure.synologyUnreachable
            }
          }
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }
}

struct FixtureClips: ClipService {
  let world: FixtureWorld

  func route(for item: MediaItem) async -> ClipRoute {
    if world.downloadedClips.contains(item.id) { return await .stream(AVURLAsset(url: FixtureClip.url())) }
    switch world.clipMode {
    case .stream: return await .stream(AVURLAsset(url: FixtureClip.url()))
    case .tooBig: return .tooBigToStream(neededMbps: 100, availableMbps: 30, fileBytes: 1_260_000_000)
    case .notPlayable: return .notPlayableHere(containerName: "MKV", fileBytes: 2_400_000_000)
    case .fails(let failure): return .unavailable(failure)
    case .stopsPlaying: return .stream(AVURLAsset(url: URL(string: "http://127.0.0.1:9/nothing.mp4")!))
    case .stalls: return .stream(FixtureStallingAsset.make())
    }
  }

  func moments(for item: MediaItem) async -> [Moment] { FixtureLibrary.moments(for: item) }

  func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage? {
    let seed = FixtureLibrary.seed(item.id) &+ UInt64(seconds) * 7
    return await Task.detached(priority: .userInitiated) {
      FixtureArt.image(seed: seed, size: CGSize(width: 960, height: 540))
    }.value
  }

  func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        do {
          let total: Int64 = 1_260_000_000
          for step in 1...48 {
            continuation.yield(.progress(TransferProgress(doneBytes: total * Int64(step) / 48, totalBytes: total)))
            try await Task.sleep(for: .milliseconds(250))
          }
          world.downloadedClips.insert(item.id)
          continuation.yield(.finished(await FixtureClip.url()))
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }
}

/// A phone backup in the words of §10b: the strip above the tab bar, and the Backup screen in
/// each of §11.1's states.
final class FixtureBackup: BackupService, @unchecked Sendable {
  let world: FixtureWorld
  private let lock = NSLock()
  private var snapshot: BackupSnapshot
  private var listeners: [UUID: AsyncStream<BackupSnapshot>.Continuation] = [:]
  private var runner: Task<Void, Never>?
  private static let nas = BackupFolder(id: "c3c3c3c3c3c3c3c3", label: "Phone backup", writable: true)
  private static let sample = BackupFolder(id: "5a3b1e0f5a3b1e0f", label: "Sample NAS on this iPhone", writable: true)

  init(world: FixtureWorld) {
    self.world = world
    var s = BackupSnapshot(state: .off, backedUp: 1_204, total: 18_320, isSample: world.backupSample)
    if !world.backupNoFolder { s.folder = world.backupSample ? Self.sample : Self.nas }
    s.space = BackupSpace(limitBytes: 500_000_000_000, usedBytes: 41_200_000_000, roomBytes: 458_800_000_000)
    if let state = world.backupState {
      s.state = state
      if state == .running { s.currentName = "IMG_0412.HEIC"; s.currentFraction = 0.4 }
      if state == .limitReached { s.space = BackupSpace(limitBytes: 500_000_000_000, usedBytes: 500_000_000_000, roomBytes: 0) }
      if state == .done { s.backedUp = s.total }
    }
    if world.backupState == .running, world.backupSample { s.total = 120; s.backedUp = 37 }
    snapshot = s
    if let ticks = world.transferTicks { run(ticks: ticks) }
  }

  var current: BackupSnapshot { lock.withLock { snapshot } }

  func updates() -> AsyncStream<BackupSnapshot> {
    AsyncStream { continuation in
      let id = UUID()
      lock.withLock { listeners[id] = continuation }
      continuation.yield(current)
      continuation.onTermination = { [weak self] _ in self?.lock.withLock { self?.listeners[id] = nil } }
    }
  }

  private func change(_ body: (inout BackupSnapshot) -> Void) {
    let (value, list) = lock.withLock { () -> (BackupSnapshot, [AsyncStream<BackupSnapshot>.Continuation]) in
      body(&snapshot)
      return (snapshot, Array(listeners.values))
    }
    list.forEach { $0.yield(value) }
  }

  private func run(ticks: Int) {
    runner?.cancel()
    runner = Task {
      change { $0.state = .running }
      for _ in 0..<ticks {
        try? await Task.sleep(for: .seconds(1))
        if Task.isCancelled { return }
        change { $0.backedUp = min($0.total, $0.backedUp + 1); $0.currentName = "IMG_\(4000 + $0.backedUp).HEIC" }
      }
      change { $0.state = .done; $0.currentName = nil; $0.backedUp = $0.total }
    }
  }

  func folders() async throws -> [BackupFolder] {
    if world.backupSample { return [Self.sample] }
    return [Self.nas, BackupFolder(id: "d4d4d4d4d4d4d4d4", label: "Family backup", writable: false)]
  }
  func requestPhotosAccess() async -> Bool { true }
  func choose(folder: BackupFolder, layout: BackupLayout) async {
    change { $0.folder = folder; $0.layout = layout; $0.state = .off }
  }
  func setAutomatic(_ on: Bool) { change { $0.automatic = on } }
  func setAllowsSleep(_ on: Bool) { change { $0.allowsSleep = on } }
  func start(overMobileData: Bool) { run(ticks: 3600) }
  func stop() {
    runner?.cancel()
    change { $0.state = .off; $0.currentName = nil }
  }
  func notBackedUp() async -> [String] { (0..<min(60, current.toGo)).map { "fixture-\($0)" } }
  func thumbnail(ofPhoto id: String, side: CGFloat) async -> UIImage? {
    let hue = CGFloat(abs(id.hashValue) % 360) / 360
    return UIGraphicsImageRenderer(size: CGSize(width: side, height: side)).image { ctx in
      UIColor(hue: hue, saturation: 0.45, brightness: 0.7, alpha: 1).setFill()
      ctx.fill(CGRect(x: 0, y: 0, width: side, height: side))
    }
  }
}

struct FixtureSaver: PhotoSaver {
  let world: FixtureWorld

  func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        do {
          if world.photosRefused { throw MediaFailure.photosNotAllowed }
          let total: Int64 = 12_000_000
          for step in 1...12 {
            continuation.yield(TransferProgress(doneBytes: total * Int64(step) / 12, totalBytes: total))
            try await Task.sleep(for: .milliseconds(120))
          }
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }
}

/// An asset whose data never arrives, so the player stays waiting.
enum FixtureStallingAsset {
  private final class Loader: NSObject, AVAssetResourceLoaderDelegate {
    func resourceLoader(_ resourceLoader: AVAssetResourceLoader,
                        shouldWaitForLoadingOfRequestedResource loadingRequest: AVAssetResourceLoadingRequest) -> Bool { true }
  }

  private static let loader = Loader()

  static func make() -> AVURLAsset {
    let asset = AVURLAsset(url: URL(string: "fixture-stall://clip/none.mp4")!)
    asset.resourceLoader.setDelegate(loader, queue: .global())
    return asset
  }
}

/// A real 24-second H.264 file, generated once, so AVPlayer and the moment ticks are genuine.
enum FixtureClip {
  private static let task = Task<URL, Never>.detached(priority: .userInitiated) { generate() }

  static func url() async -> URL { await task.value }

  private static func generate() -> URL {
    let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("fixture-clip.mp4")
    if FileManager.default.fileExists(atPath: url.path) { return url }
    let temp = url.deletingLastPathComponent().appendingPathComponent(UUID().uuidString + ".mp4")
    let width = 960, height = 540, fps: Int32 = 15, seconds = 24
    guard let writer = try? AVAssetWriter(outputURL: temp, fileType: .mp4) else { return url }
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
      AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: width, AVVideoHeightKey: height,
    ])
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
      kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
      kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
    ])
    writer.add(input)
    writer.startWriting()
    writer.startSession(atSourceTime: .zero)
    let scenes = (0..<4).map { FixtureArt.image(seed: UInt64(100 + $0 * 3), size: CGSize(width: 1300, height: 700)).cgImage! }
    for frame in 0..<(Int(fps) * seconds) {
      while !input.isReadyForMoreMediaData { usleep(1000) }
      guard let pool = adaptor.pixelBufferPool else { break }
      var buffer: CVPixelBuffer?
      CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
      guard let buffer else { break }
      CVPixelBufferLockBaseAddress(buffer, [])
      let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
                              bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
      let t = Double(frame) / Double(fps)
      let scene = scenes[min(3, Int(t / 6))]
      let pan = CGFloat(t.truncatingRemainder(dividingBy: 6) / 6) * 320
      context.draw(scene, in: CGRect(x: -pan, y: -80, width: 1300, height: 700))
      CVPixelBufferUnlockBaseAddress(buffer, [])
      adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: fps))
    }
    input.markAsFinished()
    let done = DispatchSemaphore(value: 0)
    writer.finishWriting { done.signal() }
    done.wait()
    try? FileManager.default.moveItem(at: temp, to: url)
    return url
  }
}
