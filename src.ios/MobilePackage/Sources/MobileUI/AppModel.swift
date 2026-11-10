// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the app is showing. Every state a screen can be in is a value here, so a test harness can
// put the app in that state without any code of its own in a release build.
import MobileKit
import Observation
import SwiftUI

public struct GridEntry: Identifiable, Hashable, Sendable {
  public let item: MediaItem
  public let moment: Moment?
  /// Where it sits in the grid's whole list, so opening it costs no search.
  public let position: Int
  public let id: String

  public init(item: MediaItem, moment: Moment? = nil, position: Int = 0) {
    self.item = item
    self.moment = moment
    self.position = position
    id = moment.map { "\(item.id.raw)@\($0.seconds)" } ?? item.id.raw
  }

  /// Only what is on screen asks, so a large library formats no dates up front.
  public var accessibilityName: String {
    "\(item.kind == .clip ? "Clip" : "Photo"), \(item.capturedAt.formatted(date: .long, time: .shortened))"
  }
}

public struct GridSection: Identifiable, Hashable, Sendable {
  public let id: String
  public let title: String?
  /// Which positions of the grid's whole list this section shows.
  public let range: Range<Int>

  public init(id: String, title: String?, range: Range<Int>) {
    self.id = id
    self.title = title
    self.range = range
  }
}

/// What a grid and the viewer page through. Entries are made as they are asked for, so a large
/// library holds no per-item Swift values.
public struct GridSource: Sendable {
  public let count: Int
  private let make: @Sendable (Int) -> GridEntry

  public static let empty = GridSource(count: 0) { _ in fatalError("empty list") }

  private init(count: Int, make: @escaping @Sendable (Int) -> GridEntry) {
    self.count = count
    self.make = make
  }

  public init(list: any MediaList) {
    self.init(count: list.count) { GridEntry(item: list.item(at: $0), position: $0) }
  }

  public init(entries: [GridEntry]) {
    self.init(count: entries.count) { entries[$0] }
  }

  public subscript(index: Int) -> GridEntry { make(index) }
  public var indices: Range<Int> { 0..<count }
  public var isEmpty: Bool { count == 0 }

  /// The same list with one entry swapped, such as a clip opening at a moment.
  public func replacing(_ index: Int, with entry: GridEntry) -> GridSource {
    let make = make
    return GridSource(count: count) { $0 == index ? entry : make($0) }
  }
}

public struct ViewerRequest: Identifiable {
  public let id = UUID()
  /// Which grid opened it; the thumbnail it grew from is `context/entry id`.
  public let context: String
  public let entries: GridSource
  public let startIndex: Int
  public var showDetails = false
  /// A clip's tile or a moment opens playing; a clip reached by swiping waits for Play.
  public var playClip = false

  public init(context: String, entries: GridSource, startIndex: Int, showDetails: Bool = false,
              playClip: Bool = false) {
    self.context = context
    self.entries = entries
    self.startIndex = startIndex
    self.showDetails = showDetails
    self.playClip = playClip
  }
}

/// A card in the library's top slot.
public struct LibraryNotice: Identifiable, Equatable {
  public enum Action: Equatable { case scan, retry }
  public let id: String
  public let message: String
  public let canDismiss: Bool
  public let action: Action?
}

@MainActor
@Observable
public final class AppModel {
  /// Where the Connect screen picks up: comparing with the desktop, or past that answer.
  /// In a release there is only the one start: the person compares the code themselves.
  public enum ConnectStart: Equatable {
    case checkMatch
    #if DEBUG
    case matched(LibraryIdentity)
    case notMatched
    #endif
  }
  public enum Stage: Equatable { case welcome, connect(code: String, start: ConnectStart = .checkMatch), firstCopy, main }
  public enum Tab: Hashable { case library, search, people, connection }

  public let services: Services
  public let scanner: ScanSurfaceProviding

  public private(set) var stage: Stage
  /// Counts scans, so scanning the same code again starts over.
  public private(set) var scanCount = 0
  public var tab: Tab = .library
  public var isScanning = false
  public var viewer: ViewerRequest?
  /// Where the viewer is, so the grid under it can follow and the photo closes onto its tile.
  public var viewerFocus: String?
  public private(set) var connection: ConnectionSnapshot
  public private(set) var people: [Person] = []
  /// Reading the people failed. The last good list stays; only a real empty answer clears it.
  public private(set) var peopleFailed = false
  /// A new catalogue arrived while a scan was on screen.
  private var reloadNeeded = false
  #if MV_BACKUP
  /// Long work still running, shown above the tab bar.
  public private(set) var transfer: TransferActivity?
  #endif
  public let library: LibraryModel
  public let search: SearchModel

  private var dismissedNotices: Set<String> = []

  public init(services: Services, scanner: ScanSurfaceProviding) {
    self.services = services
    self.scanner = scanner
    let now = services.connection.current
    Self.noteFirstEpoch(of: now.catalogue)
    connection = now
    // A library paired but never copied (the app was killed mid-copy) goes back to the copy.
    stage = !now.hasLibrary ? .welcome : now.catalogue == nil ? .firstCopy : .main
    library = LibraryModel(store: services.library)
    search = SearchModel(service: services.search)
    // §8.4: the catalogue opens before SwiftUI finishes attaching.
    if stage == .main { Task { await loadLibrary() } }
  }

  #if DEBUG
  /// For the fixtures' state gallery only; the app itself moves through setup step by step.
  public func setStageForChecking(_ new: Stage) { stage = new }
  #endif

  /// Runs for the app's life.
  public func run() async {
    await withTaskGroup(of: Void.self) { group in
      group.addTask { await self.followConnection() }
      #if MV_BACKUP
      group.addTask { await self.followTransfers() }
      #endif
    }
  }

  #if MV_BACKUP
  private func followTransfers() async {
    for await activity in services.transfers.activity() { transfer = activity }
  }
  #endif

  private func followConnection() async {
    for await snapshot in services.connection.updates() {
      if snapshot.synology != connection.synology { dismissedNotices.removeAll() }
      let newCatalogue = snapshot.catalogue != nil && snapshot.catalogue != connection.catalogue
      Self.noteFirstEpoch(of: snapshot.catalogue)
      connection = snapshot
      guard stage == .main else {
        if newCatalogue { reloadNeeded = true }
        continue
      }
      if snapshot.hasLibrary, snapshot.catalogue == nil {
        stage = .firstCopy
      } else if newCatalogue {
        reload()
      }
    }
  }

  /// Reloads behind the grid: what is on screen stays until the new list is ready.
  private func reload() {
    Task {
      await loadLibrary()
      search.rerun()
    }
  }

  private func loadLibrary() async {
    async let lib: Void = library.load()
    async let found: Void = loadPeople()
    _ = await (lib, found)
  }

  private func loadPeople() async {
    do {
      people = try await services.library.people()
      peopleFailed = false
    } catch {
      guard !Task.isCancelled, !(error is CancellationError) else { return }
      peopleFailed = true
    }
  }

  func retryPeople() {
    Task { await loadPeople() }
  }

  // MARK: Setup

  public func scanned(_ code: String) {
    isScanning = false
    scanCount += 1
    stage = .connect(code: code)
  }

  func connected() {
    resetLibraryState()
    stage = .firstCopy
  }

  /// Nothing from a previous library carries over.
  private func resetLibraryState() {
    library.reset()
    search.reset()
    people = []
    peopleFailed = false
    reloadNeeded = false
    dismissedNotices = []
    viewer = nil
    viewerFocus = nil
  }

  /// Leaves a scan started from a library that is already paired.
  func returnToLibrary() {
    stage = .main
    if reloadNeeded {
      reloadNeeded = false
      reload()
    }
  }

  func copied() {
    stage = .main
    Task {
      await loadLibrary()
      await search.prepare()
    }
  }

  func forgetLibrary() {
    Task {
      await services.connection.forget()
      UserDefaults.standard.removeObject(forKey: Self.epochKey)
      resetLibraryState()
      tab = .library
      connection = services.connection.current
      stage = .welcome
    }
  }

  // MARK: Viewer

  public func open(_ request: ViewerRequest, reduceMotion: Bool) {
    var t = Transaction()
    t.disablesAnimations = reduceMotion
    withTransaction(t) {
      viewerFocus = nil
      viewer = request
    }
  }

  func closeViewer(reduceMotion: Bool) {
    var t = Transaction()
    t.disablesAnimations = reduceMotion
    withTransaction(t) { viewer = nil }
  }

  // MARK: What the library's top slot says

  /// One card at a time, most serious first. "Connected" is never a card.
  public var libraryNotice: LibraryNotice? {
    let first: LibraryNotice?
    switch connection.synology {
    case .deviceRevoked:
      first = LibraryNotice(id: "revoked", message: FailureCopy.deviceRevoked, canDismiss: false, action: .scan)
    case .publisherChanged:
      first = LibraryNotice(id: "publisher", message: FailureCopy.publisherChanged, canDismiss: false, action: .scan)
    case .serviceTooOld:
      first = LibraryNotice(id: "service", message: FailureCopy.serviceTooOld, canDismiss: true, action: nil)
    case .unreachable:
      first = LibraryNotice(id: "unreachable", message: FailureCopy.synologyUnreachable, canDismiss: true, action: nil)
    case .reachableOnlyOnItsOwnNetwork:
      first = LibraryNotice(id: "own-network", message: FailureCopy.ownNetworkOnly, canDismiss: true, action: nil)
    case .reachable:
      first = nil
    }
    // The stale-catalogue and hand-over cards wait their turn behind anything more serious.
    return [first, handoverNotice, staleNotice].compactMap { $0 }.first { !dismissedNotices.contains($0.id) }
  }

  public func dismiss(_ notice: LibraryNotice) {
    dismissedNotices.insert(notice.id)
    if notice.id.hasPrefix("handover"), let epoch = connection.catalogue?.epoch { Self.epochSeen = epoch }
    if notice.id.hasPrefix("stale") { Self.staleSeen = notice.id }
  }

  private var staleNotice: LibraryNotice? {
    guard let catalogue = connection.catalogue else { return nil }
    let days = Calendar.current.dateComponents([.day], from: catalogue.updatedAt, to: .now).day ?? 0
    guard days > 30 else { return nil }
    // Said once for each catalogue.
    let id = "stale-\(Int(catalogue.updatedAt.timeIntervalSince1970))"
    guard Self.staleSeen != id else { return nil }
    return LibraryNotice(id: id, message: FailureCopy.catalogueStale(days: days), canDismiss: true, action: nil)
  }

  /// Said once when a new computer took the library over with the old one's signature (§5, rule 5).
  /// The first catalogue a phone sees sets the mark silently.
  /// Epoch 0 is a catalogue an earlier build kept without its epoch: unknown, never "handed over".
  private var handoverNotice: LibraryNotice? {
    guard let epoch = connection.catalogue?.epoch, epoch > 0, let seen = Self.epochSeen, seen > 0, epoch > seen else {
      return nil
    }
    return LibraryNotice(id: "handover-\(epoch)", message: FailureCopy.libraryHandedOver, canDismiss: true, action: nil)
  }

  /// The first known epoch is taken as the one the person paired with, silently. Not from the
  /// notice's getter, which runs while a view is drawn.
  private static func noteFirstEpoch(of info: CatalogueInfo?) {
    guard let epoch = info?.epoch, epoch > 0, (epochSeen ?? 0) == 0 else { return }
    epochSeen = epoch
  }

  private static let epochKey = "catalogueEpochSeen"
  private static var epochSeen: UInt64? {
    get { (UserDefaults.standard.object(forKey: epochKey) as? NSNumber)?.uint64Value }
    set { UserDefaults.standard.set(newValue.map { NSNumber(value: $0) }, forKey: epochKey) }
  }

  private static let staleKey = "catalogueStaleNoticeSeen"
  private static var staleSeen: String? {
    get { UserDefaults.standard.string(forKey: staleKey) }
    set { UserDefaults.standard.set(newValue, forKey: staleKey) }
  }

  /// The chip names where search and originals stand, only when that is not the usual, and
  /// only once the card that said it is gone.
  public var routeChip: String? {
    guard libraryNotice == nil else { return nil }
    switch connection.synology {
    case .unreachable: return connection.macNearby ? "From your Mac" : "On this phone"
    case .reachableOnlyOnItsOwnNetwork: return "On this phone"
    default: return nil
    }
  }
}

@MainActor
@Observable
public final class LibraryModel {
  /// The folders from the top of the library down to the one on screen; empty is All media.
  public private(set) var trail: [Folder] = []
  /// The folders inside the one on screen.
  public private(set) var subfolders: [Folder] = []
  public private(set) var sections: [GridSection] = []
  public private(set) var entries = GridSource.empty
  public private(set) var isLoading = true
  /// Why the latest load failed. Whatever was on screen stays there.
  public private(set) var failure: String?

  private let store: LibraryStore
  private var loading: Task<Void, Never>?
  /// Where the newest load is going. `trail` follows only once it has arrived.
  private var requested: [Folder] = []

  init(store: LibraryStore) { self.store = store }

  public var scope: LibraryScope { trail.last.map(LibraryScope.folder) ?? .everything }

  public func enter(_ folder: Folder) { show(trail + [folder]) }

  public func goBack(to index: Int) { show(Array(trail.prefix(index + 1))) }

  public func goToAllMedia() { show([]) }

  /// Jumps straight to a place in the tree.
  public func show(_ path: [Folder]) {
    requested = path
    Task { await load() }
  }

  func reset() {
    loading?.cancel()
    requested = []
    trail = []
    subfolders = []
    sections = []
    entries = .empty
    failure = nil
    isLoading = true
  }

  /// Only the newest load counts; switching folders drops the one before.
  public func load() async {
    loading?.cancel()
    let task = Task { await perform() }
    loading = task
    await task.value
  }

  private func perform() async {
    isLoading = sections.isEmpty
    let path = requested
    let scope = path.last.map(LibraryScope.folder) ?? .everything
    let parent = path.last
    do {
      async let found = store.list(in: scope)
      async let inside = parent?.hasSubfolders == false ? [] : store.folders(under: parent)
      let list = try await found
      let folders = try await inside
      let built = await Task.detached(priority: .userInitiated) { Self.grid(of: list) }.value
      guard !Task.isCancelled, path == requested else { return }
      trail = path
      subfolders = folders
      sections = built.sections
      entries = built.entries
      failure = nil
    } catch {
      guard !Task.isCancelled, !(error is CancellationError) else { return }
      failure = FailureCopy.libraryFailed
    }
    isLoading = false
  }

  nonisolated static func grid(of list: any MediaList) -> (sections: [GridSection], entries: GridSource) {
    let sections = list.months.map {
      GridSection(id: String($0.key), title: $0.date.formatted(.dateTime.month(.wide).year()), range: $0.range)
    }
    return (sections, GridSource(list: list))
  }
}

@MainActor
@Observable
public final class SearchModel {
  public enum Phase: Equatable { case idle, searching, results, nothing, failed }

  public private(set) var text = ""
  public private(set) var filters = SearchFilters()
  public private(set) var options = SearchFilterOptions.none
  public private(set) var phase: Phase = .idle
  public private(set) var entries = GridSource.empty
  public private(set) var recents: [String] = []
  public private(set) var capability: SearchCapability = .ready
  /// True only while someone is actually waiting for the model.
  public private(set) var isWarmingUp = false
  /// The last search failed; earlier results, if any, stay on screen.
  public private(set) var didFail = false

  private let service: SearchService
  private var running: Task<Void, Never>?

  init(service: SearchService) { self.service = service }

  func prepare() async {
    capability = await service.capability()
    recents = await service.recents()
    options = await service.filterOptions()
  }

  func reset() {
    running?.cancel()
    text = ""
    filters = SearchFilters()
    options = .none
    entries = .empty
    recents = []
    phase = .idle
    isWarmingUp = false
    didFail = false
  }

  public func setText(_ new: String) {
    guard new != text else { return }
    text = new
    run()
  }

  public func setFilters(_ new: SearchFilters) {
    guard new != filters else { return }
    filters = new
    run()
  }

  func clearFilters() { setFilters(SearchFilters()) }

  /// Runs the search on screen again, after a new catalogue or a failure.
  func rerun() {
    guard phase != .idle else { return }
    run()
  }

  private func run() {
    running?.cancel()
    isWarmingUp = false
    didFail = false
    let query = text.trimmingCharacters(in: .whitespaces)
    let filters = filters
    guard !query.isEmpty || !filters.isEmpty else {
      entries = .empty
      phase = .idle
      return
    }
    // Earlier results stay put while the new ones are found: typing never blanks the grid.
    phase = .searching
    running = Task {
      try? await Task.sleep(for: .milliseconds(90))
      guard !Task.isCancelled else { return }
      let warming = Task {
        try? await Task.sleep(for: .milliseconds(800))
        guard !Task.isCancelled, await service.isWarmingUp(), !Task.isCancelled else { return }
        isWarmingUp = true
      }
      defer {
        warming.cancel()
        if !Task.isCancelled { isWarmingUp = false }
      }
      do {
        let hits = try await service.search(query, filters: filters)
        guard !Task.isCancelled else { return }
        let built = await Task.detached(priority: .userInitiated) {
          GridSource(entries: hits.enumerated().map { GridEntry(item: $1.item, moment: $1.moment, position: $0) })
        }.value
        guard !Task.isCancelled else { return }
        entries = built
        phase = hits.isEmpty ? .nothing : .results
      } catch {
        guard !Task.isCancelled, !(error is CancellationError) else { return }
        didFail = true
        phase = entries.isEmpty ? .failed : .results
      }
    }
  }

  func submit() {
    let query = text.trimmingCharacters(in: .whitespaces)
    guard !query.isEmpty else { return }
    Task {
      await service.remember(query)
      recents = await service.recents()
    }
  }

  func forget(_ query: String) {
    recents.removeAll { $0 == query }
    Task { await service.forget(query) }
  }

  func forgetAll() {
    recents = []
    Task { await service.forgetAll() }
  }
}

extension EnvironmentValues {
  @Entry var viewerNamespace: Namespace.ID? = nil
}
