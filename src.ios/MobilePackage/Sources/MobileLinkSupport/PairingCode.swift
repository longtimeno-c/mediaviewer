// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a scanned pairing code is worth before the server is asked (docs/plans/mobile.md §7.4).
// A code past its time is refused here only on a phone that has never paired: one that has may
// already be on the server's list (it paired, then its first copy stopped part-way and the code
// ran out meanwhile), and the server answers such a phone whatever the code, spending nothing.
// A phone the server does not know is still told the code is no good, by the server.
import Foundation

public enum PairingCode {
  public enum Reading: Equatable, Sendable { case usable, expired, unreadable }

  /// `status` is mv_pair_parse's: 0 read, 2 read but past its time, anything else not a code.
  public static func reading(status: Int32, hasDeviceKey: Bool) -> Reading {
    switch status {
    case 0: return .usable
    case 2: return hasDeviceKey ? .usable : .expired
    default: return .unreadable
    }
  }
}
