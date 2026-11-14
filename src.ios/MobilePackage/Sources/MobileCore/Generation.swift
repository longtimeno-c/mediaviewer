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
  private static let serials = NSLock()
  nonisolated(unsafe) private static var lastSerial: UInt64 = 0

  let raw: OpaquePointer
  let directory: URL
  /// This opening's own number, for `MediaID.catalogue`: unique while the app runs.
  let serial: UInt64
  let epoch: UInt64
  let number: UInt64
  private let warmLock = NSLock()
  private var warmed = false
  let createdUnix: Int64
  let itemCount: Int
  /// Search hears clips (§9): sound search's model was here and answers this catalogue.
  let hearsSound: Bool

  /// `sound`: sound search's model folder (§9), when it is on the phone; nil otherwise.
  init(directory: URL, libraryRoot: URL, work: URL, sound: URL? = nil) throws {
    var options = mv_mobile_open_options(
      struct_size: UInt32(MemoryLayout<mv_mobile_open_options>.size), generation_dir: nil, library_dir: nil, work_dir: nil,
      sound_dir: nil)
    var out: OpaquePointer?
    let status = directory.path.withCString { dir in
      libraryRoot.path.withCString { library in
        work.path.withCString { work in
          (sound?.path ?? "").withCString { sound in
            options.generation_dir = dir
            options.library_dir = library
            options.work_dir = work
            options.sound_dir = sound.pointee == 0 ? nil : sound
            return mv_mobile_library_open(&options, &out)
          }
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
    serial = Self.serials.withLock {
      Self.lastSerial += 1
      return Self.lastSerial
    }
    var info = mv_mobile_library_info()
    mv_mobile_library_info_get(opened, &info)
    epoch = info.epoch
    number = info.generation
    createdUnix = info.created_unix
    itemCount = Int(info.assets)
    hearsSound = info.has_sound != 0
  }

  /// True once: the caller then starts what follows the first paint.
  func claimWarmUp() -> Bool {
    warmLock.withLock {
      defer { warmed = true }
      return !warmed
    }
  }

  /// (epoch, generation) only goes up (§5 rule 7). The same generation opened again once sound
  /// search's model has arrived replaces the one opened without it.
  func isNewer(than other: Generation) -> Bool {
    if (epoch, number) == (other.epoch, other.number) { return hearsSound && !other.hearsSound }
    return (epoch, number) > (other.epoch, other.number)
  }

  deinit {
    // Closing stops the engine and joins its threads: never on whichever thread let go last,
    // which can be the main one.
    let raw = raw
    DispatchQueue.global(qos: .utility).async { mv_mobile_library_close(raw) }
  }
}

/// The catalogue's wall clock (a date taken, read as UTC) as a `Date` that formats to the same
/// wall clock wherever the phone is, as the query language's dates do.
func capturedDate(wallSeconds: Int64) -> Date {
  let utc = Date(timeIntervalSince1970: TimeInterval(wallSeconds))
  return utc.addingTimeInterval(-TimeInterval(TimeZone.current.secondsFromGMT(for: utc)))
}

func mediaItem(_ row: mv_mobile_row, in generation: Generation) -> MediaItem {
  let name = row.file_name.map { String(decoding: UnsafeBufferPointer(start: UnsafeRawPointer($0).assumingMemoryBound(to: UInt8.self), count: Int(row.file_name_len)), as: UTF8.self) } ?? ""
  return MediaItem(id: MediaID(String(row.asset_id), catalogue: generation.serial),
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
      guard mv_mobile_list_month_at(opened, UInt32(i), &month) == MV_MOBILE_OK else { continue }
      ranges.append(MonthRange(key: Int(month.key), date: capturedDate(wallSeconds: month.wall_seconds),
                               range: Int(month.first)..<Int(month.first + month.count)))
    }
    months = ranges
  }

  deinit { mv_mobile_list_close(raw) }

  func item(at index: Int) -> MediaItem {
    var row = mv_mobile_row()
    _ = mv_mobile_list_row_at(raw, UInt32(index), &row)  // an index from `count`: always one of ours
    return mediaItem(row, in: generation)
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
