// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/phone-vpn.md: the VPN through your server is opt-in, asks before the first switch-on,
// says plainly when this build has none, and the sample's demo routes nothing and says so.
import MobileKit
@testable import MobileUI
import XCTest

final class VPNTests: XCTestCase {
  private func demo() -> DemoVPN { DemoVPN(connectingFor: .zero, now: { Date(timeIntervalSince1970: 1_000) }) }

  // MARK: DemoVPN

  func testTheDemoNeedsTheConsentBeforeItTurnsOn() async {
    let vpn = demo()
    XCTAssertEqual(vpn.current.status, .off)
    XCTAssertFalse(vpn.current.consented)
    await vpn.setOn(true)
    XCTAssertEqual(vpn.current.status, .off, "On without the consent.")
    await vpn.acceptConsent()
    await vpn.setOn(true)
    XCTAssertEqual(vpn.current.status, .connected(since: Date(timeIntervalSince1970: 1_000)))
    XCTAssertTrue(vpn.current.isOn)
  }

  func testTheDemoTurnsOffAndRemoveStartsAgain() async {
    let vpn = demo()
    await vpn.acceptConsent()
    await vpn.setOn(true)
    await vpn.setOn(false)
    XCTAssertEqual(vpn.current.status, .off)
    XCTAssertTrue(vpn.current.consented)
    await vpn.setOn(true)
    await vpn.remove()
    XCTAssertEqual(vpn.current, VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true, consented: false))
  }

  func testTheDemoIsAlwaysTheDemo() async throws {
    let vpn = demo()
    XCTAssertTrue(vpn.current.isDemo)
    try await vpn.connect(code: "anything")
    XCTAssertTrue(vpn.current.isDemo)
    XCTAssertEqual(vpn.current.host, DemoVPN.host)
    await vpn.acceptConsent()
    await vpn.setOn(true)
    XCTAssertTrue(vpn.current.isDemo)
    await vpn.remove()
    XCTAssertTrue(vpn.current.isDemo)
  }

  func testTheDemoTellsItsFollowersEachStep() async {
    let vpn = demo()
    var updates = vpn.updates().makeAsyncIterator()
    let first = await updates.next()
    XCTAssertEqual(first?.status, .off)
    await vpn.acceptConsent()
    await vpn.setOn(true)
    let consented = await updates.next()
    let connecting = await updates.next()
    let connected = await updates.next()
    XCTAssertEqual(consented?.consented, true)
    XCTAssertEqual(connecting?.status, .connecting)
    XCTAssertEqual(connected?.status, .connected(since: Date(timeIntervalSince1970: 1_000)))
  }

  // MARK: NoVPN

  func testABuildWithoutTheTunnelSaysSoAndDoesNothing() async throws {
    let vpn = NoVPN()
    XCTAssertEqual(vpn.current.status, .notInThisBuild)
    XCTAssertFalse(vpn.current.isSetUp)
    XCTAssertFalse(vpn.current.isDemo)
    do {
      try await vpn.connect(code: "mediaviewer://relay#home.example.net:51820#0123456789abcdef0123")
      XCTFail("A build with no tunnel took a code.")
    } catch let error as VPNCodeError {
      XCTAssertEqual(error, .notACode)
    }
    await vpn.acceptConsent()
    await vpn.setOn(true)
    XCTAssertEqual(vpn.current.status, .notInThisBuild)
    var all: [VPNSnapshot] = []
    for await s in vpn.updates() { all.append(s) }
    XCTAssertEqual(all, [VPNSnapshot(status: .notInThisBuild)])
    XCTAssertEqual(VPNFlow.page(vpn.current), .notInThisBuild)
  }

  // MARK: VPNSnapshot

  func testTheSwitchIsOnWhileConnectingConnectedOrReconnecting() {
    let on: [VPNStatus] = [.connecting, .connected(since: Date()), .reconnecting]
    let off: [VPNStatus] = [.notInThisBuild, .notSetUp, .off, .failed(.serverUnreachable)]
    for s in on { XCTAssertTrue(VPNSnapshot(status: s).isOn, "\(s)") }
    for s in off { XCTAssertFalse(VPNSnapshot(status: s).isOn, "\(s)") }
  }

  func testOnlyACodeInPlaceIsSetUp() {
    XCTAssertFalse(VPNSnapshot(status: .notInThisBuild).isSetUp)
    XCTAssertFalse(VPNSnapshot(status: .notSetUp).isSetUp)
    for s: VPNStatus in [.off, .connecting, .connected(since: Date()), .reconnecting, .failed(.keyRejected)] {
      XCTAssertTrue(VPNSnapshot(status: s).isSetUp, "\(s)")
    }
  }

  // MARK: The Settings row

  func testTheSettingsRowSaysWhereItIs() {
    let host = "home.example.net:51820"
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .notInThisBuild)), "Not in this version yet")
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .notSetUp)), "Off · paste a VPN code from your server")
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .off, host: host)), "Off · " + host)
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .connecting, host: host)), "Connecting… · " + host)
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .connected(since: Date()), host: host)), "Connected · " + host)
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .reconnecting, host: host)), "Reconnecting… · " + host)
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .failed(.serverUnreachable), host: host)),
                   "Your server didn't answer · " + host)
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .off)), "Off")
    // The demo says it is one, on or off.
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true)), "Try it · demo")
    XCTAssertEqual(VPNCopy.summary(VPNSnapshot(status: .connected(since: Date()), host: DemoVPN.host, isDemo: true)),
                   "Connected · demo")
  }

  // MARK: The switch

  func testTheFirstSwitchOnAsksFirst() {
    let fresh = VPNSnapshot(status: .off, host: "h:1")
    XCTAssertEqual(VPNFlow.step(switchingTo: true, fresh), .askConsent)
    var agreed = fresh
    agreed.consented = true
    XCTAssertEqual(VPNFlow.step(switchingTo: true, agreed), .turnOn)
    // A failed VPN is off: on again goes straight to on once agreed.
    agreed.status = .failed(.serverUnreachable)
    XCTAssertEqual(VPNFlow.step(switchingTo: true, agreed), .turnOn)
    agreed.status = .connected(since: Date())
    XCTAssertEqual(VPNFlow.step(switchingTo: false, agreed), .turnOff)
    XCTAssertEqual(VPNFlow.step(switchingTo: true, agreed), .nothing)
    agreed.status = .reconnecting
    XCTAssertEqual(VPNFlow.step(switchingTo: false, agreed), .turnOff)
    XCTAssertEqual(VPNFlow.step(switchingTo: false, fresh), .nothing)
    // Nothing to switch without a code, or in a build with no tunnel.
    XCTAssertEqual(VPNFlow.step(switchingTo: true, VPNSnapshot(status: .notSetUp)), .nothing)
    XCTAssertEqual(VPNFlow.step(switchingTo: true, VPNSnapshot(status: .notInThisBuild, consented: true)), .nothing)
    // The demo asks too.
    XCTAssertEqual(VPNFlow.step(switchingTo: true, DemoVPN().current), .askConsent)
  }

  func testThePagesFollowTheStatus() {
    XCTAssertEqual(VPNFlow.page(VPNSnapshot(status: .notInThisBuild)), .notInThisBuild)
    XCTAssertEqual(VPNFlow.page(VPNSnapshot(status: .notSetUp)), .notSetUp)
    XCTAssertEqual(VPNFlow.page(VPNSnapshot(status: .off, host: "h:1")), .setUp)
    XCTAssertEqual(VPNFlow.page(VPNSnapshot(status: .failed(.notAllowed), host: "h:1")), .setUp)
    XCTAssertEqual(VPNFlow.page(DemoVPN().current), .setUp)
  }

  func testTheDemoSaysItIsADemo() {
    let demo = DemoVPN().current
    XCTAssertEqual(VPNFlow.toggleTitle(demo), VPNCopy.toggleDemo)
    XCTAssertEqual(VPNFlow.consentMessage(demo), VPNCopy.consentDemo)
    XCTAssertTrue(VPNCopy.consentDemo.contains("Nothing is routed"))
    XCTAssertTrue(VPNCopy.demoBanner.contains("nothing is routed"))
    // The line under the demo's switch must not say traffic leaves through a server.
    XCTAssertEqual(VPNFlow.detail(demo), VPNCopy.detailDemo)
    XCTAssertTrue(VPNFlow.detail(demo).contains("nothing is routed"))
    XCTAssertFalse(VPNFlow.detail(demo).contains("Sample server"))
    let real = VPNSnapshot(status: .off, host: "home.example.net:51820")
    XCTAssertEqual(VPNFlow.toggleTitle(real), VPNCopy.toggle)
    XCTAssertEqual(VPNFlow.consentMessage(real), VPNCopy.consent(host: "home.example.net:51820"))
    XCTAssertTrue(VPNFlow.consentMessage(real).contains("home.example.net:51820"))
  }

  // MARK: The words

  /// The desktop's names (src/addons/phone/screens.cpp, driftwire tree.cpp).
  func testTheWordsAreTheDesktops() {
    XCTAssertEqual(VPNCopy.section, "VPN through your server")
    XCTAssertEqual(VPNCopy.toggle, "All traffic (VPN)")
    XCTAssertTrue(VPNCopy.toggleDemo.hasPrefix("All traffic (VPN)"))
    XCTAssertEqual(VPNCopy.connectHost, "Connect to a VPN host…")
    XCTAssertTrue(VPNCopy.howTo.contains("Use as VPN"))
    XCTAssertTrue(VPNCopy.howTo.contains("VPN code…"))
    for line in [VPNCopy.notInThisBuild, VPNCopy.howTo, VPNCopy.consent(host: "h:1"), VPNCopy.detail(host: "h:1"),
                 VPNCopy.removeMessage, VPNCopy.notACode] {
      XCTAssertFalse(line.contains("relay"), line)
      XCTAssertFalse(line.contains("NAS"), line)
    }
  }

  // MARK: Which VPN the screens use

  @MainActor
  func testTheSampleUsesTheDemoAndEveryOtherLibraryThisBuilds() {
    let demo = demo()
    XCTAssertTrue(AppModel.vpnService(isSample: true, demo: demo, real: NoVPN()).current.isDemo)
    XCTAssertEqual(AppModel.vpnService(isSample: false, demo: demo, real: NoVPN()).current.status, .notInThisBuild)
  }
}
