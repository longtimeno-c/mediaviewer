// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where the paired service is (docs/design/28-driftwire.md, "Addresses after pairing"): the
// pairing's addresses, updated from what the service says at each connect (mv_link_addresses_now),
// so a DuckDNS name, a relay or a new LAN address set after pairing is dialled without pairing
// again. The handshake pins the service's key, so an address can only lead back to it.
import Foundation

public struct ServiceAddresses: Equatable, Sendable {
  public var local: String
  public var publicName: String
  public var relay: String
  public init(local: String, publicName: String, relay: String) {
    self.local = local
    self.publicName = publicName
    self.relay = relay
  }

  /// The addresses to keep after a connect: each one the service gave replaces the kept one; an
  /// empty one never wipes it. nil when nothing changed (nothing to save).
  public func updated(by said: ServiceAddresses) -> ServiceAddresses? {
    var next = self
    if !said.local.isEmpty { next.local = said.local }
    if !said.publicName.isEmpty { next.publicName = said.publicName }
    if !said.relay.isEmpty { next.relay = said.relay }
    return next == self ? nil : next
  }
}
