// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Every state in docs/plans/mobile.md §11, by name. Reach one three ways: the bug button on the
// screen (the gallery), a launch argument (-fixtureState library.stale), or the pretend scan.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import MobileKit
import MobileUI
import SwiftUI
import UIKit

public enum FixtureState: String, CaseIterable, Identifiable {
  case welcome
  case scan
  case connectConfirm = "connect.confirm"
  case connectConfirmGuest = "connect.confirmGuest"
  case connectNotMatched = "connect.notMatched"
  case connectIdentifyFails = "connect.identifyFails"
  case connectWorking = "connect.working"
  case connectExpired = "connect.expired"
  case connectNotReadable = "connect.notReadable"
  case connectUnreachable = "connect.unreachable"
  case connectOwnNetwork = "connect.ownNetwork"
  case connectNoMobileData = "connect.noMobileData"
  case connectServiceTooOld = "connect.serviceTooOld"
  case firstCopy = "firstCopy.running"
  case firstCopyWiFi = "firstCopy.waitingForWiFi"
  case firstCopyFails = "firstCopy.fails"
  case firstCopyKeepOpen = "firstCopy.keepOpen"
  case firstCopyNoSpace = "firstCopy.noSpace"
  case firstCopyAfterKill = "firstCopy.afterKill"
  case firstCopyResuming = "firstCopy.resuming"
  case library
  case libraryUnreachable = "library.unreachable"
  case libraryChipPhone = "library.chipPhone"
  case libraryChipMac = "library.chipMac"
  case libraryStale = "library.stale"
  case libraryOwnNetwork = "library.ownNetwork"
  case libraryServiceTooOld = "library.serviceTooOld"
  case libraryPublisherChanged = "library.publisherChanged"
  case libraryRevoked = "library.revoked"
  case libraryAccessEnded = "library.accessEnded"
  case libraryHandedOver = "library.handedOver"
  case settingsLight = "settings.light"
  case libraryEmpty = "library.empty"
  case libraryFolder = "library.folder"
  case libraryNewCatalogue = "library.newCatalogue"
  case libraryFolderFails = "library.folderFails"
  case libraryCatalogueDuringScan = "library.catalogueDuringScan"
  case libraryFailed = "library.failed"
  case libraryUnreachableThenStale = "library.unreachableThenStale"
  case libraryFolderEmpty = "library.folderEmpty"
  #if MV_BACKUP
  case libraryBackup = "library.backupRunning"
  case libraryBackupEnds = "library.backupEnds"
  #endif
  case searchRecents = "search.recents"
  case searchTyping = "search.typing"
  case searchResults = "search.results"
  case searchNothing = "search.nothing"
  case searchTower = "search.unsupportedTower"
  case searchFiltered = "search.filtered"
  case searchFailed = "search.failed"
  case searchFilteredNothing = "search.filteredNothing"
  case searchWarming = "search.warmingUp"
  case viewer
  case viewerNoFilmstrip = "viewer.noFilmstrip"
  case viewerRefining = "viewer.refining"
  case viewerRefiningUnknown = "viewer.refiningUnknownSize"
  case viewerSlowNetwork = "viewer.slowNetwork"
  case viewerServiceTooOld = "viewer.serviceTooOld"
  case viewerPublisherChanged = "viewer.publisherChanged"
  case viewerUnreachable = "viewer.unreachable"
  case viewerGone = "viewer.gone"
  case viewerNotThereYet = "viewer.notThereYet"
  case viewerCannotDecode = "viewer.cannotDecode"
  case viewerPhotosDenied = "viewer.photosDenied"
  case clip
  case clipMoment = "clip.moment"
  case clipStops = "clip.stopsPlaying"
  case clipStalls = "clip.stalls"
  case clipTooBig = "clip.tooBig"
  case clipTooBigMoment = "clip.tooBigMoment"
  case clipNotPlayable = "clip.notPlayable"
  case clipGone = "clip.gone"
  case details
  case people
  case peopleNone = "people.none"
  case peopleFails = "people.fails"
  case peopleReloadFails = "people.reloadFails"
  case personFails = "people.personFails"
  case connection
  case connectionFromMac = "connection.fromMac"
  case connectionUnreachable = "connection.unreachable"
  case connectionOwnNetwork = "connection.ownNetwork"
  case connectionServiceTooOld = "connection.serviceTooOld"
  case connectionPublisherChanged = "connection.publisherChanged"

  public var id: String { rawValue }

  fileprivate var group: String { String(rawValue.prefix { $0 != "." }) }
}

private extension ConnectionSnapshot {
  static func with(_ synology: SynologyState, macNearby: Bool = false, source: CatalogueInfo.Source = .synology) -> ConnectionSnapshot {
    let base = FixtureWorld.connected(source: source)
    return ConnectionSnapshot(hasLibrary: true, synology: synology, macNearby: macNearby, catalogue: base.catalogue,
                              cacheBytes: base.cacheBytes)
  }
}

@MainActor
final class FixtureRun {
  let world: FixtureWorld
  let model: AppModel
  let state: FixtureState
  let id = UUID()

  init(state: FixtureState) {
    self.state = state
    let connected = FixtureWorld.connected()
    var snapshot: ConnectionSnapshot = connected
    switch state {
    case .firstCopyAfterKill:
      snapshot = ConnectionSnapshot(hasLibrary: true, synology: connected.synology, macNearby: false, catalogue: nil,
                                    cacheBytes: 0)
    case .libraryUnreachableThenStale:
      let old = FixtureWorld.connected(age: 34.5 * 86_400)
      snapshot = ConnectionSnapshot(hasLibrary: true, synology: .unreachable(FixtureConnection.triedAddresses), macNearby: false,
                                    catalogue: old.catalogue, cacheBytes: old.cacheBytes)
    case .welcome, .scan, .connectIdentifyFails, .connectConfirm, .connectNotMatched, .firstCopyKeepOpen, .firstCopyNoSpace, .firstCopyResuming, .connectWorking, .connectExpired, .connectNotReadable, .connectUnreachable,
         .connectOwnNetwork, .connectNoMobileData, .connectServiceTooOld, .firstCopy, .firstCopyWiFi, .firstCopyFails:
      snapshot = .noLibrary
    case .libraryUnreachable, .libraryChipPhone, .connectionUnreachable, .viewerUnreachable:
      snapshot = .with(.unreachable(FixtureConnection.triedAddresses))
    case .libraryChipMac:
      snapshot = .with(.unreachable(FixtureConnection.triedAddresses), macNearby: true, source: .nearbyMac)
    case .libraryStale:
      snapshot = FixtureWorld.connected(age: 34.5 * 86_400)
    case .libraryOwnNetwork, .connectionOwnNetwork:
      snapshot = .with(.reachableOnlyOnItsOwnNetwork)
    case .libraryServiceTooOld, .connectionServiceTooOld:
      snapshot = .with(.serviceTooOld)
    case .libraryPublisherChanged, .connectionPublisherChanged:
      snapshot = .with(.publisherChanged)
    case .libraryRevoked:
      snapshot = .with(.deviceRevoked)
    case .libraryAccessEnded:
      snapshot = .with(.accessEnded)
      snapshot.accessUntil = Date(timeIntervalSince1970: 1_794_000_000)
    case .libraryHandedOver:
      let base = FixtureWorld.connected()
      let raised = base.catalogue.map { CatalogueInfo(source: $0.source, updatedAt: $0.updatedAt, itemCount: $0.itemCount, epoch: 2) }
      snapshot = ConnectionSnapshot(hasLibrary: true, synology: base.synology, macNearby: false, catalogue: raised,
                                    cacheBytes: base.cacheBytes)
    case .connectionFromMac:
      snapshot = FixtureWorld.connected(source: .nearbyMac)
    default:
      break
    }
    let world = FixtureWorld(snapshot: snapshot)
    switch state {
    case .firstCopyWiFi: world.copyMode = .waitingForWiFi
    case .firstCopyFails: world.copyMode = .fails
    case .firstCopyKeepOpen: world.copyContinuesAfterLeaving = false
    case .firstCopyNoSpace: world.copyMode = .noSpace
    case .connectIdentifyFails: world.identifyFailures = 1
    case .libraryFailed: world.libraryFails = true
    case .libraryNewCatalogue, .libraryCatalogueDuringScan: world.hasPeople = false
    case .peopleFails: world.peopleFailures = 1
    case .personFails: world.personFailures = 1
    case .libraryFolderFails: world.foldersFail = true
    case .searchFailed: world.searchFails = true
    case .clipStops: world.clipMode = .stopsPlaying
    case .clipStalls: world.clipMode = .stalls
    case .firstCopyResuming:
      world.copyMode = .resuming
      world.copyContinuesAfterLeaving = false
    case .viewerRefiningUnknown: world.imageMode = .slowUnknownSize
    case .viewerSlowNetwork: world.imageMode = .slowNetwork
    case .viewerServiceTooOld: world.imageMode = .fails(.serviceTooOld)
    case .viewerPublisherChanged: world.imageMode = .fails(.publisherChanged)
    case .searchWarming: world.searchMode = .modelLoading
    case .viewerRefining: world.imageMode = .slowOriginal
    case .viewerUnreachable: world.imageMode = .fails(.synologyUnreachable)
    case .viewerGone: world.imageMode = .fails(.originalGone)
    case .viewerNotThereYet: world.imageMode = .fails(.notThereYet)
    case .viewerCannotDecode: world.imageMode = .fails(.cannotDecode(format: "Sigma X3F"))
    case .viewerPhotosDenied: world.photosRefused = true
    case .searchTyping: world.searchMode = .neverAnswers
    case .searchTower: world.capability = .unsupportedTower
    case .clipTooBig, .clipTooBigMoment: world.clipMode = .tooBig
    case .clipNotPlayable: world.clipMode = .notPlayable
    case .clipGone: world.clipMode = .fails(.originalGone)
    case .peopleNone: world.hasPeople = false
    case .libraryEmpty: world.libraryEmpty = true
    #if MV_BACKUP
    case .libraryBackup: world.transferTicks = 3600
    case .libraryBackupEnds: world.transferTicks = 8
    #endif
    default: break
    }
    self.world = world
    NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { _ in
      world.copyWasLeft = true
      world.copyIsAway = true
    }
    NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { _ in
      world.copyIsAway = false
    }
    model = AppModel(services: world.services(), scanner: FixtureScanSurface())
    UserDefaults.standard.removeObject(forKey: "catalogueStaleNoticeSeen")
    // The phone saw epoch 1 before; the hand-over state then sees 2.
    UserDefaults.standard.set(NSNumber(value: UInt64(1)), forKey: "catalogueEpochSeen")
    UserDefaults.standard.set(state == .settingsLight ? "light" : "dark", forKey: Preferences.appearance)
    UserDefaults.standard.set(state != .viewerNoFilmstrip, forKey: Preferences.filmstrip)
  }

  private static func code(for state: FixtureState) -> String? {
    switch state {
    case .connectWorking: return "fixture:slow"
    case .connectExpired: return "fixture:expired"
    case .connectNotReadable: return "fixture:unreadable"
    case .connectUnreachable: return "fixture:unreachable"
    case .connectOwnNetwork: return "fixture:own-network"
    case .connectNoMobileData: return "fixture:no-mobile-data"
    case .connectServiceTooOld: return "fixture:too-old"
    default: return nil
    }
  }

  /// Moves the app to the state, after the first screen has had a frame.
  func arrive() async {
    if state == .connectIdentifyFails { model.setStageForChecking(.connect(code: "fixture:identify-fails-once")); return }
    if state == .connectConfirm { model.setStageForChecking(.connect(code: "fixture:good")); return }
    if state == .connectConfirmGuest { model.setStageForChecking(.connect(code: "fixture:guest")); return }
    if state == .connectNotMatched { model.setStageForChecking(.connect(code: "fixture:good", start: .notMatched)); return }
    if let code = Self.code(for: state) {
      let readsCode = state == .connectExpired || state == .connectNotReadable
      let identity = LibraryIdentity(name: "Home library", fingerprint: "7F3A 92C1")
      model.setStageForChecking(.connect(code: code, start: readsCode ? .checkMatch : .matched(identity)))
      return
    }
    switch state {
    case .firstCopy, .firstCopyWiFi, .firstCopyFails, .firstCopyKeepOpen, .firstCopyNoSpace, .firstCopyResuming:
      model.setStageForChecking(.firstCopy)
    case .peopleFails, .personFails:
      model.tab = .people
    case .peopleReloadFails:
      try? await Task.sleep(for: .seconds(2))
      model.tab = .people
      world.peopleFailures = 1
      world.publish(FixtureWorld.connected(age: 60))
    case .libraryCatalogueDuringScan:
      try? await Task.sleep(for: .seconds(1))
      model.setStageForChecking(.connect(code: "fixture:good", start: .notMatched))
      try? await Task.sleep(for: .seconds(1))
      world.hasPeople = true
      world.publish(FixtureWorld.connected(age: 60))
    case .libraryNewCatalogue:
      try? await Task.sleep(for: .seconds(4))
      world.hasPeople = true
      world.publish(FixtureWorld.connected(age: 60))
    case .searchFailed:
      model.tab = .search
      model.search.setText("boats at sunset")
    case .scan: model.isScanning = true
    case .searchRecents, .searchTower: model.tab = .search
    case .searchTyping:
      model.tab = .search
      model.search.setText("boats at sun")
    case .searchResults:
      model.tab = .search
      model.search.setText("boats at sunset")
    case .searchWarming:
      model.tab = .search
      model.search.setText("the lake that morning")
    case .searchFiltered:
      model.tab = .search
      model.search.setText("boats")
      model.search.setFilters(SearchFilters(place: FixtureLibrary.places[1], kind: .photo))
    case .searchFilteredNothing:
      model.tab = .search
      model.search.setText("zzz purple giraffe")
      model.search.setFilters(SearchFilters(camera: FixtureLibrary.cameras[0].0))
    case .searchNothing:
      model.tab = .search
      model.search.setText("zzz purple giraffe")
    case .people: model.tab = .people
    case .settingsLight: model.tab = .connection
    case .connection, .connectionFromMac, .connectionUnreachable, .connectionOwnNetwork, .connectionServiceTooOld,
         .connectionPublisherChanged:
      model.tab = .connection
    case .libraryFolder:
      if let source = FixtureLibrary.folders[""]?.first, let year = FixtureLibrary.folders[source.id]?.first {
        model.library.show([source, year])
      }
    case .libraryFolderEmpty:
      if let folder = FixtureLibrary.folders[""]?.last { model.library.show([folder]) }
    case .libraryChipPhone, .libraryChipMac:
      if let notice = model.libraryNotice { model.dismiss(notice) }
    case .viewerNoFilmstrip, .viewerRefiningUnknown, .viewerSlowNetwork, .viewerServiceTooOld, .viewerPublisherChanged:
      openViewer(kind: .photo)
    case .viewer, .viewerRefining, .viewerUnreachable, .viewerGone, .viewerNotThereYet, .viewerCannotDecode, .viewerPhotosDenied:
      openViewer(kind: .photo)
    case .details:
      openViewer(kind: .photo, details: true)
    case .clip, .clipTooBig, .clipNotPlayable, .clipGone, .clipStops, .clipStalls:
      openViewer(kind: .clip, play: true)
    case .clipMoment, .clipTooBigMoment:
      openViewer(kind: .clip, play: true, atMoment: true)
    default:
      break
    }
  }

  private func openViewer(kind: MediaKind, details: Bool = false, play: Bool = false, atMoment: Bool = false) {
    model.tab = .library
    Task {
      await waitForLibrary()
      let items = model.library.entries
      guard let index = items.indices.first(where: { items[$0].item.kind == kind }) else { return }
      var entries = items
      if atMoment {
        let item = items[index].item
        entries = items.replacing(index, with: GridEntry(item: item, moment: FixtureLibrary.moments(for: item)[1], position: index))
      }
      model.open(ViewerRequest(context: "library", entries: entries, startIndex: index, showDetails: details, playClip: play),
                 reduceMotion: UIAccessibility.isReduceMotionEnabled)
    }
  }

  private func waitForLibrary() async {
    while model.library.entries.isEmpty { try? await Task.sleep(for: .milliseconds(50)) }
    try? await Task.sleep(for: .milliseconds(250))
  }
}

/// Hosts the app under a state, and the bug button over it.
public struct FixtureHost: View {
  @State private var run: FixtureRun
  private let showsButton: Bool

  public init() {
    let name = UserDefaults.standard.string(forKey: "fixtureState")
    let state = name.flatMap(FixtureState.init(rawValue:)) ?? .welcome
    _run = State(initialValue: FixtureRun(state: state))
    showsButton = UserDefaults.standard.object(forKey: "fixtureChrome") == nil
      || UserDefaults.standard.bool(forKey: "fixtureChrome")
  }

  public var body: some View {
    RootView(model: run.model)
      .id(run.id)
      .task(id: run.id) {
        try? await Task.sleep(for: .milliseconds(150))
        await run.arrive()
      }
      .background(WindowAnchor(showsButton: showsButton) { state in
        run = FixtureRun(state: state)
      })
  }
}

/// Puts the bug button in its own window, above covers and sheets.
private struct WindowAnchor: UIViewRepresentable {
  let showsButton: Bool
  let pick: (FixtureState) -> Void

  final class Coordinator { var overlay: FixtureOverlayWindow? }

  func makeCoordinator() -> Coordinator { Coordinator() }
  func makeUIView(context: Context) -> UIView { UIView() }

  func updateUIView(_ view: UIView, context: Context) {
    DispatchQueue.main.async {
      if let scene = view.window?.windowScene { FixtureFrameMeter.startIfAsked(in: scene) }
    }
    guard showsButton, context.coordinator.overlay == nil else { return }
    DispatchQueue.main.async {
      guard context.coordinator.overlay == nil, let scene = view.window?.windowScene else { return }
      context.coordinator.overlay = FixtureOverlayWindow(scene: scene, pick: pick)
    }
  }
}

private final class FixtureOverlayWindow: UIWindow {
  private let button = UIButton(type: .system)
  private let pick: (FixtureState) -> Void

  init(scene: UIWindowScene, pick: @escaping (FixtureState) -> Void) {
    self.pick = pick
    super.init(windowScene: scene)
    windowLevel = .alert + 1
    backgroundColor = .clear
    let controller = UIViewController()
    controller.view.backgroundColor = .clear
    rootViewController = controller
    var config = UIButton.Configuration.filled()
    config.image = UIImage(systemName: "ladybug")
    config.baseBackgroundColor = UIColor.black.withAlphaComponent(0.55)
    config.baseForegroundColor = .white
    config.cornerStyle = .capsule
    button.configuration = config
    button.accessibilityLabel = "Fixture states"
    button.addTarget(self, action: #selector(openGallery), for: .touchUpInside)
    button.translatesAutoresizingMaskIntoConstraints = false
    controller.view.addSubview(button)
    NSLayoutConstraint.activate([
      button.trailingAnchor.constraint(equalTo: controller.view.safeAreaLayoutGuide.trailingAnchor, constant: -8),
      button.centerYAnchor.constraint(equalTo: controller.view.centerYAnchor, constant: 90),
      button.widthAnchor.constraint(equalToConstant: 44),
      button.heightAnchor.constraint(equalToConstant: 44),
    ])
    isHidden = false
  }

  @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

  override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
    if rootViewController?.presentedViewController != nil { return super.hitTest(point, with: event) }
    let hit = super.hitTest(point, with: event)
    return hit === rootViewController?.view || hit === self ? nil : hit
  }

  @objc private func openGallery() {
    let gallery = UIHostingController(rootView: FixtureGallery { [weak self] state in
      self?.rootViewController?.dismiss(animated: false)
      self?.pick(state)
    })
    gallery.overrideUserInterfaceStyle = .dark
    rootViewController?.present(gallery, animated: true)
  }
}

private struct FixtureGallery: View {
  let pick: (FixtureState) -> Void

  private var groups: [(String, [FixtureState])] {
    var seen: [String] = []
    for state in FixtureState.allCases where !seen.contains(state.groupName) { seen.append(state.groupName) }
    return seen.map { name in (name, FixtureState.allCases.filter { $0.groupName == name }) }
  }

  var body: some View {
    NavigationStack {
      List {
        ForEach(groups, id: \.0) { group in
          Section(group.0) {
            ForEach(group.1) { state in
              Button(state.rawValue) { pick(state) }
            }
          }
        }
      }
      .navigationTitle("States")
    }
  }
}

fileprivate extension FixtureState {
  var groupName: String { group }
}

/// Where the camera would be. The Simulator has none, so these are the codes it could have read.
struct FixtureScanSurface: ScanSurfaceProviding {
  func surface(onCode: @escaping (String) -> Void) -> AnyView {
    AnyView(FixtureScanView(onCode: onCode))
  }
}

private struct FixtureScanView: View {
  let onCode: (String) -> Void

  private let codes: [(String, String)] = [
    ("A good code", "fixture:good"),
    ("An expired code", "fixture:expired"),
    ("Not a MediaViewer code", "fixture:unreadable"),
    ("Synology not reachable", "fixture:unreachable"),
    ("Reachable only at home", "fixture:own-network"),
    ("No mobile data on this iPad", "fixture:no-mobile-data"),
    ("Service needs updating", "fixture:too-old"),
  ]

  var body: some View {
    ZStack {
      Color(white: 0.09)
      VStack(spacing: 8) {
        ForEach(codes, id: \.1) { code in
          Button(code.0) { onCode(code.1) }
            .font(.subheadline)
            .foregroundStyle(.white)
            .frame(maxWidth: 300, minHeight: 44)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.18)))
        }
      }
      .padding(24)
    }
  }
}
