// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Forget must leave nothing on the phone (docs/plans/mobile.md §12): a write into the library's
// folders (an original moved into its store, a downloaded clip created) runs only while its library
// is the paired one, and Forget waits for a write in progress before it removes the folders.
import Foundation

public final class WriteGate: @unchecked Sendable {
  private let gate = NSLock()   // held by a write, and passed by close()
  private let state = NSLock()  // guards `live`; never held while a write runs
  private var live: ObjectIdentifier?

  public init() {}

  /// Writes for `owner` (the open library's handle) may run from now on; any other owner's may not.
  public func open(for owner: AnyObject) {
    state.lock()
    live = ObjectIdentifier(owner)
    state.unlock()
  }

  /// Refuses every later write, then waits for one already running. Forget calls this before it
  /// removes the folders.
  public func close() {
    state.lock()
    live = nil
    state.unlock()
    gate.lock()
    gate.unlock()
  }

  /// Runs `body` only while `owner`'s library is the one open. Keep it short: a create or a move.
  /// nil when refused. `owner` is held by the caller, so its identity cannot be reused meanwhile.
  public func whileLive<T>(_ owner: AnyObject, _ body: () throws -> T) rethrows -> T? {
    gate.lock()
    defer { gate.unlock() }
    state.lock()
    let mine = live == ObjectIdentifier(owner)
    state.unlock()
    guard mine else { return nil }
    return try body()
  }
}
