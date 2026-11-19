// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN screens' words and accessibility identifiers, shared with the tests. The names follow the
// desktop's (src/addons/phone/screens.cpp "VPN through your server" / "Use as VPN", driftwire
// tree.cpp "All traffic (VPN)", "Connect to a VPN host…"); the consent follows
// docs/plans/phone-vpn.md §4.2 with "server" for "relay", as the desktop says it.
import Foundation
import MobileKit

enum VPNCopy {
  /// Settings' section, and the VPN screen's title.
  static let section = "VPN through your server"
  /// The switch, as the desktop's.
  static let toggle = "All traffic (VPN)"
  static let toggleDemo = "All traffic (VPN) — demo"

  // Not in this build.
  static let notInThisBuild =
    "Not in this version yet. The part of MediaViewer that carries your traffic is still being built. When it's here, you'll paste the VPN code from your server on this screen."

  // Setting up.
  static let howTo =
    "On your server, in MediaViewer: Settings → Add-ons → Phone access → Use as VPN, then VPN code…. Copy it and paste it here."
  static let connectHost = "Connect to a VPN host…"
  static let connectAnother = "Connect to another…"
  static let codeField = "VPN code"
  static let connect = "Connect"
  static let notACode = "That isn't a VPN code. Copy it again from Phone access on your server (VPN code…)."
  static let host = "VPN host"

  /// What the switch does, under it.
  static func detail(host: String) -> String {
    "Every app's traffic leaves through your server at \(host), so sites see your home's address. Your server can see which sites you visit, not what you send over HTTPS."
  }

  // Status.
  static let status = "Status"
  static func status(_ s: VPNStatus) -> String {
    switch s {
    case .notInThisBuild: return "Not in this version yet"
    case .notSetUp: return "Not set up"
    case .off: return "Off"
    case .connecting: return "Connecting…"
    case .connected: return "Connected"
    case .reconnecting: return "Reconnecting…"
    case .failed(let p): return problem(p)
    }
  }

  static func problem(_ p: VPNProblem) -> String {
    switch p {
    case .serverUnreachable: return "Your server didn't answer"
    case .keyRejected: return "Your server refused this code. Copy a new one."
    case .notAllowed: return "iOS wasn't allowed to add the VPN"
    case .other(let words): return words
    }
  }

  /// The Settings row's second line.
  static func summary(_ snapshot: VPNSnapshot) -> String {
    if snapshot.isDemo { return snapshot.isOn ? status(snapshot.status) + " · demo" : "Try it · demo" }
    switch snapshot.status {
    case .notInThisBuild: return "Not in this version yet"
    case .notSetUp: return "Off · paste a VPN code from your server"
    default:
      guard let host = snapshot.host else { return status(snapshot.status) }
      return status(snapshot.status) + " · " + host
    }
  }

  // Consent (App Review 5.4), before the first switch-on.
  static let consentTitle = "Send all traffic through your server?"
  static func consent(host: String) -> String {
    "Every app's traffic will leave through your own server at \(host). Your server can see which sites you visit and when; it can't see what you send over HTTPS. MediaViewer's developers see nothing: we run no server, keep no logs and collect no data."
  }
  static let consentDemo =
    "This is a demo. Nothing is routed: your traffic leaves this phone as usual. It shows how the VPN works with your own server."
  static let turnOn = "Turn On"
  static let notNow = "Not Now"

  // The demo's banner, for as long as the screen shows the demo.
  static let demoBanner =
    "Demo: nothing is routed. Your traffic leaves this phone as usual; this shows how the VPN works with your own server."

  // Remove.
  static let remove = "Remove VPN"
  static let removeTitle = "Remove the VPN?"
  static let removeMessage =
    "The VPN code and iOS's VPN configuration are removed from this phone. Nothing on your server changes."
}

/// Accessibility identifiers the UI tests find.
enum VPNIDs {
  static let settingsRow = "settings.vpn"
  static let toggle = "vpn.toggle"
  static let status = "vpn.status"
  static let connectHost = "vpn.connectHost"
  static let codeField = "vpn.code"
  static let connect = "vpn.connect"
  static let codeError = "vpn.codeError"
  static let consentTurnOn = "vpn.consent.turnOn"
  static let consentNotNow = "vpn.consent.notNow"
  static let demoBanner = "vpn.demoBanner"
  static let remove = "vpn.remove"
  /// Remove VPN in the confirmation that asks first.
  static let removeConfirm = "vpn.remove.confirm"
  static let notInThisBuild = "vpn.notInThisBuild"
}
