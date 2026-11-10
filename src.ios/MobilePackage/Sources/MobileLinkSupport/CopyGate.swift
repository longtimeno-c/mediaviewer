// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The first copy's "Waiting for Wi-Fi" (docs/plans/mobile.md §8.4): it waits until bytes may move,
// and ends at once when the task is cancelled or the library is forgotten, whichever comes first.
import Foundation

public enum CopyGate {
  /// Returns when `mayMove` says yes. `waiting` is called each time round while it waits. Throws
  /// CancellationError when cancelled or `stopped`.
  public static func wait(mayMove: () -> Bool, stopped: () -> Bool, waiting: () -> Void,
                          poll: Duration = .milliseconds(500)) async throws {
    while !mayMove() {
      waiting()
      try Task.checkCancellation()
      if stopped() { throw CancellationError() }
      try await Task.sleep(for: poll)
    }
  }
}
