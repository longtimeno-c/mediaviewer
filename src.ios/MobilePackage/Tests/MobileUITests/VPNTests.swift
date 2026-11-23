// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/phone-vpn.md: the VPN through your server is opt-in, asks before the first switch-on,
// says plainly when this build has none, and the sample's demo routes nothing and says so.
import MobileKit
import SwiftUI
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

  /// Seen by hand in the simulator: "hello" was taken and turned the demo off, as if any text worked.
  func testTheDemoRefusesTextThatIsNotAVPNCode() async throws {
    let vpn = demo()
    await vpn.acceptConsent()
    await vpn.setOn(true)
    for text in ["hello", "", "   ", "mediaviewer://pair#abc", "mediaviewer://relay", "mediaviewer://relay#",
                 "mediaviewer://relay#home.example.net#0123456789abcdef0123", "home.example.net:port#key",
                 "home.example.net:51820#", ":51820#0123456789abcdef0123", "home.example.net:51820 #0123456789abcdef0123",
                 // As the core refuses them: a short key, two #, port 0 or past 65535, a colon outside brackets.
                 "home.example.net:51820#short", "home.example.net:51820#0123456789abcdef#0123",
                 "home.example.net:0#0123456789abcdef0123", "home.example.net:65536#0123456789abcdef0123",
                 "https://example.com:443#0123456789abcdef0123", "mediaviewer://pair:1#0123456789abcdef0123",
                 "::1:51820#0123456789abcdef0123"] {
      do {
        try await vpn.connect(code: text)
        XCTFail("The demo took \"\(text)\" as a VPN code")
      } catch let error as VPNCodeError {
        XCTAssertEqual(error, .notACode)
      }
    }
    XCTAssertTrue(vpn.current.isOn, "A refused code must leave the demo as it was")
    try await vpn.connect(code: "  MediaViewer://relay#home.example.net:51820#0123456789abcdef0123\n")
    try await vpn.connect(code: "mediaviewer://relay-tls#home.example.net:443#0123456789abcdef0123")
    // The bare form the server's parser also takes.
    try await vpn.connect(code: "home.example.net:51820#0123456789abcdef0123")
    try await vpn.connect(code: "[::1]:51820#0123456789abcdef0123")
    XCTAssertFalse(vpn.current.isOn, "A code in place leaves the VPN off")
    XCTAssertTrue(vpn.current.consented, "A new code keeps the consent")
    XCTAssertEqual(vpn.current.host, DemoVPN.host, "The demo never takes a host from a code")
  }

  /// A follower that stops while the demo changes must not deadlock it: the stop takes the demo's
  /// lock from inside the Swift runtime's task lock, which telling a follower also needs.
  func testAFollowerStoppingWhileTheDemoChangesDoesNotDeadlock() async {
    let vpn = demo()
    for _ in 0..<2_000 {
      let follower = Task.detached { for await _ in vpn.updates() {} }
      await Task.yield()
      let change = Task.detached { await vpn.remove() }
      follower.cancel()
      await change.value
      await follower.value
    }
  }

  func testTheDemoIsAlwaysTheDemo() async throws {
    let vpn = demo()
    XCTAssertTrue(vpn.current.isDemo)
    try await vpn.connect(code: "mediaviewer://relay#home.example.net:51820#0123456789abcdef0123")
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
    // The demo's consent never asks to send traffic anywhere.
    XCTAssertEqual(VPNFlow.consentTitle(demo), VPNCopy.consentTitleDemo)
    XCTAssertFalse(VPNFlow.consentTitle(demo).contains("through your server"))
    // Under the code field, the demo says a code is only checked; the real VPN adds nothing.
    XCTAssertEqual(VPNFlow.codeNote(demo), VPNCopy.codeDemo)
    XCTAssertTrue(VPNCopy.consentDemo.contains("Nothing is routed"))
    XCTAssertTrue(VPNCopy.demoBanner.contains("nothing is routed"))
    // The line under the demo's switch must not say traffic leaves through a server.
    XCTAssertEqual(VPNFlow.detail(demo), VPNCopy.detailDemo)
    XCTAssertTrue(VPNFlow.detail(demo).contains("nothing is routed"))
    XCTAssertFalse(VPNFlow.detail(demo).contains("Sample server"))
    let real = VPNSnapshot(status: .off, host: "home.example.net:51820")
    XCTAssertEqual(VPNFlow.toggleTitle(real), VPNCopy.toggle)
    XCTAssertEqual(VPNFlow.consentMessage(real), VPNCopy.consent(host: "home.example.net:51820"))
    XCTAssertEqual(VPNFlow.consentTitle(real), VPNCopy.consentTitle)
    XCTAssertNil(VPNFlow.codeNote(real))
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

// MARK: DemoVPN under overlapping calls

final class DemoVPNOverlapTests: XCTestCase {
  /// On while on changes nothing: no second "Connecting…", the same since.
  func testOnWhileOnChangesNothing() async {
    let vpn = DemoVPN(connectingFor: .zero, now: { Date(timeIntervalSince1970: 1_000) })
    await vpn.acceptConsent()
    await vpn.setOn(true)
    var updates = vpn.updates().makeAsyncIterator()
    _ = await updates.next()
    await vpn.setOn(true)
    await vpn.setOn(false)
    let next = await updates.next()
    XCTAssertEqual(next?.status, .off, "A second on published something.")
  }

  /// On, off, on again while the first is still connecting: the first never lands Connected.
  func testAnEarlierOnNeverFinishesOverALaterOne() async {
    let vpn = DemoVPN(connectingFor: .seconds(3_600), now: { Date(timeIntervalSince1970: 1_000) })
    await vpn.acceptConsent()
    var updates = vpn.updates().makeAsyncIterator()
    _ = await updates.next()
    let first = Task { await vpn.setOn(true) }
    let connecting = await updates.next()
    XCTAssertEqual(connecting?.status, .connecting)
    await vpn.setOn(false)
    let second = Task { await vpn.setOn(true) }
    _ = await updates.next()  // off
    _ = await updates.next()  // connecting again
    first.cancel()  // wakes the first from its sleep
    await first.value
    XCTAssertEqual(vpn.current.status, .connecting, "The first switch-on finished over the second.")
    second.cancel()
    await second.value
    XCTAssertEqual(vpn.current.status, .connected(since: Date(timeIntervalSince1970: 1_000)))
  }

  /// Remove while connecting: the connect gives up.
  func testRemoveStopsAConnect() async {
    let vpn = DemoVPN(connectingFor: .seconds(3_600))
    await vpn.acceptConsent()
    var updates = vpn.updates().makeAsyncIterator()
    _ = await updates.next()
    let on = Task { await vpn.setOn(true) }
    _ = await updates.next()
    await vpn.remove()
    on.cancel()
    await on.value
    XCTAssertEqual(vpn.current, VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true))
  }
}

// MARK: AppModel follows the VPN the library uses

@MainActor
final class AppModelVPNTests: XCTestCase {
  private func snapshot(sample: Bool) -> ConnectionSnapshot {
    var s = ConnectionSnapshot.noLibrary
    s.isSample = sample
    return s
  }

  private func eventually(_ what: String, _ condition: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async {
    for _ in 0..<20_000 {
      if condition() { return }
      await Task.yield()
    }
    XCTFail(what, file: file, line: line)
  }

  func testTheSampleDemoIsFollowedThenForgottenWhenTheSampleGoes() async {
    let demo = DemoVPN(connectingFor: .zero, now: { Date(timeIntervalSince1970: 1_000) })
    let connection = SwitchingConnection(snapshot(sample: true))
    let model = AppModel(services: StubServices.make(connection: connection), scanner: NoScan(), demoVPN: demo)
    XCTAssertTrue(model.vpn.isDemo)
    let run = Task { await model.run() }
    defer { run.cancel() }

    await demo.acceptConsent()
    await demo.setOn(true)
    await eventually("The demo's Connected did not reach the model.") { model.vpn.isOn }
    model.showsVPN = true

    // Another library replaces the sample (as pairing does): this build's VPN, the demo forgotten.
    connection.publish(snapshot(sample: false))
    await eventually("The model still shows the demo.") { !model.vpn.isDemo }
    XCTAssertEqual(model.vpn.status, .notInThisBuild)
    await eventually("The demo was not forgotten with the sample.") { !demo.current.consented && demo.current.status == .off }
    XCTAssertFalse(model.showsVPN)

    // The demo's later changes no longer reach the model.
    await demo.acceptConsent()
    await demo.setOn(true)
    for _ in 0..<200 { await Task.yield() }
    XCTAssertEqual(model.vpn.status, .notInThisBuild, "The demo still reaches the model.")

    // Back to the sample, then Forget: the demo is forgotten again.
    connection.publish(snapshot(sample: true))
    await eventually("The sample's demo did not come back.") { model.vpn.isDemo && model.vpn.isOn }
    model.forgetLibrary()
    await eventually("Forget did not finish.") { model.stage == .welcome && connection.forgot }
    await eventually("Forget left the demo on.") { !demo.current.consented && demo.current.status == .off }
    XCTAssertFalse(model.vpn.isDemo)
  }

  /// A second run() takes the follow over; the first one ending does not stop it.
  func testASecondRunKeepsFollowing() async {
    let demo = DemoVPN(connectingFor: .zero)
    let connection = SwitchingConnection(snapshot(sample: true))
    let model = AppModel(services: StubServices.make(connection: connection), scanner: NoScan(), demoVPN: demo)
    let first = Task { await model.run() }
    for _ in 0..<50 { await Task.yield() }
    let second = Task { await model.run() }
    for _ in 0..<50 { await Task.yield() }
    first.cancel()
    await first.value
    defer { second.cancel() }
    await demo.acceptConsent()
    await demo.setOn(true)
    await eventually("The first run's end stopped the second's follow.") { model.vpn.isOn }
  }
}

/// A connection whose snapshot the test changes.
private final class SwitchingConnection: ConnectionService, @unchecked Sendable {
  private let lock = NSLock()
  private var snapshot: ConnectionSnapshot
  private var listeners: [UUID: AsyncStream<ConnectionSnapshot>.Continuation] = [:]
  private var didForget = false
  var forgot: Bool { lock.withLock { didForget } }

  init(_ start: ConnectionSnapshot) { snapshot = start }

  var current: ConnectionSnapshot { lock.withLock { snapshot } }

  func publish(_ s: ConnectionSnapshot) {
    // Told outside the lock: a follower's cancel takes it in onTermination (see DemoVPN).
    let list = lock.withLock {
      snapshot = s
      return Array(listeners.values)
    }
    list.forEach { $0.yield(s) }
  }

  func updates() -> AsyncStream<ConnectionSnapshot> {
    AsyncStream { c in
      let id = UUID()
      let first = lock.withLock {
        listeners[id] = c
        return snapshot
      }
      c.yield(first)
      c.onTermination = { [weak self] _ in self?.lock.withLock { _ = self?.listeners.removeValue(forKey: id) } }
    }
  }

  func identify(code: String) async throws -> LibraryIdentity { throw MediaFailure.synologyUnreachable }
  func pair(code: String, confirmed: LibraryIdentity) -> AsyncThrowingStream<PairingEvent, Error> { AsyncThrowingStream { $0.finish() } }
  func retry() async {}
  func forget() async {
    lock.withLock { didForget = true }
    publish(.noLibrary)
  }
}

private struct NoScan: ScanSurfaceProviding {
  func surface(onCode: @escaping (String) -> Void) -> AnyView { AnyView(EmptyView()) }
}

/// Services that answer nothing, around the test's connection.
private enum StubServices {
  static func make(connection: ConnectionService) -> Services {
    Services(library: EmptyStore(), images: NoImages(), search: NoSearch(), connection: connection, sync: NoSync(),
             clips: NoClips(), saver: NoSaver())
  }

  struct EmptyStore: LibraryStore {
    func list(in scope: LibraryScope) async throws -> any MediaList { ArrayMediaList([]) }
    func folders(under parent: Folder?) async throws -> [Folder] { [] }
    func people() async throws -> [Person] { [] }
    func details(for item: MediaItem) async throws -> MediaDetails { throw MediaFailure.synologyUnreachable }
    func preview(of folder: Folder, count: Int) async -> FolderPreview { .empty }
  }

  struct NoImages: ImageLadder {
    func cachedImage(for id: MediaID) -> UIImage? { nil }
    func thumbnail(for item: MediaItem) async -> UIImage? { nil }
    func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> { AsyncThrowingStream { $0.finish() } }
    func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {}
  }

  struct NoSearch: SearchService {
    func capability() async -> SearchCapability { .ready }
    func recents() async -> [String] { [] }
    func remember(_ query: String) async {}
    func forget(_ query: String) async {}
    func forgetAll() async {}
    func search(_ query: String, filters: SearchFilters) async throws -> [SearchHit] { [] }
    func filterOptions() async -> SearchFilterOptions { SearchFilterOptions(people: [], places: [], dates: [], cameras: []) }
    func isWarmingUp() async -> Bool { false }
  }

  struct NoSync: CatalogueSync {
    var continuesAfterLeaving: Bool { false }
    func firstCopy(overMobileData: Bool) -> AsyncThrowingStream<CopyEvent, Error> { AsyncThrowingStream { $0.finish() } }
  }

  struct NoClips: ClipService {
    func route(for item: MediaItem) async -> ClipRoute { .unavailable(.originalGone) }
    func moments(for item: MediaItem) async -> [Moment] { [] }
    func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage? { nil }
    func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> { AsyncThrowingStream { $0.finish() } }
  }

  struct NoSaver: PhotoSaver {
    func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> { AsyncThrowingStream { $0.finish() } }
    func shareable(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> { AsyncThrowingStream { $0.finish() } }
  }
}
