// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's services: the real connection, sync, pictures and clips from MobileLink, and stand-ins
// for what its owner has not landed yet (docs/plans/mobile.md M5 search, the Photos saver).
import AVFoundation
import MobileKit
import MobileLink
import UIKit

enum PendingCore {
  static func services() -> Services {
    let core = Empty()
    let link = LinkServices.make()
    return Services(library: link.library, images: link.images, search: core, connection: link.connection, sync: link.connection,
                    clips: link.clips, saver: core, transfers: core)
  }

  private struct Empty: SearchService, PhotoSaver, TransferMonitor {
    func capability() async -> SearchCapability { .ready }
    func recents() async -> [String] { [] }
    func remember(_ query: String) async {}
    func forget(_ query: String) async {}
    func forgetAll() async {}
    func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] { [] }
    func filterOptions() async -> SearchFilterOptions { .none }
    func isWarmingUp() async -> Bool { false }

    func activity() -> AsyncStream<TransferActivity?> { AsyncStream { $0.finish() } }

    func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> {
      AsyncThrowingStream { $0.finish(throwing: MediaFailure.synologyUnreachable) }
    }
  }
}
