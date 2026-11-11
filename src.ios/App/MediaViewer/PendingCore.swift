// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's services: the library, its search and tiles from the installed generation (MobileCore),
// the real connection, sync and clips from MobileLink, and stand-ins for what has not landed yet
// (the Photos saver, transfer activity).
import AVFoundation
import MobileCore
import MobileKit
import MobileLink
import UIKit

enum PendingCore {
  static func services() -> Services {
    let catalogue = LocalCatalogue(location: .standard)
    let link = LinkServices.make()
    let core = Empty()
    reopenOnNewCatalogue(catalogue, connection: link.connection)
    return Services(library: catalogue.library, images: catalogue.images, search: catalogue.search,
                    connection: link.connection, sync: link.connection, clips: link.clips, saver: core, transfers: core)
  }

  /// A new `CatalogueInfo` is the only "catalogue changed" signal: the sync has installed a
  /// generation, so open what installed.json now names.
  private static func reopenOnNewCatalogue(_ catalogue: LocalCatalogue, connection: LinkConnection) {
    Task.detached(priority: .utility) {
      var last = connection.current.catalogue
      for await snapshot in connection.updates() where snapshot.catalogue != last {
        last = snapshot.catalogue
        if snapshot.catalogue != nil { try? await catalogue.reloadInstalled() }
      }
    }
  }

  private struct Empty: PhotoSaver, TransferMonitor {
    func activity() -> AsyncStream<TransferActivity?> { AsyncStream { $0.finish() } }

    func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> {
      AsyncThrowingStream { $0.finish(throwing: MediaFailure.synologyUnreachable) }
    }
  }
}
