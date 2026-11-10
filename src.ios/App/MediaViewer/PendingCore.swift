// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Holds the place of the mv_mobile_* core until it is built (docs/plans/mobile.md M3–M8): a phone
// with no library, on which any scan is refused. Replace `services()` with the real implementations.
import AVFoundation
import MobileKit
import UIKit

enum PendingCore {
  static func services() -> Services {
    let core = Empty()
    return Services(library: core, images: core, search: core, connection: core, sync: core, clips: core, saver: core, transfers: core)
  }

  private struct Empty: LibraryStore, ImageLadder, SearchService, ConnectionService, CatalogueSync, ClipService, PhotoSaver, TransferMonitor {
    func list(in scope: LibraryScope) async throws -> any MediaList { ArrayMediaList([]) }
    func folders(under parent: Folder?) async throws -> [Folder] { [] }
    func people() async throws -> [Person] { [] }
    func details(for item: MediaItem) async throws -> MediaDetails { throw MediaFailure.synologyUnreachable }

    func cachedImage(for id: MediaID) -> UIImage? { nil }
    func thumbnail(for item: MediaItem) async -> UIImage? { nil }
    func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
      AsyncThrowingStream { $0.finish(throwing: MediaFailure.synologyUnreachable) }
    }
    func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {}

    func capability() async -> SearchCapability { .ready }
    func recents() async -> [String] { [] }
    func remember(_ query: String) async {}
    func forget(_ query: String) async {}
    func forgetAll() async {}
    func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] { [] }
    func filterOptions() async -> SearchFilterOptions { .none }
    func isWarmingUp() async -> Bool { false }

    var current: ConnectionSnapshot { .noLibrary }
    func updates() -> AsyncStream<ConnectionSnapshot> { AsyncStream { $0.finish() } }
    func identify(code: String) async throws -> LibraryIdentity { throw PairingFailure.codeNotReadable }
    func pair(code: String, confirmed: LibraryIdentity) -> AsyncThrowingStream<PairingEvent, Error> {
      AsyncThrowingStream { $0.finish(throwing: PairingFailure.codeNotReadable) }
    }
    func retry() async {}
    func forget() async {}

    var continuesAfterLeaving: Bool { false }
    func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error> {
      AsyncThrowingStream { $0.finish(throwing: CopyFailure.synologyUnreachable) }
    }

    func route(for item: MediaItem) async -> ClipRoute { .unavailable(.synologyUnreachable) }
    func moments(for item: MediaItem) async -> [Moment] { [] }
    func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage? { nil }
    func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> {
      AsyncThrowingStream { $0.finish(throwing: MediaFailure.synologyUnreachable) }
    }

    func activity() -> AsyncStream<TransferActivity?> { AsyncStream { $0.finish() } }

    func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> {
      AsyncThrowingStream { $0.finish(throwing: MediaFailure.synologyUnreachable) }
    }
  }
}
