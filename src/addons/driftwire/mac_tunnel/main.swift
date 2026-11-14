// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A system extension's entry point: hand the process to NetworkExtension,
// which makes PacketTunnelProvider when the add-on starts the tunnel.
import Foundation
import NetworkExtension

autoreleasepool {
  NEProvider.startSystemExtensionMode()
}
dispatchMain()
