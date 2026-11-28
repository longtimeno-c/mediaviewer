// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup's key for each photo (docs/plans/mobile.md §10b): its iCloud id where iCloud Photos
// gives one, since a local id is per device, else "local:<local id>". Asking Photos for iCloud ids
// is slow on a big library (minutes for tens of thousands), and an asset's iCloud id never
// changes, so the ids from the last listing are used again and only new photos, and those that
// had none yet, are asked about.
import Foundation

public enum BackupKeys {
  public static let localPrefix = "local:"

  /// Local id -> key for `ids`. `known` is the last listing's (local id -> key); `ask` gets the
  /// ids that need asking (never called with none) and answers the iCloud ids it has.
  public static func keys(for ids: [String], known: [String: String],
                          ask: ([String]) -> [String: String]) -> [String: String] {
    var out: [String: String] = [:]
    out.reserveCapacity(ids.count)
    var unknown: [String] = []
    for id in ids {
      if let key = known[id], !key.hasPrefix(localPrefix) {
        out[id] = key
      } else {
        unknown.append(id)
      }
    }
    if !unknown.isEmpty {
      let cloud = ask(unknown)
      for id in unknown { out[id] = cloud[id] ?? localPrefix + id }
    }
    return out
  }
}
