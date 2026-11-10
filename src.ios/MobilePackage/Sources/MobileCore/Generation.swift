// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One installed generation (docs/plans/mobile.md §5). Owns the C library; the lists and tiles
// taken from it keep it alive, so a swap never pulls the rows out from under a screen.
import CMobileSearch
import Foundation
import MobileKit

/// What went wrong reading the catalogue on this phone.
public enum CatalogueOpenFailure: Error, Hashable, Sendable {
  case notInstalled
  case corrupt
  case unreadable
}

final class Generation: @unchecked Sendable {
  let raw: OpaquePointer
  let directory: URL
  let number: UInt64
  let createdUnix: Int64
  let itemCount: Int

  init(directory: URL, libraryRoot: URL, work: URL) throws {
    var options = mv_mobile_open_options(
      struct_size: UInt32(MemoryLayout<mv_mobile_open_options>.size), generation_dir: nil, library_dir: nil, work_dir: nil)
    var out: OpaquePointer?
    let status = directory.path.withCString { dir in
      libraryRoot.path.withCString { library in
        work.path.withCString { work in
          options.generation_dir = dir
          options.library_dir = library
          options.work_dir = work
          return mv_mobile_library_open(&options, &out)
        }
      }
    }
    guard status == MV_MOBILE_OK, let opened = out else {
      switch status {
      case MV_MOBILE_ERR_CORRUPT, MV_MOBILE_ERR_UNSUPPORTED_FORMAT: throw CatalogueOpenFailure.corrupt
      case MV_MOBILE_ERR_NOT_FOUND: throw CatalogueOpenFailure.notInstalled
      default: throw CatalogueOpenFailure.unreadable
      }
    }
    raw = opened
    self.directory = directory
    var info = mv_mobile_library_info()
    mv_mobile_library_info_get(opened, &info)
    number = info.generation
    createdUnix = info.created_unix
    itemCount = Int(info.assets)
  }

  deinit { mv_mobile_library_close(raw) }
}

/// The catalogue's wall clock (a date taken, read as UTC) as a `Date` that formats to the same
/// wall clock wherever the phone is, as the query language's dates do.
func capturedDate(wallSeconds: Int64) -> Date {
  let utc = Date(timeIntervalSince1970: TimeInterval(wallSeconds))
  return utc.addingTimeInterval(-TimeInterval(TimeZone.current.secondsFromGMT(for: utc)))
}

func mediaItem(_ row: mv_mobile_row) -> MediaItem {
  let name = row.file_name.map { String(decoding: UnsafeBufferPointer(start: UnsafeRawPointer($0).assumingMemoryBound(to: UInt8.self), count: Int(row.file_name_len)), as: UTF8.self) } ?? ""
  return MediaItem(id: MediaID(String(row.asset_id)),
                   kind: row.kind == MV_MOBILE_KIND_CLIP ? .clip : .photo,
                   capturedAt: capturedDate(wallSeconds: row.wall_seconds),
                   pixelWidth: 0, pixelHeight: 0,
                   duration: row.kind == MV_MOBILE_KIND_CLIP ? TimeInterval(row.duration_ms) / 1000 : nil,
                   fileName: name,
                   aspectHint: row.tile_height > 0 ? Double(row.tile_width) / Double(row.tile_height) : nil)
}

/// A `MediaList` over the rows the core already holds: nothing is copied into Swift values, and
/// `item(at:)` builds one row when asked.
final class CoreMediaList: MediaList, @unchecked Sendable {
  private let raw: OpaquePointer
  private let generation: Generation
  let count: Int
  let months: [MonthRange]

  init(generation: Generation, scope: UInt32, key: String?) throws {
    self.generation = generation
    var out: OpaquePointer?
    let status = key.withOptionalCString { mv_mobile_list_open(generation.raw, scope, $0, &out) }
    guard status == MV_MOBILE_OK, let opened = out else { throw CatalogueOpenFailure.unreadable }
    raw = opened
    count = Int(mv_mobile_list_count(opened))
    let total = Int(mv_mobile_list_month_count(opened))
    var ranges: [MonthRange] = []
    ranges.reserveCapacity(total)
    for i in 0..<total {
      var month = mv_mobile_month()
      mv_mobile_list_month_at(opened, UInt32(i), &month)
      ranges.append(MonthRange(key: Int(month.key), date: capturedDate(wallSeconds: month.wall_seconds),
                               range: Int(month.first)..<Int(month.first + month.count)))
    }
    months = ranges
  }

  deinit { mv_mobile_list_close(raw) }

  func item(at index: Int) -> MediaItem {
    var row = mv_mobile_row()
    mv_mobile_list_row_at(raw, UInt32(index), &row)
    return mediaItem(row)
  }
}

extension Optional where Wrapped == String {
  func withOptionalCString<R>(_ body: (UnsafePointer<CChar>?) -> R) -> R {
    switch self {
    case .some(let s): return s.withCString { body($0) }
    case .none: return body(nil)
    }
  }
}
