// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The real core where it exists (the library, its search and tiles: MobileCore), and the place of
// the parts not built yet (connection, sync, clips, saving: docs/plans/mobile.md M3, M4, M7, M8).
// A phone with no installed generation is a phone with no library.
import AVFoundation
import MobileCore
import MobileKit
import UIKit

enum PendingCore {
  static func services() -> Services {
    let catalogue = LocalCatalogue(location: .standard)
    let stubs = Stubs(catalogue: catalogue)
    return Services(library: catalogue.library, images: catalogue.images, search: catalogue.search,
                    connection: stubs, sync: stubs, clips: stubs, saver: stubs, transfers: stubs)
  }

  private struct Stubs: ConnectionService, CatalogueSync, ClipService, PhotoSaver, TransferMonitor {
    let catalogue: LocalCatalogue

    // One stat at launch so the first screen is the right one (§8.4).
    var current: ConnectionSnapshot {
      let installed = FileManager.default.fileExists(atPath: LibraryLocation.standard.root.appendingPathComponent("gen").path)
      guard installed else { return .noLibrary }
      // A catalogue is here, so the library shows at once; its date and count follow once it
      // has opened (`updates`). Nil would send the person to the first copy.
      let info = catalogue.infoIfOpen ?? CatalogueInfo(source: .synology, updatedAt: Date(), itemCount: 0)
      return ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: info, cacheBytes: 0)
    }

    func updates() -> AsyncStream<ConnectionSnapshot> {
      let first = current
      let catalogue = catalogue
      return AsyncStream { continuation in
        continuation.yield(first)
        Task {
          if first.hasLibrary, let info = await catalogue.info() {
            continuation.yield(ConnectionSnapshot(hasLibrary: true, synology: first.synology, macNearby: false, catalogue: info, cacheBytes: 0))
          }
          continuation.finish()
        }
      }
    }
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
