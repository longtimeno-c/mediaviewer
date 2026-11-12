// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue on this phone and the three services over it (docs/plans/mobile.md M5, M6's local
// half). Holds the current generation; a swap opens the new one beside the old, and the old one
// goes when the last list or search using it does. Everything below runs off the main thread
// except what `MediaList.item(at:)` and `cachedImage(for:)` promise to answer within a frame.
import CMobileSearch
import Foundation
import ImageIO
import MobileKit
import UIKit

/// Where the phone keeps a library, as the sync installs it: `<root>/gen/<epoch>-<generation>/`,
/// the manifest's shared `models/` under the root, `<root>/installed.json` naming the generation
/// in force, and the engine's own work folders in `<root>/engine/`. Excluded from iCloud backup
/// (§8.3): a catalogue of the whole library must not go there.
public struct LibraryLocation: Sendable {
  public let root: URL
  /// The library this phone is paired with, when known: an installed generation of any other
  /// library (left behind by an earlier pairing) is never opened. "" or nil: not known yet.
  public var pairedLibrary: @Sendable () -> String?

  public init(root: URL, pairedLibrary: @escaping @Sendable () -> String? = { nil }) {
    self.root = root
    self.pairedLibrary = pairedLibrary
  }

  /// `Application Support/library`. Cheap: no disk.
  public static var standard: LibraryLocation {
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    return LibraryLocation(root: support.appendingPathComponent("library", isDirectory: true))
  }

  /// The file the sync replaces atomically when a generation has landed.
  public var installedFile: URL { root.appendingPathComponent("installed.json") }

  var generations: URL { root.appendingPathComponent("gen", isDirectory: true) }
  /// A generation opened with sound search's model gets a work folder of its own: the engine of
  /// the one opened without it may still be reading the other.
  func work(for generation: URL, sound: Bool = false) -> URL {
    root.appendingPathComponent("engine", isDirectory: true)
      .appendingPathComponent(generation.lastPathComponent + (sound ? "+sound" : ""), isDirectory: true)
  }

  /// Sound search's model (§9; src/mobile/sound_tower.h), as the link downloads it.
  public var soundFolder: URL {
    root.appendingPathComponent("sound", isDirectory: true).appendingPathComponent("clap-general", isDirectory: true)
  }

  /// Whether sound search's model is here, verified: the record the download writes last. The
  /// core checks it whole (each file's size, every hash named) when the library opens.
  var hasSoundModel: Bool { FileManager.default.fileExists(atPath: soundFolder.appendingPathComponent("verified.txt").path) }

  private struct Installed: Decodable {
    let dir: String
    let library_id: String?
  }

  /// The generation `installed.json` names, if it has a manifest. Reads the disk.
  public func installedGeneration() -> URL? {
    guard let data = try? Data(contentsOf: installedFile),
          let installed = try? JSONDecoder().decode(Installed.self, from: data) else { return nil }
    let dir = installed.dir
    guard !dir.isEmpty, dir != ".", dir != "..", !dir.contains("/") else { return nil }
    if let paired = pairedLibrary(), !paired.isEmpty, installed.library_id != paired { return nil }
    let url = generations.appendingPathComponent(dir, isDirectory: true)
    return FileManager.default.fileExists(atPath: url.appendingPathComponent("manifest.json").path) ? url : nil
  }

  /// Marks the folder never to go to iCloud (§8.3).
  public func excludeFromBackup() {
    var url = root
    try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    var values = URLResourceValues()
    values.isExcludedFromBackup = true
    try? url.setResourceValues(values)
  }
}

public final class LocalCatalogue: @unchecked Sendable {
  public let location: LibraryLocation
  private let lock = NSLock()
  private var current: Generation?
  private var opening: Task<Generation, Error>?
  /// Counts Forgets. An open that began before the latest one is never put in force.
  private var forgets = 0
  /// Every generation still held by a list or a result, by serial, so an item from before a swap
  /// still finds its own tiles.
  private var open: [UInt64: WeakGeneration] = [:]
  private let warmQueue = DispatchQueue(label: "mediaviewer.catalogue.warm", qos: .utility)

  private struct WeakGeneration { weak var value: Generation? }

  private func register(_ g: Generation) {
    lock.withLock {
      open = open.filter { $0.value.value != nil }
      open[g.serial] = WeakGeneration(value: g)
    }
  }

  /// The generation in force, if one has opened.
  func generationInForce() -> Generation? { lock.withLock { current } }

  func generation(serial: UInt64) -> Generation? {
    lock.withLock { open[serial]?.value }
  }

  /// After a tile has painted: the shapes from the tiles, then the model, on a queue of their
  /// own so neither competes with the first paint (§9 "Readiness").
  func afterFirstPaint(_ g: Generation) {
    guard g.claimWarmUp() else { return }
    let queue = warmQueue
    // Two turns of the main queue: past the update that puts the tile on screen.
    DispatchQueue.main.async {
      DispatchQueue.main.async {
        queue.async {
          _ = mv_mobile_library_fill_aspects(g.raw)
          _ = mv_mobile_search_start(g.raw)
        }
      }
    }
  }

  public init(location: LibraryLocation) {
    self.location = location
  }

  /// Tests only: runs on the opening thread before a generation opens, to hold an open open.
  nonisolated(unsafe) static var beforeOpen: (@Sendable (URL) -> Void)?

  private static func open(_ directory: URL, in root: LibraryLocation) throws -> Generation {
    beforeOpen?(directory)
    let sound = root.hasSoundModel
    return try Generation(directory: directory, libraryRoot: root.root, work: root.work(for: directory, sound: sound),
                          sound: sound ? root.soundFolder : nil)
  }

  /// Puts `g` in force unless a newer (epoch, generation) already is: opens can finish in any
  /// order, and the newest always wins.
  private func offer(_ g: Generation, startedAt era: Int) {
    register(g)
    lock.withLock {
      guard era == forgets else { return }  // a forgotten library's open, finishing late
      if let current, !g.isNewer(than: current) { return }
      current = g
    }
  }

  private func inForce() -> Generation? { lock.withLock { current } }

  /// The generation in force, opening it on first use.
  func generation() async throws -> Generation {
    if let g = inForce() { return g }
    let task: Task<Generation, Error> = lock.withLock {
      if let opening { return opening }
      let root = location
      let era = forgets
      let made = Task.detached(priority: .userInitiated) { [self] () throws -> Generation in
        guard let directory = root.installedGeneration() else { throw CatalogueOpenFailure.notInstalled }
        root.excludeFromBackup()
        let g = try Self.open(directory, in: root)
        offer(g, startedAt: era)  // inside the task: whoever awaits it finds it already offered
        return g
      }
      opening = made
      return made
    }
    do {
      let opened = try await task.value
      return inForce() ?? opened
    } catch {
      lock.withLock { if opening == task { opening = nil } }
      throw error
    }
  }

  /// The library was forgotten: nothing of it stays in force, so the next library's first
  /// generation opens even when its (epoch, generation) is no newer than the forgotten one's.
  /// Lists and results still holding the old generation keep it until they let go.
  public func forgetInForce() {
    lock.withLock {
      current = nil
      opening = nil
      forgets += 1
    }
  }

  /// Opens what `installed.json` now names: call this on the "catalogue changed" signal.
  public func reloadInstalled() async throws {
    let root = location
    let directory = try await Task.detached(priority: .userInitiated) { () throws -> URL in
      guard let found = root.installedGeneration() else { throw CatalogueOpenFailure.notInstalled }
      return found
    }.value
    try await swap(to: directory)
  }

  /// Opens `directory` and puts it in force, unless a newer generation is. Whoever holds the old
  /// one keeps it until they let go.
  public func swap(to directory: URL) async throws {
    // A first open still in flight is compared too: it may be the newer one.
    let era = lock.withLock { forgets }
    if let pending = lock.withLock({ current == nil ? opening : nil }) { _ = try? await pending.value }
    let root = location
    let new = try await Task.detached(priority: .userInitiated) { () throws -> Generation in
      try Self.open(directory, in: root)
    }.value
    offer(new, startedAt: era)
  }

  public func info() async -> CatalogueInfo? {
    guard let g = try? await generation() else { return nil }
    return Self.info(of: g)
  }

  /// What is known without waiting: nil until the generation has opened.
  public var infoIfOpen: CatalogueInfo? { lock.withLock { current.map(Self.info(of:)) } }

  private static func info(of g: Generation) -> CatalogueInfo {
    CatalogueInfo(source: .synology, updatedAt: Date(timeIntervalSince1970: TimeInterval(g.createdUnix)), itemCount: g.itemCount)
  }

  public var library: LibraryStore { CoreLibrary(catalogue: self) }
  public var images: ImageLadder { CoreImages(catalogue: self) }
  public var search: SearchService { CoreSearch(catalogue: self) }
}

// ---- library --------------------------------------------------------------------------------

private struct CoreLibrary: LibraryStore {
  let catalogue: LocalCatalogue

  func list(in scope: LibraryScope) async throws -> any MediaList {
    let g = try await catalogue.generation()
    try Task.checkCancellation()
    let list: CoreMediaList
    switch scope {
    case .everything: list = try CoreMediaList(generation: g, scope: MV_MOBILE_SCOPE_EVERYTHING, key: nil)
    case .folder(let f): list = try CoreMediaList(generation: g, scope: MV_MOBILE_SCOPE_FOLDER, key: f.id)
    case .person(let p): list = try CoreMediaList(generation: g, scope: MV_MOBILE_SCOPE_PERSON, key: p.id)
    }
    return list
  }

  func folders(under parent: Folder?) async throws -> [Folder] {
    let g = try await catalogue.generation()
    var out: [Folder] = []
    let status = withUnsafeMutablePointer(to: &out) { ptr in
      parent.map(\.id).withOptionalCString { key in
        mv_mobile_library_folders(g.raw, key, { user, row in
          guard let user, let row, let key = row.pointee.key, let name = row.pointee.name else { return }
          user.assumingMemoryBound(to: [Folder].self).pointee.append(
            Folder(id: String(cString: key), name: String(cString: name),
                   itemCount: Int(row.pointee.item_count), hasSubfolders: row.pointee.has_subfolders != 0))
        }, ptr)
      }
    }
    guard status == MV_MOBILE_OK else { throw CatalogueOpenFailure.unreadable }
    return out
  }

  func people() async throws -> [Person] {
    let g = try await catalogue.generation()
    return try peopleRows(g).map { row in
      Person(id: row.id, name: row.name, itemCount: row.count, face: try itemFor(g, asset: row.face))
    }
  }

  func details(for item: MediaItem) async throws -> MediaDetails {
    // The item's own generation: after a swap the same id can be another file.
    guard let g = catalogue.generation(serial: item.id.catalogue) else { throw CatalogueOpenFailure.unreadable }
    guard let id = Int64(item.id.raw) else { throw CatalogueOpenFailure.unreadable }
    var d = mv_mobile_details()
    guard mv_mobile_library_details(g.raw, id, &d) == MV_MOBILE_OK else { throw CatalogueOpenFailure.unreadable }
    func text<T>(_ t: T) -> String { withUnsafeBytes(of: t) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } }
    var exposure: [String] = []
    if d.shutter_den > 0 { exposure.append("1/\(d.shutter_den) s") }
    else if d.shutter_us > 0 { exposure.append(String(format: "%.2f s", Double(d.shutter_us) / 1_000_000)) }
    if d.aperture_x100 > 0 { exposure.append(String(format: "ƒ/%.1f", Double(d.aperture_x100) / 100)) }
    if d.focal_mm > 0 { exposure.append("\(d.focal_mm) mm") }
    if d.iso > 0 { exposure.append("ISO \(d.iso)") }
    let place = text(d.place), camera = text(d.camera), lens = text(d.lens), format = text(d.format), folder = text(d.folder)
    return MediaDetails(place: place.isEmpty ? nil : place, camera: camera.isEmpty ? nil : camera, lens: lens.isEmpty ? nil : lens,
                        exposure: exposure.isEmpty ? nil : exposure.joined(separator: " · "),
                        folderPath: folder, formatName: format, fileBytes: d.file_bytes)
  }
}

private func itemFor(_ g: Generation, asset: Int64) throws -> MediaItem {
  var row = mv_mobile_row()
  guard mv_mobile_library_row(g.raw, asset, &row) == MV_MOBILE_OK else { throw CatalogueOpenFailure.unreadable }
  return mediaItem(row, in: g)
}

private struct PersonRow {
  let id, name: String
  let count: Int
  let face: Int64
}

private func peopleRows(_ g: Generation) throws -> [PersonRow] {
  var out: [PersonRow] = []
  let status = withUnsafeMutablePointer(to: &out) { ptr in
    mv_mobile_library_people(g.raw, { user, row in
      guard let user, let row, let key = row.pointee.key, let name = row.pointee.name else { return }
      user.assumingMemoryBound(to: [PersonRow].self).pointee.append(
        PersonRow(id: String(cString: key), name: String(cString: name),
                  count: Int(row.pointee.item_count), face: row.pointee.face_asset_id))
    }, ptr)
  }
  guard status == MV_MOBILE_OK else { throw CatalogueOpenFailure.unreadable }
  return out.filter { (try? itemFor(g, asset: $0.face)) != nil }
}

// ---- tiles ----------------------------------------------------------------------------------

private final class CoreImages: ImageLadder, @unchecked Sendable {
  let catalogue: LocalCatalogue
  private let cache: NSCache<NSString, UIImage>

  init(catalogue: LocalCatalogue) {
    self.catalogue = catalogue
    let c = NSCache<NSString, UIImage>()
    c.totalCostLimit = 96 << 20  // decoded tiles, on-screen plus a few screens back
    cache = c
  }

  /// A tile is its generation's: the same asset id in the next one can be another picture.
  private static func key(_ id: MediaID) -> NSString { "\(id.catalogue)/\(id.raw)" as NSString }

  func cachedImage(for id: MediaID) -> UIImage? { cache.object(forKey: Self.key(id)) }

  /// The paint path (a tile, a face, a poster on screen): the first one for the generation in
  /// force starts what waits for the first paint.
  func thumbnail(for item: MediaItem) async -> UIImage? {
    let image = tile(for: item)
    if image != nil, let g = catalogue.generation(serial: item.id.catalogue), g === catalogue.generationInForce() {
      catalogue.afterFirstPaint(g)
    }
    return image
  }

  /// A tile into the cache, and nothing else: prefetch's path.
  private func tile(for item: MediaItem) -> UIImage? {
    let key = Self.key(item.id)
    if let hit = cache.object(forKey: key) { return hit }
    // The item's own generation, still held by the list it came from; never the newest.
    guard let g = catalogue.generation(serial: item.id.catalogue), let id = Int64(item.id.raw),
          let image = Self.decode(g, id) else { return nil }
    let cost = (image.cgImage?.bytesPerRow ?? 256) * (image.cgImage?.height ?? 256)
    cache.setObject(image, forKey: key, cost: cost)
    return image
  }

  private static func decode(_ g: Generation, _ id: Int64) -> UIImage? {
    var bytes: UnsafePointer<UInt8>?
    var length: UInt32 = 0
    guard mv_mobile_library_tile(g.raw, id, &bytes, &length) == MV_MOBILE_OK, let bytes else { return nil }
    let data = Data(bytesNoCopy: UnsafeMutablePointer(mutating: bytes), count: Int(length), deallocator: .none)
    guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
    let options: [CFString: Any] = [kCGImageSourceShouldCacheImmediately: true]
    guard let cg = CGImageSourceCreateImageAtIndex(source, 0, options as CFDictionary) else { return nil }
    return withExtendedLifetime(g) { UIImage(cgImage: cg) }
  }

  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        if let tile = await thumbnail(for: item) { continuation.yield(.image(tile, isOriginal: false)) }
        // The remote half of the ladder is the media path's.
        continuation.finish(throwing: MediaFailure.synologyUnreachable)
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    for item in items {
      guard cache.object(forKey: Self.key(item.id)) == nil else { continue }
      Task.detached(priority: .utility) { [self] in _ = tile(for: item) }
    }
  }
}

// ---- search ---------------------------------------------------------------------------------

private final class CoreSearch: SearchService, @unchecked Sendable {
  private static let queue = DispatchQueue(label: "mediaviewer.search", qos: .userInitiated)
  private static let recentsCap = 20

  let catalogue: LocalCatalogue
  private let recentsLock = NSLock()
  private var cachedRecents: [String]?

  init(catalogue: LocalCatalogue) { self.catalogue = catalogue }

  private var recentsFile: URL { catalogue.location.root.appendingPathComponent("recent-searches.json") }

  func capability() async -> SearchCapability {
    guard let g = try? await catalogue.generation() else { return .ready }
    let status = await run { mv_mobile_search_start(g.raw) }
    return status == MV_MOBILE_ERR_UNSUPPORTED_TOWER ? .unsupportedTower : .ready
  }

  /// Blocking C calls wait here, off the cooperative pool.
  private func run<R>(_ body: @escaping @Sendable () -> R) async -> R {
    await withCheckedContinuation { continuation in Self.queue.async { continuation.resume(returning: body()) } }
  }

  func recents() async -> [String] {
    recentsLock.withLock {
      if let cachedRecents { return cachedRecents }
      let read = (try? JSONDecoder().decode([String].self, from: Data(contentsOf: recentsFile))) ?? []
      cachedRecents = read
      return read
    }
  }

  private func save(_ list: [String]) {
    recentsLock.withLock { cachedRecents = list }
    catalogue.location.excludeFromBackup()  // the folder, so the words people search for stay here
    if let data = try? JSONEncoder().encode(list) { try? data.write(to: recentsFile, options: .atomic) }
  }

  func remember(_ query: String) async {
    var list = await recents()
    list.removeAll { $0 == query }
    list.insert(query, at: 0)
    if list.count > Self.recentsCap { list.removeLast(list.count - Self.recentsCap) }
    save(list)
  }

  func forget(_ query: String) async {
    var list = await recents()
    list.removeAll { $0 == query }
    save(list)
  }

  func forgetAll() async { save([]) }

  func isWarmingUp() async -> Bool {
    guard let g = try? await catalogue.generation() else { return false }
    return mv_mobile_search_state(g.raw) == MV_MOBILE_SEARCH_WARMING
  }

  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] {
    let g = try await catalogue.generation()
    let text = QueryText.compose(query, filters: filters)
    return try await withTaskCancellationHandler {
      try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<[SearchHit], Error>) in
        Self.queue.async {
          do {
            continuation.resume(returning: try Self.answer(g, text: text))
          } catch {
            continuation.resume(throwing: error)
          }
        }
      }
    } onCancel: {
      mv_mobile_search_cancel(g.raw)
    }
  }

  // The phone shows photos and clips: nothing else asks for a tile here.
  private static func answer(_ g: Generation, text: String) throws -> [SearchHit] {
    let started = mv_mobile_search_start(g.raw)
    if started == MV_MOBILE_ERR_UNSUPPORTED_TOWER { return [] }
    guard started == MV_MOBILE_OK else { throw MediaFailure.synologyUnreachable }
    var results: OpaquePointer?
    let status = text.withCString { mv_mobile_search_run(g.raw, $0, MV_MOBILE_SEARCH_PHOTOS | MV_MOBILE_SEARCH_CLIPS, 0, &results) }
    if status == MV_MOBILE_ERR_CANCELLED { throw CancellationError() }
    if status == MV_MOBILE_ERR_UNSUPPORTED_TOWER { return [] }
    guard status == MV_MOBILE_OK, let results else { throw MediaFailure.synologyUnreachable }
    defer { mv_mobile_results_close(results) }
    var hits: [SearchHit] = []
    let n = mv_mobile_results_count(results)
    hits.reserveCapacity(Int(n))
    for i in 0..<n {
      var hit = mv_mobile_hit()
      guard mv_mobile_results_at(results, i, &hit) == MV_MOBILE_OK else { continue }
      guard let item = try? itemFor(g, asset: hit.asset_id) else { continue }
      hits.append(SearchHit(item: item, moment: moment(of: hit, results: results, index: i)))
    }
    return hits
  }

  private static func moment(of hit: mv_mobile_hit, results: OpaquePointer, index: UInt32) -> Moment? {
    guard hit.kind == MV_MOBILE_SEARCH_CLIPS, hit.pts_ms >= 0 else { return nil }
    let text = String(cString: mv_mobile_results_snippet(results, index))
    let kind: Moment.Kind
    if hit.match & MV_MOBILE_MATCH_SPEECH != 0 { kind = .speech }
    else if hit.match & MV_MOBILE_MATCH_TEXT != 0 { kind = .wordsOnScreen }
    else if hit.match & MV_MOBILE_MATCH_SOUND != 0 { kind = .sound }  // what the clip sounds like (§9)
    else { kind = .scene }
    return Moment(seconds: TimeInterval(hit.pts_ms) / 1000, kind: kind, text: text)
  }

  func filterOptions() async -> SearchFilterOptions {
    guard let g = try? await catalogue.generation() else { return .none }
    let people = (try? await CoreLibrary(catalogue: catalogue).people()) ?? []
    final class Sink { var places: [String] = []; var cameras: [String] = []; var years: [Int] = [] }
    let sink = Sink()
    _ = mv_mobile_library_filter_options(
      g.raw,
      { u, v in guard let u, let v else { return }; Unmanaged<Sink>.fromOpaque(u).takeUnretainedValue().places.append(String(cString: v)) },
      { u, v in guard let u, let v else { return }; Unmanaged<Sink>.fromOpaque(u).takeUnretainedValue().cameras.append(String(cString: v)) },
      { u, y in guard let u else { return }; Unmanaged<Sink>.fromOpaque(u).takeUnretainedValue().years.append(Int(y)) },
      Unmanaged.passUnretained(sink).toOpaque())
    return SearchFilterOptions(people: people, places: sink.places, dates: Self.dates(years: sink.years), cameras: sink.cameras)
  }

  private static func dates(years: [Int]) -> [SearchDateRange] {
    let calendar = Calendar.current
    var out: [SearchDateRange] = []
    for year in years {
      if let start = calendar.date(from: DateComponents(year: year)), let end = calendar.date(from: DateComponents(year: year + 1)) {
        out.append(SearchDateRange(id: String(year), label: String(year), from: start, to: end))
      }
    }
    return out
  }
}

/// A search and its filters, in the query language the desktop speaks (query.h): the filters are
/// words in the query, so the phone and the Mac answer the same way.
enum QueryText {
  static func compose(_ query: String, filters: SearchFilters) -> String {
    var terms: [String] = query.isEmpty ? [] : [query]
    if let person = filters.person { terms.append("person:\(quoted(person.name))") }
    if let place = filters.place { terms.append("place:\(quoted(place))") }
    if let camera = filters.camera { terms.append("camera:\(quoted(camera))") }
    if let kind = filters.kind { terms.append(kind == .clip ? "is:video" : "is:photo") }
    if let date = filters.date {
      let calendar = Calendar.current
      terms.append("since:\(day(calendar.dateComponents([.year, .month, .day], from: date.from)))")
      terms.append("before:\(day(calendar.dateComponents([.year, .month, .day], from: date.to)))")
    }
    return terms.joined(separator: " ")
  }

  private static func quoted(_ text: String) -> String { "\"\(text.replacingOccurrences(of: "\"", with: ""))\"" }

  private static func day(_ d: DateComponents) -> String {
    String(format: "%04d-%02d-%02d", d.year ?? 0, d.month ?? 1, d.day ?? 1)
  }
}
