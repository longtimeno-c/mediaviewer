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
  /// File search: why a tag match matched ("Canon EOS R5"), under its tile.
  public let caption: String?
  public let id: String

  public init(item: MediaItem, moment: Moment? = nil, position: Int = 0, caption: String? = nil) {
    self.item = item
    self.moment = moment
    self.position = position
    self.caption = caption
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
  /// Each position's identity, when the list is small enough to hold them (a search's answers): a
  /// grid keys its tiles by these, so an item in the next answer keeps its tile and its picture
  /// instead of another item taking them over. Unique even when an item comes twice. nil for a
  /// library, whose tiles are keyed by position and never change underneath.
  public let ids: [String]?

  public static let empty = GridSource(count: 0, ids: []) { _ in fatalError("empty list") }

  private init(count: Int, ids: [String]?, make: @escaping @Sendable (Int) -> GridEntry) {
    self.count = count
    self.ids = ids
    self.make = make
  }

  public init(list: any MediaList) {
    self.init(count: list.count, ids: nil) { GridEntry(item: list.item(at: $0), position: $0) }
  }

  public init(entries: [GridEntry]) {
    var seen: [String: Int] = [:]
    seen.reserveCapacity(entries.count)
    let ids = entries.map { entry -> String in
      let n = seen[entry.id, default: 0]
      seen[entry.id] = n + 1
      return n == 0 ? entry.id : "\(entry.id)#\(n)"
    }
    self.init(count: entries.count, ids: ids) { entries[$0] }
  }

  public subscript(index: Int) -> GridEntry { make(index) }
  public var indices: Range<Int> { 0..<count }
  public var isEmpty: Bool { count == 0 }

  /// The same list with one entry swapped, such as a clip opening at a moment.
  public func replacing(_ index: Int, with entry: GridEntry) -> GridSource {
    let make = make
    return GridSource(count: count, ids: ids) { $0 == index ? entry : make($0) }
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
  /// Long work still running, shown above the tab bar.
  public private(set) var transfer: TransferActivity?
  /// Phone backup's state, for the Backup screen and the Settings row.
  public private(set) var backup = BackupSnapshot()
  /// The Backup screen is open in Settings.
  public var showsBackup = false
  /// The sample library's VPN demo (routes nothing), forgotten with the sample.
  public let demoVPN: DemoVPN
  /// The VPN the screens show and switch: the demo with the sample library, else this build's.
  public var vpnService: VPNService { Self.vpnService(isSample: connection.isSample, demo: demoVPN, real: services.vpn) }
  /// `vpnService`'s state, for the VPN screen and the Settings row.
  public private(set) var vpn: VPNSnapshot
  /// The VPN screen is open in Settings.
  public var showsVPN = false
  @ObservationIgnored private var vpnFollow: Task<Void, Never>?
  /// Which VPN `vpnFollow` follows: the demo (true) or this build's (false).
  @ObservationIgnored private var vpnFollowsSample: Bool?
  /// The follow whose snapshots `vpn` takes; an older one's are dropped.
  @ObservationIgnored private var vpnFollowID: UUID?
  /// The `run()` that owns the follow: a later run takes it over, and only the owner stops it.
  @ObservationIgnored private var vpnRun: UUID?
  /// The library was the sample when last seen, so leaving it forgets the demo.
  @ObservationIgnored private var wasSample: Bool
  public let library: LibraryModel
  public let search: SearchModel

  private var dismissedNotices: Set<String> = []

  public init(services: Services, scanner: ScanSurfaceProviding, demoVPN: DemoVPN = DemoVPN()) {
    self.services = services
    self.scanner = scanner
    self.demoVPN = demoVPN
    let now = services.connection.current
    Self.noteFirstEpoch(of: now.catalogue)
    connection = now
    vpn = Self.vpnService(isSample: now.isSample, demo: demoVPN, real: services.vpn).current
    wasSample = now.isSample
    // A library paired but never copied (the app was killed mid-copy) goes back to the copy; one
    // with no catalogue on its NAS yet opens as its folders (file mode), and one whose access
    // ended opens on the ended card.
    stage = Self.stage(for: now)
    library = LibraryModel(store: services.library, changes: services.changes)
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
    let run = UUID()
    vpnRun = run
    vpnFollowsSample = nil
    followVPN()
    defer {
      if vpnRun == run {
        vpnRun = nil
        vpnFollow?.cancel()
        vpnFollow = nil
        vpnFollowID = nil
        vpnFollowsSample = nil
      }
    }
    await withTaskGroup(of: Void.self) { group in
      group.addTask { await self.followConnection() }
      group.addTask { await self.followTransfers() }
      group.addTask { await self.followBackup() }
    }
  }

  private func followTransfers() async {
    for await activity in services.backup.activity() { transfer = activity }
  }

  private func followBackup() async {
    for await snapshot in services.backup.updates() { backup = snapshot }
  }

  static func vpnService(isSample: Bool, demo: DemoVPN, real: VPNService) -> VPNService {
    isSample ? demo : real
  }

  /// After `connection` changes: leaving the sample forgets its demo (whichever way it was left:
  /// Forget, or pairing another library), and the follow moves to the VPN the library now uses.
  private func connectionChangedForVPN() {
    if wasSample, !connection.isSample {
      showsVPN = false
      let demo = demoVPN
      Task { await demo.remove() }
    }
    wasSample = connection.isSample
    if vpnRun != nil { followVPN() }
  }

  /// Follows `vpnService`, starting again when the library becomes or stops being the sample.
  private func followVPN() {
    let sample = connection.isSample
    guard vpnFollowsSample != sample else { return }
    vpnFollow?.cancel()
    vpnFollowsSample = sample
    let service = vpnService
    let id = UUID()
    vpnFollowID = id
    vpn = service.current
    vpnFollow = Task { [weak self] in
      for await snapshot in service.updates() {
        guard !Task.isCancelled, let self, self.vpnFollowID == id else { return }
        self.vpn = snapshot
      }
    }
  }

  /// Where launch picks up. A library paired but never copied (the app was killed mid-copy) goes
  /// back to the copy, unless its access has ended: then the library, empty, with the ended card
  /// (§8.2), never a copy the NAS would refuse.
  static func stage(for s: ConnectionSnapshot) -> Stage {
    if !s.hasLibrary { return .welcome }
    if s.catalogue == nil, !s.fileMode, s.synology != .accessEnded { return .firstCopy }
    return .main
  }

  private func followConnection() async {
    for await snapshot in services.connection.updates() {
      if snapshot.synology != connection.synology { dismissedNotices.removeAll() }
      let newCatalogue = snapshot.catalogue != nil && snapshot.catalogue != connection.catalogue
      // Folders and a catalogue's library are different trees: a switch starts again at the top.
      let modeChanged = snapshot.hasLibrary && connection.hasLibrary && snapshot.fileMode != connection.fileMode
      Self.noteFirstEpoch(of: snapshot.catalogue)
      connection = snapshot
      connectionChangedForVPN()
      // The access ended during a copy: the library, with the card that says so.
      if stage == .firstCopy, snapshot.hasLibrary, snapshot.synology == .accessEnded {
        stage = .main
        Task { await loadLibrary() }  // the grid, empty, under the ended card: never a skeleton left waiting
        continue
      }
      guard stage == .main else {
        if newCatalogue || modeChanged { reloadNeeded = true }
        continue
      }
      if snapshot.hasLibrary, snapshot.catalogue == nil, !snapshot.fileMode, snapshot.synology != .accessEnded {
        stage = .firstCopy  // an extension, or a copy never made: copied again
      } else if modeChanged {
        viewer = nil
        // The same library, its folders seen another way: files being sent to its server go on.
        library.reset(keepingUploads: true)
        reload()
      } else if newCatalogue {
        reload()
      }
    }
  }

  /// Reloads behind the grid: what is on screen stays until the new list is ready.
  private func reload() {
    library.forgetPreviews()  // a new catalogue: its items are other ids
    Task {
      await loadLibrary()
      await search.prepare()  // file search and the catalogue's answer differently
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

  /// What a pair link opened from outside the app (another app, a web page) may do (§11.1).
  public enum PairLinkAction: Equatable { case open, confirm, ignore }

  /// Any app can open a mediaviewer://pair link, so one never interrupts pairing or a first copy,
  /// and never replaces a library in use without asking.
  public static func pairLinkAction(stage: Stage, isScanning: Bool, hasLibrary: Bool,
                                    connectEnded: Bool = false) -> PairLinkAction {
    if isScanning { return .open }
    switch stage {
    case .welcome: return .open
    // A Connect screen that ended in a failure (expired, unreachable, not a match) takes a fresh
    // link; one still reading or pairing does not.
    case .connect: return connectEnded ? .open : .ignore
    case .firstCopy: return .ignore
    case .main: return hasLibrary ? .confirm : .open
    }
  }

  /// The Connect screen has stopped on a failure, so nothing is in progress there.
  public var connectEnded = false

  /// A pair link waiting for the person to say it may replace the library on this phone.
  public var pendingPairLink: String?

  public func openedPairLink(_ link: String) {
    switch Self.pairLinkAction(stage: stage, isScanning: isScanning, hasLibrary: connection.hasLibrary,
                               connectEnded: connectEnded) {
    case .open: scanned(link)
    case .confirm:
      viewer = nil  // the question is asked from under the viewer, which would hide it
      pendingPairLink = link
    case .ignore: break
    }
  }

  public func confirmPendingPairLink() {
    guard let link = pendingPairLink else { return }
    pendingPairLink = nil
    scanned(link)
  }

  public func scanned(_ code: String) {
    connectEnded = false
    isScanning = false
    scanCount += 1
    stage = .connect(code: code)
  }

  func connected() {
    resetLibraryState()
    // A new pairing, perhaps of another library: its first catalogue sets the epoch afresh.
    UserDefaults.standard.removeObject(forKey: Self.epochKey)
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

  /// The library the sample library would replace on this phone, by name; nil when nothing
  /// would be lost (no library, or the sample itself). Opening the sample asks first (§11.1).
  public static func sampleWouldReplace(_ connection: ConnectionSnapshot) -> String? {
    guard connection.hasLibrary, !connection.isSample else { return nil }
    return connection.libraryName.isEmpty ? "your library" : connection.libraryName
  }

  /// Leaves the Connect screen without pairing: back to the library, or to Welcome when there is none.
  func leaveConnect() {
    if connection.hasLibrary { returnToLibrary() } else { stage = .welcome }
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
      await demoVPN.remove()
      showsVPN = false
      UserDefaults.standard.removeObject(forKey: Self.epochKey)
      resetLibraryState()
      tab = .library
      connection = services.connection.current
      connectionChangedForVPN()
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
    case .accessEnded:
      first = LibraryNotice(id: "access-ended",
                            message: FailureCopy.accessEnded(library: connection.libraryName, at: connection.accessUntil),
                            canDismiss: false, action: .retry)
    case .publisherChanged where connection.isSample:
      // Not another computer: what arrived from GitHub was not signed by the sample's key.
      first = LibraryNotice(id: "publisher", message: FailureCopy.sampleRejected, canDismiss: true, action: .retry)
    case .publisherChanged:
      first = LibraryNotice(id: "publisher", message: FailureCopy.publisherChanged, canDismiss: false, action: .scan)
    case .serviceTooOld:
      first = LibraryNotice(id: "service", message: FailureCopy.serviceTooOld, canDismiss: true, action: nil)
    case .unreachable:
      first = LibraryNotice(id: "unreachable",
                            message: connection.isSample ? FailureCopy.sampleUnreachable : FailureCopy.synologyUnreachable,
                            canDismiss: true, action: nil)
    case .reachableOnlyOnItsOwnNetwork:
      first = LibraryNotice(id: "own-network", message: FailureCopy.ownNetworkOnly, canDismiss: true, action: nil)
    case .reachable:
      first = nil
    }
    // The stale-catalogue and hand-over cards wait their turn behind anything more serious.
    return [first, storageNotice, handoverNotice, staleNotice].compactMap { $0 }.first { !dismissedNotices.contains($0.id) }
  }

  public func dismiss(_ notice: LibraryNotice) {
    dismissedNotices.insert(notice.id)
    if notice.id.hasPrefix("handover"), let epoch = connection.catalogue?.epoch { Self.epochSeen = epoch }
    if notice.id.hasPrefix("stale") { Self.staleSeen = notice.id }
  }

  /// The server's combined drive is not healthy (docs/plans/self-host-drives.md §4): "Your server's
  /// mirror lost a drive." Calm and dismissable; said again when the state changes.
  private var storageNotice: LibraryNotice? { Self.storageNotice(for: connection) }

  nonisolated static func storageNotice(for c: ConnectionSnapshot) -> LibraryNotice? {
    guard c.hasLibrary, c.serverStorage > 0, !c.serverStorageWords.isEmpty else { return nil }
    return LibraryNotice(id: "storage-\(c.serverStorage)", message: c.serverStorageWords, canDismiss: true, action: nil)
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
    // Said on every visit, not once: nobody should take the sample for their own library (§11.1).
    if connection.isSample { return "Sample library" }
    switch connection.synology {
    case .unreachable: return connection.macNearby ? "From your Mac" : "On this phone"
    case .reachableOnlyOnItsOwnNetwork: return "On this phone"
    default: return nil
    }
  }
}

/// What one folder's page lists. Kept per folder, so a folder opened before is laid out at once.
struct FolderListing {
  var subfolders: [Folder]
  var others: [OtherFile]
  var sections: [GridSection]
  var entries: GridSource
  /// Which items, in order (made off the main thread with the grid): a listing asked for again that
  /// holds the same items redraws no tile.
  var signature: Int = 0
  /// When it was had, for a folder listed live from the server.
  var listedAt = Date()
}

/// One page of the library's navigation stack: All media at the bottom, a page per folder above
/// it. Each keeps what it shows while it is on the stack, so going back finds it as it was, scroll
/// position and all.
@MainActor
@Observable
public final class FolderPage: Identifiable, Hashable {
  /// The folders from the top of the library down to this one; empty is All media.
  public let path: [Folder]
  /// The folders inside this one.
  public internal(set) var subfolders: [Folder] = []
  /// The files in this one that are not photos or clips (file mode only), by name.
  public internal(set) var others: [OtherFile] = []
  public internal(set) var sections: [GridSection] = []
  public internal(set) var entries = GridSource.empty
  /// Nothing listed yet.
  public internal(set) var isLoading = true
  /// Why the latest load failed. Whatever was on screen stays there.
  public internal(set) var failure: String?
  @ObservationIgnored var loading: Task<Void, Never>?
  /// This phone may change files here (FolderChanges): asked once the page has listed.
  public internal(set) var changeable = false
  /// Select mode: taps choose instead of opening, and the bottom bar acts on what is chosen.
  public internal(set) var selecting = false
  /// The ids (FolderEntry.id) chosen.
  public internal(set) var selected: Set<String> = []
  /// The tile whose hold started Select: the tap its lift brings chooses nothing more.
  @ObservationIgnored var held: String?
  @ObservationIgnored var entriesSignature = 0
  /// Why the last change here was refused, on a card at the top of the page.
  public internal(set) var changeFailure: String?
  /// A change is being made here: the bar's commands wait for it.
  public internal(set) var changing = false

  init(path: [Folder]) { self.path = path }

  public nonisolated static func == (a: FolderPage, b: FolderPage) -> Bool { a === b }
  public nonisolated func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }

  public var folder: Folder? { path.last }
  public var scope: LibraryScope { path.last.map(LibraryScope.folder) ?? .everything }
  public var title: String { path.last?.name ?? "All media" }
  /// The grid's name: a viewer closes onto a tile in the page that opened it, never one under it.
  public var context: String { path.last.map { "library:\($0.id)" } ?? "library" }

  /// Only what changed is replaced, so a listing asked for again redraws nothing that stayed.
  func apply(_ listing: FolderListing) {
    if subfolders != listing.subfolders { subfolders = listing.subfolders }
    if others != listing.others { others = listing.others }
    if sections != listing.sections { sections = listing.sections }
    if entriesSignature != listing.signature || entries.count != listing.entries.count {
      entries = listing.entries
      entriesSignature = listing.signature
    }
    // What is chosen and no longer here (moved or deleted, here or elsewhere) is no longer chosen.
    if !selected.isEmpty {
      var present = Set(subfolders.map(\.id)).union(others.map(\.id))
      for i in entries.indices where selected.contains(entries[i].item.id.raw) { present.insert(entries[i].item.id.raw) }
      selected.formIntersection(present)
    }
    failure = nil
    isLoading = false
  }

  func clear() {
    loading?.cancel()
    subfolders = []
    others = []
    sections = []
    entries = .empty
    failure = nil
    isLoading = true
  }
}

@MainActor
@Observable
public final class LibraryModel {
  /// All media: the bottom of the stack, always there.
  public let root = FolderPage(path: [])
  /// The folders opened above All media, in order: the navigation stack's path. The system's back
  /// (the button, its menu of earlier folders, the edge swipe) shortens it.
  public var stack: [FolderPage] = [] {
    didSet {
      // A page taken off stops loading; a file being fetched for the page left stops too.
      for page in oldValue where !stack.contains(page) { page.loading?.cancel() }
      if oldValue.last != stack.last {
        cancelOpening()
        openFailure = nil
      }
    }
  }
  /// The other file being fetched for the phone's preview. Changes only when one starts or ends,
  /// so a list of files is not redrawn on every progress step.
  public private(set) var openingID: String?
  /// How far that fetch has got: read only by the row being opened.
  public private(set) var openProgress: (done: Int64, total: Int64) = (0, 0)
  @ObservationIgnored private var openToken = 0
  /// The fetched file the preview shows; nil closes it.
  public var previewing: URL?
  /// Why the last other file could not be opened, on its row.
  public private(set) var openFailure: (id: String, message: String)?
  @ObservationIgnored private var openTask: Task<Void, Never>?

  private let store: LibraryStore
  /// The top level's ask for the server's offline folders, after its own folders are on screen.
  private var offlineCheck: Task<Void, Never>?
  /// Folder tiles' pictures by folder id, so a folder gone back to shows its tiles at once. Not
  /// observed: a tile asks for its own and keeps it, until `previewVersion` says its folder changed.
  @ObservationIgnored private var previews: [String: FolderPreview] = [:]
  /// Bumped for a folder whose contents changed, so its tile asks for its cover and counts again.
  public private(set) var previewVersion: [String: Int] = [:]
  /// Whether this phone may change files in a folder, as last asked (FolderChanges.canChange).
  @ObservationIgnored var knownChangeable: [String: Bool] = [:]
  /// Folders being listed ahead of a tap (their tiles are on screen), and how many at once.
  @ObservationIgnored private var prefetching: Set<String> = []
  static let prefetchesAtOnce = 2
  /// A folder listed live from the server, opened again within this long, is shown as it was
  /// listed and not asked again under the transition.
  static let freshFor: TimeInterval = 30
  static let previewCount = 3
  /// What each folder listed last, by its path, so a folder opened again slides in laid out. The
  /// newest few only.
  @ObservationIgnored private var listings: [String: FolderListing] = [:]
  @ObservationIgnored private var listingOrder: [String] = []
  static let listingsKept = 24

  let changes: FolderChanges
  /// Files being sent to the server, in order. They keep going when their folder is left.
  public internal(set) var uploads: [Upload] = []
  @ObservationIgnored var uploading: Task<Void, Never>?

  init(store: LibraryStore, changes: FolderChanges = NoFolderChanges()) {
    self.store = store
    self.changes = changes
  }

  /// The page on screen.
  public var current: FolderPage { stack.last ?? root }
  /// The folders from the top of the library down to the one on screen; empty is All media.
  public var trail: [Folder] { current.path }
  public var scope: LibraryScope { current.scope }

  public func enter(_ folder: Folder) {
    let from = current
    // A folder whose drive is not connected has nothing to show: say so, stay here. The notice's
    // Try again reloads this level, which asks the server again.
    guard !folder.offline else {
      from.failure = FailureCopy.driveNotConnected(folder.name)
      return
    }
    // The tap answers at once: the page slides in with what its folder listed last (listed ahead
    // when its tile was on screen), or with its skeleton at the grid's own size.
    let page = FolderPage(path: from.path + [folder])
    // Its commands from the first frame: a folder inside one this phone may change, or one asked before.
    page.changeable = knownChangeable[folder.id] ?? (from.folder != nil && from.changeable)
    if let known = listings[Self.key(page.path)] {
      page.apply(known)
      stack.append(page)
      // A catalogue's folders change only with a new catalogue, which forgets every listing; a
      // folder listed live from the server is asked again unless it was listed just now.
      if folder.itemCount < 0, Date().timeIntervalSince(known.listedAt) > Self.freshFor {
        load(page)
      } else {
        askChangeable(page)
      }
      return
    }
    load(page)
    stack.append(page)
  }

  /// Lists a folder whose tile is on screen, so a tap on it finds it laid out. A few at a time, at
  /// a low priority; nothing for one already listed or being listed.
  @discardableResult
  public func prefetch(_ folder: Folder, in parent: FolderPage) -> Task<Void, Never>? {
    let path = parent.path + [folder]
    let key = Self.key(path)
    guard !folder.offline, listings[key] == nil, !prefetching.contains(key),
          prefetching.count < Self.prefetchesAtOnce else { return nil }
    prefetching.insert(key)
    let store = store
    return Task(priority: .utility) {
      defer { prefetching.remove(key) }
      guard let listing = try? await Self.listing(store, path: path, keepOffline: []) else { return }
      if listings[key] == nil { remember(listing, for: path) }
    }
  }

  /// Jumps straight to a place in the tree.
  public func show(_ path: [Folder]) {
    stack = path.indices.map { end in
      let page = FolderPage(path: Array(path.prefix(end + 1)))
      if let known = listings[Self.key(page.path)] { page.apply(known) }
      load(page)
      return page
    }
  }

  /// A folder tile's preview, known or asked for (LibraryStore.preview). Cancellable: a tile
  /// scrolled away stops asking.
  public func preview(of folder: Folder) async -> FolderPreview {
    if let known = previews[folder.id] { return known }
    let got = await store.preview(of: folder, count: Self.previewCount)
    // Nothing at all (the server could not be asked, a tile cancelled) is asked again next time.
    if !Task.isCancelled, got != .empty { previews[folder.id] = got }
    return got
  }

  public func knownPreview(of folder: Folder) -> FolderPreview? { previews[folder.id] }

  /// Fetches an other file and shows it in the phone's preview; tapping the one being fetched stops it.
  public func open(_ file: OtherFile) {
    if openingID == file.id {
      cancelOpening()
      return
    }
    openTask?.cancel()
    openToken += 1
    let token = openToken
    openFailure = nil
    openingID = file.id
    openProgress = (0, file.bytes)
    openTask = Task { [store] in
      do {
        let url = try await store.download(file) { done, total in
          Task { @MainActor [weak self] in
            // A step from a fetch already stopped never moves this one's bar.
            guard let self, self.openToken == token, done >= self.openProgress.done else { return }
            self.openProgress = (done, total)
          }
        }
        guard !Task.isCancelled, openToken == token else { return }
        openingID = nil
        previewing = url
      } catch {
        guard !Task.isCancelled, openToken == token else { return }
        openingID = nil
        openFailure = (file.id, FailureCopy.fileNotOpened)
      }
    }
  }

  public func cancelOpening() {
    openTask?.cancel()
    openTask = nil
    openToken += 1
    openingID = nil
  }

  /// A new catalogue: its items are other ids, and its folders may hold other things.
  func forgetPreviews() {
    previews = [:]
    listings = [:]
    listingOrder = []
    knownChangeable = [:]
  }

  /// `keepingUploads`: the same library seen another way (its folders, then its catalogue): files
  /// being sent to its server go on. Another library: none of them goes to it.
  func reset(keepingUploads: Bool = false) {
    forgetPreviews()
    if !keepingUploads {
      uploading?.cancel()
      uploading = nil
      uploads.compactMap(\.file).forEach(Self.discard)
      uploads = []
    }
    offlineCheck?.cancel()
    stack = []
    root.clear()
    cancelOpening()
    openFailure = nil
    previewing = nil
  }

  /// Lists every page on the stack again (a new catalogue): what is on screen stays until each
  /// page's new listing is ready.
  public func load() async {
    let tasks = ([root] + stack).map { load($0) }
    for task in tasks { await task.value }
  }

  /// Lists one page again (its Try again); only its newest load counts.
  /// `animated`: after a change, folders and files coming and going move into place.
  @discardableResult
  func load(_ page: FolderPage, animated: Bool = false) -> Task<Void, Never> {
    page.loading?.cancel()
    let task = Task { await perform(page, animated: animated) }
    page.loading = task
    return task
  }

  /// A folder's listing: its photos and clips (the grid made off the main thread), its folders and
  /// its other files. The top level keeps `keepOffline` (shown until the server answers again).
  static func listing(_ store: LibraryStore, path: [Folder], keepOffline: [Folder]) async throws -> FolderListing {
    let parent = path.last
    let scope = parent.map(LibraryScope.folder) ?? .everything
    async let found = store.list(in: scope)
    async let inside = parent?.hasSubfolders == false ? [] : store.folders(under: parent)
    async let loose = Self.otherFiles(store, in: parent)
    let list = try await found
    let folders = try await inside
    let otherFiles = try await loose
    let built = await Task.detached(priority: .userInitiated) { Self.grid(of: list) }.value
    try Task.checkCancellation()
    let subfolders = path.isEmpty ? Self.merging(keepOffline, into: folders) : folders
    return FolderListing(subfolders: subfolders, others: otherFiles, sections: built.sections, entries: built.entries,
                         signature: built.signature)
  }

  private func perform(_ page: FolderPage, animated: Bool = false) async {
    let path = page.path
    do {
      let listing = try await Self.listing(store, path: path, keepOffline: page.subfolders.filter(\.offline))
      guard !Task.isCancelled else { return }
      let folders = listing.subfolders.filter { !$0.offline }
      remember(listing, for: path)
      if animated {
        withAnimation(Self.changeMotion) { page.apply(listing) }
      } else {
        page.apply(listing)
      }
      if path.isEmpty { addOfflineFolders(over: folders) }
      if let folder = path.last, !folder.offline { askChangeable(page) }
    } catch {
      guard !Task.isCancelled, !(error is CancellationError) else { return }
      page.failure = FailureCopy.libraryFailed
    }
    page.isLoading = false
  }

  private func remember(_ listing: FolderListing, for path: [Folder]) {
    let key = Self.key(path)
    listings[key] = listing
    listingOrder.removeAll { $0 == key }
    listingOrder.append(key)
    while listingOrder.count > Self.listingsKept { listings[listingOrder.removeFirst()] = nil }
  }

  private static func key(_ path: [Folder]) -> String { path.last?.id ?? "" }

  /// Folders changed: the next time each is opened it is listed afresh, never shown from before.
  func forgetListings(where changed: (String) -> Bool) {
    for key in listings.keys where changed(key) { listings[key] = nil }
    listingOrder.removeAll { listings[$0] == nil }
  }

  /// Folders changed: their tiles ask for their cover and counts again.
  func forgetPreviews(where changed: (String) -> Bool) {
    for key in Array(previews.keys) where changed(key) {
      previews[key] = nil
      previewVersion[key, default: 0] += 1
    }
  }

  nonisolated private static func otherFiles(_ store: LibraryStore, in folder: Folder?) async throws -> [OtherFile] {
    guard let folder else { return [] }
    return try await store.otherFiles(in: folder)
  }

  /// The server's folders whose drive is not connected join the top level once they are known: its
  /// own folders never wait for the server to answer.
  private func addOfflineFolders(over own: [Folder]) {
    offlineCheck?.cancel()
    offlineCheck = Task { [store, root] in
      let offline = await store.offlineFolders()
      guard !Task.isCancelled else { return }
      // The answer replaces the offline folders shown before: a drive plugged back in drops out.
      let merged = Self.merging(offline, into: own)
      if merged != root.subfolders { root.subfolders = merged }
    }
  }

  /// `folders` with each offline folder added at the end, by name, unless one of `folders` has its id
  /// (file mode lists offline roots itself) or its name (a catalogue's own folder for that share, which
  /// still browses from what is on the phone: no second chip beside it).
  nonisolated static func merging(_ offline: [Folder], into folders: [Folder]) -> [Folder] {
    let ids = Set(folders.map(\.id))
    let names = Set(folders.map { $0.name.lowercased() })
    let added = offline.filter { !ids.contains($0.id) && !names.contains($0.name.lowercased()) }
      .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    return folders + added
  }

  nonisolated static func grid(of list: any MediaList) -> (sections: [GridSection], entries: GridSource, signature: Int) {
    let sections = list.months.map {
      GridSection(id: String($0.key), title: $0.date.formatted(.dateTime.month(.wide).year()), range: $0.range)
    }
    var hasher = Hasher()
    hasher.combine(list.count)
    for i in 0..<list.count { hasher.combine(list.item(at: i).id.raw) }
    return (sections, GridSource(list: list), hasher.finalize())
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
  /// What the screen shows. The last answer stays (results, nothing found, or the recents before
  /// any) until the next one is ready, so typing never flashes a skeleton or an empty grid
  /// between keys; `.searching` only once a wait has passed `skeletonDelay` with nothing to show.
  public private(set) var shown: Phase = .idle
  public private(set) var entries = GridSource.empty
  /// Counts answers, so the grid animates from one answer to the next and at no other time.
  public private(set) var answer = 0
  /// The words the entries on screen answer: earlier results stay while new ones are found.
  public private(set) var answered = ""
  /// File search (no catalogue yet): what the last answer searched. nil for the catalogue's search.
  public private(set) var fileScope: FileScope?

  public struct FileScope: Equatable {
    /// The folder searched; nil when no folder is open in the library.
    public let folderName: String?
    public let found: Int
    public let outOf: Int
    /// Tags still being read there: the answer grows as they are.
    public let readingTags: SearchAnswer.Progress?
  }
  public private(set) var recents: [String] = []
  public private(set) var capability: SearchCapability = .ready
  /// True only while someone is actually waiting for the model.
  public private(set) var isWarmingUp = false
  /// The last search failed; earlier results, if any, stay on screen.
  public private(set) var didFail = false

  private let service: SearchService
  private var running: Task<Void, Never>?
  /// The running search's skeleton and warming notice. Cancelled with the search itself: a search
  /// blocked in the core (a cold model) may not return for seconds after it is cancelled.
  private var warming: Task<Void, Never>?

  /// How long a first search waits on the recents before the skeleton shows.
  static let skeletonDelay = Duration.milliseconds(800)

  init(service: SearchService) { self.service = service }

  func prepare() async {
    capability = await service.capability()
    recents = await service.recents()
    options = await service.filterOptions()
  }

  func reset() {
    running?.cancel()
    warming?.cancel()
    text = ""
    filters = SearchFilters()
    fileScope = nil
    options = .none
    entries = .empty
    recents = []
    phase = .idle
    shown = .idle
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

  /// File search: the tag progress the last poll saw, and how many polls in a row saw no change.
  private var lastReading: SearchAnswer.Progress?
  private var unchangedPolls = 0
  /// Polls with no progress before file search stops asking (a pass that stalled, or a slow file).
  static let maxUnchangedPolls = 6

  private func run(polling: Bool = false) {
    if !polling {
      lastReading = nil
      unchangedPolls = 0
    }
    running?.cancel()
    warming?.cancel()
    isWarmingUp = false
    didFail = false
    let query = text.trimmingCharacters(in: .whitespaces)
    let filters = filters
    guard !query.isEmpty || !filters.isEmpty else {
      entries = .empty
      phase = .idle
      shown = .idle
      answer += 1
      return
    }
    // Earlier results stay put while the new ones are found: typing never blanks the grid.
    phase = .searching
    // A failure with nothing to show: its card goes with `didFail`, so the skeleton takes the
    // page at once rather than a blank one until the delay.
    if shown == .failed { shown = .searching }
    running = Task {
      try? await Task.sleep(for: .milliseconds(90))
      guard !Task.isCancelled else { return }
      let warming = Task {
        try? await Task.sleep(for: Self.skeletonDelay - .milliseconds(90))
        guard !Task.isCancelled else { return }
        // Nothing answered yet, from the recents: the content's own empty geometry, not a blank.
        if shown == .idle { shown = .searching }
        guard await service.isWarmingUp(), !Task.isCancelled else { return }
        isWarmingUp = true
      }
      self.warming = warming
      defer {
        warming.cancel()
        if !Task.isCancelled { isWarmingUp = false }
      }
      do {
        let reply = try await service.answer(query, filters: filters)
        let hits = reply.hits
        guard !Task.isCancelled else { return }
        let built = await Task.detached(priority: .userInitiated) {
          GridSource(entries: hits.enumerated().map { GridEntry(item: $1.item, moment: $1.moment, position: $0, caption: $1.why) })
        }.value
        guard !Task.isCancelled else { return }
        entries = built
        answered = query
        fileScope = reply.outOf.map {
          FileScope(folderName: reply.folderName, found: hits.count, outOf: $0, readingTags: reply.readingTags)
        }
        phase = hits.isEmpty ? .nothing : .results
        shown = phase
        answer += 1
        // File search: the folder's tags are still being read, so ask again as they arrive, as the
        // desktop's bar does while it says "Still reading tags".
        // Only while the count moves: a pass that has stopped moving is not asked about for ever.
        if let reading = reply.readingTags {
          if reading == lastReading {
            unchangedPolls += 1
          } else {
            lastReading = reading
            unchangedPolls = 0
          }
          if unchangedPolls < Self.maxUnchangedPolls {
            warming.cancel()
            try? await Task.sleep(for: .milliseconds(700))
            if !Task.isCancelled { run(polling: true) }
          }
        }
      } catch {
        guard !Task.isCancelled, !(error is CancellationError) else { return }
        didFail = true
        phase = entries.isEmpty ? .failed : .results
        shown = phase
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
