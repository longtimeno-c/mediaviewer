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

/// Where the phone keeps a library, as the NAS lays it out (§5): `<root>/gen/000123/`, the shared
/// `models/`, and the engine's own work folders in `<root>/engine/`. Excluded from iCloud backup
/// (§8.3): a catalogue of the whole library must not go there.
public struct LibraryLocation: Sendable {
  public let root: URL

  public init(root: URL) { self.root = root }

  /// `Application Support/Library`. Cheap: no disk.
  public static var standard: LibraryLocation {
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    return LibraryLocation(root: support.appendingPathComponent("Library", isDirectory: true))
  }

  var generations: URL { root.appendingPathComponent("gen", isDirectory: true) }
  func work(for generation: URL) -> URL {
    root.appendingPathComponent("engine", isDirectory: true).appendingPathComponent(generation.lastPathComponent, isDirectory: true)
  }

  /// The highest-numbered generation with a manifest. Reads the disk.
  public func newestGeneration() -> URL? {
    guard let names = try? FileManager.default.contentsOfDirectory(atPath: generations.path) else { return nil }
    let numbered = names.compactMap { name -> (UInt64, String)? in
      guard let n = UInt64(name), name.allSatisfy(\.isNumber) else { return nil }
      return (n, name)
    }.sorted { $0.0 > $1.0 }
    for (_, name) in numbered {
      let dir = generations.appendingPathComponent(name, isDirectory: true)
      if FileManager.default.fileExists(atPath: dir.appendingPathComponent("manifest.json").path) { return dir }
    }
    return nil
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

  public init(location: LibraryLocation) {
    self.location = location
  }

  /// The generation in force, opening it on first use.
  func generation() async throws -> Generation {
    let task: Task<Generation, Error> = lock.withLock {
      if let opening { return opening }
      let root = location
      let made = Task.detached(priority: .userInitiated) { () throws -> Generation in
        guard let directory = root.newestGeneration() else { throw CatalogueOpenFailure.notInstalled }
        root.excludeFromBackup()
        return try Generation(directory: directory, libraryRoot: root.root, work: root.work(for: directory))
      }
      opening = made
      return made
    }
    do {
      let opened = try await task.value
      // Only the open still in force: a swap meanwhile has already set the newer one.
      lock.withLock { if opening == task { current = opened } }
      return opened
    } catch {
      lock.withLock { if opening == task { opening = nil } }
      throw error
    }
  }

  /// Opens `directory` and makes it the one in force. The sync calls this when a generation has
  /// landed. Whoever holds the old one keeps it until they let go.
  public func swap(to directory: URL) async throws {
    let root = location
    let new = try await Task.detached(priority: .userInitiated) { () throws -> Generation in
      try Generation(directory: directory, libraryRoot: root.root, work: root.work(for: directory))
    }.value
    lock.withLock {
      current = new
      opening = Task { new }
    }
  }

  public func info() async -> CatalogueInfo? {
    guard let g = try? await generation() else { return nil }
    return Self.info(of: g)
  }

  /// Which generation tiles belong to: an asset id can mean another picture in the next one.
  var generationNumber: UInt64 { lock.withLock { current?.number ?? 0 } }

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
    // After the grid's first paint and out of its way: shapes from the tiles, and the model
    // loading, so the first search rarely waits for it (§9).
    Task.detached(priority: .utility) {
      mv_mobile_library_fill_aspects(g.raw)
      _ = mv_mobile_search_start(g.raw)
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
    let g = try await catalogue.generation()
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
  return mediaItem(row)
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
  private let shownLock = NSLock()
  private var shownGeneration: UInt64 = 0

  init(catalogue: LocalCatalogue) {
    self.catalogue = catalogue
    let c = NSCache<NSString, UIImage>()
    c.totalCostLimit = 96 << 20  // decoded tiles, on-screen plus a few screens back
    cache = c
  }

  /// A generation swap empties the cache: ids can be reused.
  private func dropTilesOfOlderGenerations() {
    let number = catalogue.generationNumber
    let changed = shownLock.withLock {
      defer { shownGeneration = number }
      return shownGeneration != number
    }
    if changed { cache.removeAllObjects() }
  }

  func cachedImage(for id: MediaID) -> UIImage? {
    dropTilesOfOlderGenerations()
    return cache.object(forKey: id.raw as NSString)
  }

  func thumbnail(for item: MediaItem) async -> UIImage? {
    dropTilesOfOlderGenerations()
    if let hit = cache.object(forKey: item.id.raw as NSString) { return hit }
    guard let g = try? await catalogue.generation(), let id = Int64(item.id.raw), let image = Self.decode(g, id) else { return nil }
    let cost = (image.cgImage?.bytesPerRow ?? 256) * (image.cgImage?.height ?? 256)
    cache.setObject(image, forKey: item.id.raw as NSString, cost: cost)
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
      guard cache.object(forKey: item.id.raw as NSString) == nil else { continue }
      Task.detached(priority: .utility) { [self] in _ = await thumbnail(for: item) }
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
      mv_mobile_results_at(results, i, &hit)
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
