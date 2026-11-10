// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A Sendable handle over the C ABI (src/mobile/mv_mobile_link.h), the blocking queue it runs on,
// and the small helpers every service needs. docs/plans/mobile.md §8.2: everything that can wait
// is on this queue, never the main thread.
import CMobileLink
import Foundation

let linkNetQueue = DispatchQueue(label: "mediaviewer.link.net", qos: .userInitiated, attributes: .concurrent)

func onLinkQueue<T>(_ body: @escaping @Sendable () -> T) async -> T {
  await withCheckedContinuation { c in linkNetQueue.async { c.resume(returning: body()) } }
}

func onLinkQueue<T>(_ body: @escaping @Sendable () throws -> T) async throws -> T {
  try await withCheckedThrowingContinuation { c in
    linkNetQueue.async {
      do { c.resume(returning: try body()) } catch { c.resume(throwing: error) }
    }
  }
}

/// Fixed C char arrays, which Swift imports as tuples, to String.
func linkString<T>(_ tuple: T) -> String {
  withUnsafeBytes(of: tuple) { raw in
    guard let base = raw.baseAddress else { return "" }
    return String(cString: base.assumingMemoryBound(to: CChar.self))
  }
}

func linkBytes<T>(_ tuple: T) -> Data { withUnsafeBytes(of: tuple) { Data($0) } }

/// The open connection, with no shared mutable Swift state; the core locks for itself.
final class LinkHandle: @unchecked Sendable {
  let raw: OpaquePointer

  init(raw: OpaquePointer) { self.raw = raw }
  deinit { mv_link_close(raw) }

  static func open(record: PairingRecord, deviceKey: Data, libraryDir: URL, cacheDir: URL) -> LinkHandle? {
    guard let identity = Data(hex: record.identity), let access = Data(hex: record.accessKey),
          let publisher = Data(hex: record.publisherKey), identity.count == 32, access.count == 32,
          publisher.count == 32, deviceKey.count == 32 else { return nil }
    return identity.withUnsafeBytes { i in
      access.withUnsafeBytes { a in
        deviceKey.withUnsafeBytes { d in
          publisher.withUnsafeBytes { p in
            libraryDir.path.withCString { lib in
              cacheDir.path.withCString { cache in
                record.libraryID.withCString { id in
                  record.local.withCString { local in
                    record.publicName.withCString { pub in
                      record.deviceName.withCString { name in
                        var c = mv_link_config()
                        c.library_dir = lib
                        c.cache_dir = cache
                        c.library_id = id
                        c.local = local
                        c.public_name = pub
                        c.device_name = name
                        c.identity = i.bindMemory(to: UInt8.self).baseAddress
                        c.access_key = a.bindMemory(to: UInt8.self).baseAddress
                        c.device_key = d.bindMemory(to: UInt8.self).baseAddress
                        c.publisher_key = p.bindMemory(to: UInt8.self).baseAddress
                        c.thumb_cache_bytes = 2 << 30
                        return mv_link_open(&c).map { LinkHandle(raw: $0) }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

/// A `mv_cancel` that a Swift task cancellation fires.
final class LinkCancel: @unchecked Sendable {
  let raw: OpaquePointer
  init() { raw = mv_cancel_new() }
  deinit { mv_cancel_free(raw) }
  func fire() { mv_cancel_fire(raw) }
}

func withLinkCancel<T>(_ body: (LinkCancel) async -> T) async -> T {
  let cancel = LinkCancel()
  return await withTaskCancellationHandler {
    await body(cancel)
  } onCancel: {
    cancel.fire()
  }
}

/// The three folders the core and caches use, all excluded from iCloud backup (§8.3).
enum LinkFolders {
  static let library: URL = {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    return prepared(base.appendingPathComponent("library", isDirectory: true))
  }()

  static let thumbs: URL = prepared(cache("thumbs"))
  static let originals: URL = prepared(cache("originals"))
  static let clips: URL = prepared(cache("clips"))

  private static func cache(_ name: String) -> URL {
    FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent(name, isDirectory: true)
  }

  private static func prepared(_ url: URL) -> URL {
    let fm = FileManager.default
    try? fm.createDirectory(at: url, withIntermediateDirectories: true,
                            attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
    var u = url
    var values = URLResourceValues()
    values.isExcludedFromBackup = true
    try? u.setResourceValues(values)
    return url
  }
}
