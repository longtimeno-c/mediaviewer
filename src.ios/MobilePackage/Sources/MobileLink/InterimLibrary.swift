// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A flat LibraryStore straight from catalog.db, so the grid can be proven before the search
// session's library store (docs/plans/mobile.md M5) lands. Everything else about the library is that store's.
import CMobileLink
import Foundation
import MobileKit

public struct InterimLibrary: LibraryStore {
  private let connection: LinkConnection

  public init(connection: LinkConnection) { self.connection = connection }

  public func list(in scope: LibraryScope) async throws -> any MediaList {
    guard let link = connection.link else { return ArrayMediaList([]) }
    return await onLinkQueue {
      final class Rows { var items: [MediaItem] = [] }
      let rows = Rows()
      _ = mv_assets_list(link.raw, { ctx, asset, kind, takenAt, durationMs, name in
        guard let ctx else { return }
        let rows = Unmanaged<Rows>.fromOpaque(ctx).takeUnretainedValue()
        let fileName = name.map { String(cString: $0) } ?? ""
        rows.items.append(MediaItem(id: MediaID(String(asset)), kind: kind == 2 ? .clip : .photo,
                                    capturedAt: Date(timeIntervalSince1970: TimeInterval(takenAt)), pixelWidth: 4, pixelHeight: 3,
                                    duration: durationMs > 0 ? TimeInterval(durationMs) / 1000 : nil, fileName: fileName))
      }, Unmanaged.passUnretained(rows).toOpaque())
      return ArrayMediaList(rows.items)
    }
  }

  public func folders(under parent: Folder?) async throws -> [Folder] { [] }
  public func people() async throws -> [Person] { [] }
  public func details(for item: MediaItem) async throws -> MediaDetails {
    MediaDetails(place: nil, camera: nil, lens: nil, exposure: nil, folderPath: "", formatName: (item.fileName as NSString).pathExtension.uppercased(), fileBytes: 0)
  }
}
