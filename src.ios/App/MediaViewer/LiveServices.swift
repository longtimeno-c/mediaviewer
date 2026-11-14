// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's services. The library, its search and the tiles in tiny.pack come from the installed
// generation (MobileCore), so the grid and search work with no network. The connection, sync,
// JPEG-512 thumbnails, originals and clips come from the link to the NAS (MobileLink). A library
// with no catalogue yet is its NAS's folders (file mode, docs/plans/phone-in-the-app.md §4): its
// library and search come from the link too, until a catalogue arrives.
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
    let connection = link.connection
    let library = ModeLibrary(catalogue: catalogue.library, files: link.files.library) { connection.current }
    let search = ModeSearch(catalogue: catalogue.search, files: link.files.search) { connection.current }
    return Services(library: library, images: images, search: search,
                    connection: link.connection, sync: link.connection, clips: link.clips,
                    saver: PhotoLibrarySaver(originals: link.images), backup: link.backup)
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

/// File mode while the library has no catalogue on the phone and its NAS has none either.
private func inFileMode(_ snapshot: ConnectionSnapshot) -> Bool { snapshot.fileMode && snapshot.catalogue == nil }

/// The catalogue's library, or the NAS's folders in file mode. A file-mode folder is always the
/// folders', whatever the mode is now, so a list asked for just before a catalogue arrived still answers.
struct ModeLibrary: LibraryStore {
  let catalogue: LibraryStore
  let files: LibraryStore
  let mode: @Sendable () -> ConnectionSnapshot

  private func store(for folder: Folder?) -> LibraryStore {
    if let folder { return LinkFiles.isFileID(folder.id) ? files : catalogue }
    return inFileMode(mode()) ? files : catalogue
  }

  func list(in scope: LibraryScope) async throws -> any MediaList {
    switch scope {
    case .folder(let folder): return try await store(for: folder).list(in: scope)
    case .everything, .person: return try await store(for: nil).list(in: scope)
    }
  }

  func folders(under parent: Folder?) async throws -> [Folder] { try await store(for: parent).folders(under: parent) }
  func people() async throws -> [Person] { try await store(for: nil).people() }

  func details(for item: MediaItem) async throws -> MediaDetails {
    try await (LinkFiles.isFileID(item.id.raw) ? files : catalogue).details(for: item)
  }
}

/// The catalogue's search, or File search over the folder on screen in file mode. Recent searches
/// are one list either way.
struct ModeSearch: SearchService {
  let catalogue: SearchService
  let files: SearchService
  let mode: @Sendable () -> ConnectionSnapshot

  private var current: SearchService { inFileMode(mode()) ? files : catalogue }

  func capability() async -> SearchCapability { await current.capability() }
  func recents() async -> [String] { await catalogue.recents() }
  func remember(_ query: String) async { await catalogue.remember(query) }
  func forget(_ query: String) async { await catalogue.forget(query) }
  func forgetAll() async { await catalogue.forgetAll() }
  func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] {
    try await current.search(query, filters: filters)
  }
  func answer(_ query: String, filters: SearchFilters) async throws -> SearchAnswer {
    try await current.answer(query, filters: filters)
  }
  func filterOptions() async -> SearchFilterOptions { await current.filterOptions() }
  func isWarmingUp() async -> Bool { await current.isWarmingUp() }
}

/// §10's ladder across both owners: the tile on the phone paints first, offline or not; the
/// JPEG-512 and the original come over the link and sharpen it.
struct LayeredImages: ImageLadder {
  let local: ImageLadder
  let remote: LinkImages

  /// The sharpest picture in memory first: a tile that already showed its JPEG-512 paints it
  /// again in its first frame, rather than the 64 px tile and then a second swap.
  func cachedImage(for id: MediaID) -> UIImage? {
    remote.memoryImage(for: id) ?? local.cachedImage(for: id) ?? remote.cachedImage(for: id)
  }

  /// The tile from the phone's own catalogue: no network, so it paints offline and first. A
  /// file-mode item has none.
  func placeholder(for item: MediaItem) async -> UIImage? {
    if LinkFiles.isFileID(item.id.raw) { return nil }
    return await local.thumbnail(for: item)
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
