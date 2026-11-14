// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The link's pictures in memory, keyed so one library's picture can never stand in for another's:
// by library, by catalogue generation (a new generation can reuse an asset id for another
// picture), then by asset. Asking with a different library empties it first, and Forget empties it.
import Foundation

public final class ScopedCache<Value: AnyObject>: @unchecked Sendable {
  private let cache = NSCache<NSString, Value>()
  private let lock = NSLock()
  private var scope = ""

  public init(countLimit: Int = 0, totalCostLimit: Int = 0) {
    cache.countLimit = countLimit
    cache.totalCostLimit = totalCostLimit
  }

  /// Looking up with another library is what moves the cache to it, emptying it first.
  public func object(library: String, generation: UInt64, id: String) -> Value? {
    let changed = lock.withLock { () -> Bool in
      guard library != scope else { return false }
      scope = library
      return true
    }
    if changed { cache.removeAllObjects() }  // another library: nothing of the last one may show
    return cache.object(forKey: Self.key(library, generation, id))
  }

  /// Stored only for the library the cache is on: a picture that finishes loading after Forget,
  /// or after another library was paired, is dropped instead of bringing its library back.
  public func set(_ value: Value, library: String, generation: UInt64, id: String, cost: Int = 0) {
    lock.withLock {
      guard !library.isEmpty, library == scope else { return }
      cache.setObject(value, forKey: Self.key(library, generation, id), cost: cost)
    }
  }

  /// Everything, for Forget and for a library that is no longer paired.
  public func removeAll() {
    lock.withLock {
      scope = ""
      cache.removeAllObjects()
    }
  }

  private static func key(_ library: String, _ generation: UInt64, _ id: String) -> NSString {
    "\(library)|\(generation)|\(id)" as NSString
  }
}
