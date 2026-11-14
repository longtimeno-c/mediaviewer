// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Notices the paired library changing: Forget, or pairing another library over this one. Either
// way, what is open of the old one must close before the new one's catalogue is offered, or the
// old one's newer-numbered generation would stay in force.
public struct LibraryWatch: Sendable {
  private var last: String

  public init(current: String) { last = current }

  /// True when `id` is not the library seen last ("" for none). Going from none to a first library
  /// is not a change: nothing was open.
  public mutating func changed(to id: String) -> Bool {
    defer { last = id }
    return !last.isEmpty && id != last
  }
}
