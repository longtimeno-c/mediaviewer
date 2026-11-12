// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The seams. The screens talk only to these; the mv_mobile_* core (docs/plans/mobile.md §8.1)
// implements them later, and MobileFixtures implements them for checking the interface today.
// Nothing here may block its caller: main-thread methods return at once, everything else is async.
import AVFoundation
import UIKit

public protocol LibraryStore: Sendable {
  /// A folder's scope includes every folder beneath it, newest first. Honours cancellation:
  /// switching folders drops a load in flight.
  func list(in scope: LibraryScope) async throws -> any MediaList
  /// nil: the library's top level.
  func folders(under parent: Folder?) async throws -> [Folder]
  /// Empty unless the Mac has named people; the People tab appears only when it is not.
  func people() async throws -> [Person]
  func details(for item: MediaItem) async throws -> MediaDetails
}

/// One step of the §10 ladder.
public enum LadderEvent: Sendable {
  case image(UIImage, isOriginal: Bool)
  /// The original is on its way; `totalBytes` nil while unknown.
  case loading(doneBytes: Int64, totalBytes: Int64?)
}

public protocol ImageLadder: Sendable {
  /// The best picture already in memory or `tiny.pack`. Called on the main thread for first paint,
  /// so it must return within a frame.
  func cachedImage(for id: MediaID) -> UIImage?
  /// What is already on the phone, quickly and without the network, painted while `thumbnail`
  /// is still on its way. nil when the ladder has nothing quicker than `thumbnail`.
  func placeholder(for item: MediaItem) async -> UIImage?
  /// A grid-sized picture.
  func thumbnail(for item: MediaItem) async -> UIImage?
  /// Each better picture as it arrives, in place of the last. Throws `MediaFailure` when the
  /// original will not come; whatever was already sent stays on screen.
  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error>
  /// §8.2 net lane 2: the viewer's ±2.
  func prefetch(_ items: [MediaItem], fittingPixels: CGSize)
}

public extension ImageLadder {
  func placeholder(for item: MediaItem) async -> UIImage? { nil }
}

public enum SearchCapability: Hashable, Sendable {
  case ready
  /// The library's meaning search needs a model this phone cannot run.
  case unsupportedTower
}

public protocol SearchService: Sendable {
  func capability() async -> SearchCapability
  func recents() async -> [String]
  func remember(_ query: String) async
  func forget(_ query: String) async
  func forgetAll() async
  /// Honours cancellation: one query at a time, newest wins.
  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit]
  func filterOptions() async -> SearchFilterOptions
  /// True while a search would be waiting for the model to load. Asked only after a search has
  /// already taken a moment, so a quick answer never mentions it.
  func isWarmingUp() async -> Bool
}

public protocol ConnectionService: Sendable {
  /// What is known right now, without waiting, so launch paints the right screen first.
  var current: ConnectionSnapshot { get }
  /// The current state, then every change.
  func updates() -> AsyncStream<ConnectionSnapshot>
  /// Reads the scanned code without pairing, so the person can compare it with the desktop.
  /// Throws `PairingFailure.codeExpired` or `.codeNotReadable`.
  func identify(code: String) async throws -> LibraryIdentity
  /// Called with the identity the person has confirmed matches the desktop, which the core checks
  /// against what the code carries. Finishes when pairing is done; throws `PairingFailure`.
  func pair(code: String, confirmed: LibraryIdentity) -> AsyncThrowingStream<PairingEvent, Error>
  func retry() async
  func forget() async
}

public protocol CatalogueSync: Sendable {
  /// iOS 26 and later keep a copy the person started going after they leave the app (§8.4).
  var continuesAfterLeaving: Bool { get }
  /// Finishes when the catalogue is on the phone; throws `CopyFailure`. Cancelling the iteration
  /// stops the copy.
  func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error>
}

public enum ClipRoute {
  /// AVPlayer can have it; the asset's loader reads from the NAS.
  case stream(AVURLAsset)
  case tooBigToStream(neededMbps: Int, availableMbps: Int, fileBytes: Int64)
  /// MKV, WebM, AVI, TS, MPEG-2: moments as stills, and the file.
  case notPlayableHere(containerName: String, fileBytes: Int64)
  case unavailable(MediaFailure)
}

public protocol ClipService: Sendable {
  func route(for item: MediaItem) async -> ClipRoute
  func moments(for item: MediaItem) async -> [Moment]
  func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage?
  /// Copies the whole clip to the phone, ending with `.finished` and the local file.
  func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error>
}

public protocol PhotoSaver: Sendable {
  /// Saves a copy to the phone's Photos library.
  func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error>
}

/// Everything the screens need, handed in at launch.
public struct Services: Sendable {
  public let library: LibraryStore
  public let images: ImageLadder
  public let search: SearchService
  public let connection: ConnectionService
  public let sync: CatalogueSync
  public let clips: ClipService
  public let saver: PhotoSaver
  /// Phone backup (1.1, §10b).
  public private(set) var backup: BackupService = NoBackup()

  public init(library: LibraryStore, images: ImageLadder, search: SearchService,
              connection: ConnectionService, sync: CatalogueSync, clips: ClipService,
              saver: PhotoSaver, backup: BackupService) {
    self.init(library: library, images: images, search: search, connection: connection, sync: sync,
              clips: clips, saver: saver)
    self.backup = backup
  }

  public init(library: LibraryStore, images: ImageLadder, search: SearchService,
              connection: ConnectionService, sync: CatalogueSync, clips: ClipService,
              saver: PhotoSaver) {
    self.library = library
    self.images = images
    self.search = search
    self.connection = connection
    self.sync = sync
    self.clips = clips
    self.saver = saver
  }
}
