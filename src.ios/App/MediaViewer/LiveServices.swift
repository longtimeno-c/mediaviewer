// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's services. The library, its search and the tiles in tiny.pack come from the installed
// generation (MobileCore), so the grid and search work with no network. The connection, sync,
// JPEG-512 thumbnails, originals and clips come from the link to the NAS (MobileLink).
import MobileCore
import MobileKit
import MobileLink
import UIKit

enum LiveServices {
  static func services() -> Services {
    let catalogue = LocalCatalogue(location: LibraryLocation(root: LibraryLocation.standard.root,
                                                             pairedLibrary: { LinkConnection.storedLibraryID }))
    let link = LinkServices.make()
    reopenOnNewCatalogue(catalogue, connection: link.connection)
    let images = LayeredImages(local: catalogue.images, remote: link.images)
    return Services(library: catalogue.library, images: images, search: catalogue.search,
                    connection: link.connection, sync: link.connection, clips: link.clips,
                    saver: PhotoLibrarySaver(originals: link.images))
  }

  /// A new `CatalogueInfo` is the only "catalogue changed" signal: the sync has installed a
  /// generation, so open what installed.json now names. Sound search's model arriving (§9) opens
  /// the same generation again, with it.
  private static func reopenOnNewCatalogue(_ catalogue: LocalCatalogue, connection: LinkConnection) {
    Task.detached(priority: .utility) {
      var last = connection.current.catalogue
      var hears = connection.current.soundSearch == .ready
      var library = LinkConnection.LibraryChange(current: connection.current.libraryID)
      for await snapshot in connection.updates() {
        // Forgotten, or another library paired over this one: the open catalogue goes too, or
        // its generation (numbered higher than the new library's first) would stay in force.
        if library.changed(to: snapshot.libraryID) || !snapshot.hasLibrary {
          catalogue.forgetInForce()
          last = nil
        }
        let nowHears = snapshot.soundSearch == .ready
        // At launch the model may be here from before: the generation opened with it.
        guard snapshot.catalogue != last || (nowHears && !hears && !catalogue.hearsSound) else {
          hears = nowHears
          continue
        }
        last = snapshot.catalogue
        hears = nowHears
        if snapshot.catalogue != nil { try? await catalogue.reloadInstalled() }
      }
    }
  }
}

/// §10's ladder across both owners: the tile on the phone paints first, offline or not; the
/// JPEG-512 and the original come over the link and sharpen it.
struct LayeredImages: ImageLadder {
  let local: ImageLadder
  let remote: LinkImages

  func cachedImage(for id: MediaID) -> UIImage? {
    local.cachedImage(for: id) ?? remote.cachedImage(for: id)
  }

  /// The tile from the phone's own catalogue: no network, so it paints offline and first.
  func placeholder(for item: MediaItem) async -> UIImage? {
    await local.thumbnail(for: item)
  }

  /// The JPEG-512 over the link, else the phone's tile.
  func thumbnail(for item: MediaItem) async -> UIImage? {
    if let sharper = await remote.thumbnail(for: item) { return sharper }
    return await local.thumbnail(for: item)
  }

  /// The phone's tile first, then each rung from the link. If the link fails, the tile stays on
  /// screen under the failure.
  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { continuation in
      let task = Task {
        if let tile = await local.thumbnail(for: item), !Task.isCancelled {
          continuation.yield(.image(tile, isOriginal: false))
        }
        do {
          for try await event in remote.ladder(for: item, fittingPixels: fittingPixels) { continuation.yield(event) }
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {
    local.prefetch(items, fittingPixels: fittingPixels)
    remote.prefetch(items, fittingPixels: fittingPixels)
  }
}
