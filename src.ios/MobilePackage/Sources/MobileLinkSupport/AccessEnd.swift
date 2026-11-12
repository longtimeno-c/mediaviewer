// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A time-limited phone whose access ended (docs/plans/nas-owner-and-guests.md §8.2, call C6):
// when the phone says so, and when it deletes its copy of the library. The NAS decides; the
// phone's own clock only ever changes what is said, never what is deleted.
import Foundation

public enum AccessEnd {
  public struct Decision: Equatable, Sendable {
    /// Say "Your access ended", not "unreachable" or "removed".
    public let showEnded: Bool
    /// Delete the local copy (catalogue, tiles, originals, clips), keeping the pairing.
    public let dropLocalCopy: Bool
    public init(showEnded: Bool, dropLocalCopy: Bool) {
      self.showEnded = showEnded
      self.dropLocalCopy = dropLocalCopy
    }
  }

  /// `serviceSaidEnded`: the NAS refused this phone as expired. `noRouteAnswered`: nothing answered
  /// at all (not a refusal of another kind). `storedEnd`: the end the NAS last gave, unix seconds.
  public static func decide(serviceSaidEnded: Bool, noRouteAnswered: Bool, storedEnd: Double?,
                            now: Double) -> Decision {
    if serviceSaidEnded { return Decision(showEnded: true, dropLocalCopy: true) }
    // Offline, past the end this phone was told: said, never acted on. The owner may have
    // extended it meanwhile, and a phone's clock can be wrong.
    if noRouteAnswered, let end = storedEnd, end <= now { return Decision(showEnded: true, dropLocalCopy: false) }
    return Decision(showEnded: false, dropLocalCopy: false)
  }

  /// Runs `remove` only while the library stored on this phone is still `expected` (a pairing over
  /// it, or a Forget, meanwhile leaves everything to that). True when it ran.
  @discardableResult
  public static func dropIfStill(_ expected: String, stored: () -> String?, remove: () -> Void) -> Bool {
    guard let current = stored(), current == expected else { return false }
    remove()
    return true
  }

  /// The folders of the local copy that are plain files on this phone: deleted whole.
  public static func removeFolders(_ folders: [URL], fileManager: FileManager = .default) {
    for folder in folders { try? fileManager.removeItem(at: folder) }
  }
}
