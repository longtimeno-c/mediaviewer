// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §12 "Lost phone": the optional Face ID or passcode lock (AppLock.swift).
import MobileKit
import SwiftUI
@testable import MobileUI
import XCTest

@MainActor
final class AppLockTests: XCTestCase {
  private var defaults: UserDefaults!
  private let suite = "AppLockTests"

  override func setUp() {
    defaults = UserDefaults(suiteName: suite)
    defaults.removePersistentDomain(forName: suite)
  }

  override func tearDown() { defaults.removePersistentDomain(forName: suite) }

  private func makeLock(_ owner: FakeOwner, hasLibrary: Bool = true, required: Bool = false) -> AppLock {
    AppLock(owner: owner, defaults: defaults, hasLibrary: hasLibrary, required: required)
  }

  func testOffItNeverLocksOrCovers() {
    let lock = makeLock(FakeOwner())
    XCTAssertFalse(lock.isOn)
    lock.sceneChanged(to: .inactive)
    lock.sceneChanged(to: .background)
    XCTAssertFalse(lock.covers)
  }

  func testTurningOnNeedsTheOwnerFirst() async {
    let owner = FakeOwner(passes: false)
    let lock = makeLock(owner)
    let failed = await lock.turnOn()
    XCTAssertFalse(failed)
    XCTAssertFalse(lock.isOn)
    XCTAssertEqual(owner.reasons, [LockCopy.turnOnReason])
    owner.passes = true
    let passed = await lock.turnOn()
    XCTAssertTrue(passed)
    XCTAssertTrue(lock.isOn)
    XCTAssertFalse(lock.isLocked, "Turning it on does not lock the person out there and then.")
  }

  func testAPhoneWithNoPasscodeCannotTurnItOn() async {
    let owner = FakeOwner(kind: nil)
    let lock = makeLock(owner)
    let turnedOn = await lock.turnOn()
    XCTAssertFalse(turnedOn)
    XCTAssertTrue(owner.reasons.isEmpty)
  }

  func testLeavingLocksAndComingBackAsksOnce() async {
    let owner = FakeOwner()
    let lock = makeLock(owner)
    await lock.turnOn()
    owner.passes = false

    lock.sceneChanged(to: .inactive)
    XCTAssertTrue(lock.covers, "The app switcher's picture must show nothing.")
    XCTAssertFalse(lock.isLocked, "Only covered: a glance at Control Center does not lock.")
    lock.sceneChanged(to: .active)
    XCTAssertFalse(lock.covers)

    lock.sceneChanged(to: .background)
    XCTAssertTrue(lock.isLocked)
    await lock.sceneChanged(to: .active)?.value
    XCTAssertTrue(lock.isLocked, "A failed check stays locked.")
    XCTAssertEqual(owner.reasons.last, LockCopy.unlockReason)

    // Cancelling the check makes the app inactive then active again: it is not asked again by itself.
    let asked = owner.reasons.count
    lock.sceneChanged(to: .inactive)
    XCTAssertNil(lock.sceneChanged(to: .active))
    XCTAssertEqual(owner.reasons.count, asked)

    // The lock screen's button asks.
    owner.passes = true
    await lock.unlock()
    XCTAssertFalse(lock.isLocked)
    XCTAssertFalse(lock.covers)
  }

  func testTheSystemsOwnCheckDoesNotCover() async {
    let owner = FakeOwner()
    let lock = makeLock(owner)
    await lock.turnOn()
    lock.sceneChanged(to: .background)
    owner.onCheck = { _ = lock.sceneChanged(to: .inactive) }
    await lock.sceneChanged(to: .active)?.value
    XCTAssertFalse(lock.isLocked)
    XCTAssertFalse(lock.covers, "The check's own inactive covered the app.")
  }

  func testItIsRememberedAndLockedAtLaunch() async {
    await makeLock(FakeOwner()).turnOn()
    let relaunched = makeLock(FakeOwner())
    XCTAssertTrue(relaunched.isOn)
    XCTAssertTrue(relaunched.isLocked)
    XCTAssertTrue(relaunched.covers)
  }

  func testWithNoLibraryThereIsNothingToLock() async {
    await makeLock(FakeOwner()).turnOn()
    let relaunched = makeLock(FakeOwner(), hasLibrary: false)
    XCTAssertFalse(relaunched.isOn)
    XCTAssertFalse(relaunched.covers)
    XCTAssertFalse(makeLock(FakeOwner()).isOn, "It stays off once a library comes back.")
  }

  func testTurningOffOpensAndForgets() async {
    let lock = makeLock(FakeOwner())
    await lock.turnOn()
    lock.sceneChanged(to: .background)
    lock.turnOff()
    XCTAssertFalse(lock.covers)
    XCTAssertFalse(makeLock(FakeOwner()).isOn)
  }

  /// Forget takes the lock with the library: the next library starts unlocked, with it off.
  func testForgettingTheLibraryTurnsItOff() async {
    let paired = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    let connection = SwitchingConnection(paired)
    let lock = makeLock(FakeOwner())
    await lock.turnOn()
    let model = AppModel(services: StubServices.make(connection: connection), scanner: NoScan(), lock: lock)
    model.forgetLibrary()
    for _ in 0..<20_000 where model.stage != .welcome { await Task.yield() }
    XCTAssertEqual(model.stage, .welcome)
    XCTAssertFalse(lock.isOn)
    XCTAssertFalse(makeLock(FakeOwner()).isOn)
  }

  // MARK: Required by the library's owner

  func testTheOwnersLockIsOnAtLaunchAndCannotBeTurnedOff() {
    let lock = makeLock(FakeOwner(), required: true)
    XCTAssertTrue(lock.isOn)
    XCTAssertFalse(lock.isChosen)
    XCTAssertTrue(lock.isLocked, "A phone kept offline still locks: the pairing kept the owner's word.")
    lock.turnOff()
    XCTAssertTrue(lock.isOn)
    XCTAssertTrue(lock.isLocked)
  }

  func testNewlyRequiredItLocksOnLeavingNotUnderThePerson() async {
    let lock = makeLock(FakeOwner(passes: false))
    lock.setRequired(true)
    XCTAssertTrue(lock.isOn)
    XCTAssertFalse(lock.covers)
    lock.sceneChanged(to: .background)
    XCTAssertTrue(lock.isLocked)
    await lock.sceneChanged(to: .active)?.value
    XCTAssertTrue(lock.isLocked)
  }

  func testNoLongerRequiredItOpensUnlessThePersonChoseIt() async {
    let lock = makeLock(FakeOwner(), required: true)
    lock.setRequired(false)
    XCTAssertFalse(lock.isOn)
    XCTAssertFalse(lock.covers)

    let chosen = makeLock(FakeOwner())
    await chosen.turnOn()
    chosen.setRequired(true)
    chosen.sceneChanged(to: .background)
    chosen.setRequired(false)
    XCTAssertTrue(chosen.isOn)
    XCTAssertTrue(chosen.isLocked, "The person's own lock stays.")
  }

  func testTheOwnersWordIsNotTheStoredChoice() async {
    let lock = makeLock(FakeOwner(), required: true)
    // Opting in on top of a required lock is still the person's own choice, kept for later.
    let chose = await lock.turnOn()
    XCTAssertTrue(chose)
    XCTAssertTrue(makeLock(FakeOwner()).isChosen)
    lock.forget()
    XCTAssertFalse(lock.isOn)
    XCTAssertFalse(lock.covers)
    XCTAssertFalse(makeLock(FakeOwner()).isOn)
  }

  func testWithNoLibraryTheOwnersWordIsIgnored() {
    XCTAssertFalse(makeLock(FakeOwner(), hasLibrary: false, required: true).isOn)
  }

  /// The owner's word reaches the lock through the connection, and Forget drops it.
  func testTheConnectionCarriesTheOwnersWord() async {
    var required = ConnectionSnapshot(hasLibrary: true, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    required.lockRequired = true
    let connection = SwitchingConnection(required)
    let lock = makeLock(FakeOwner(), required: required.lockRequired)
    let model = AppModel(services: StubServices.make(connection: connection), scanner: NoScan(), lock: lock)
    let run = Task { await model.run() }
    defer { run.cancel() }
    XCTAssertTrue(lock.isRequired)
    var free = required
    free.lockRequired = false
    connection.publish(free)
    for _ in 0..<20_000 where lock.isRequired { await Task.yield() }
    XCTAssertFalse(lock.isRequired)
    connection.publish(required)
    for _ in 0..<20_000 where !lock.isRequired { await Task.yield() }
    XCTAssertTrue(lock.isRequired)
    model.forgetLibrary()
    for _ in 0..<20_000 where model.stage != .welcome { await Task.yield() }
    XCTAssertFalse(lock.isOn)
  }

  func testTheWordsNameTheCheck() {
    XCTAssertEqual(LockCopy.toggle(.faceID), "Require Face ID")
    XCTAssertEqual(LockCopy.toggle(.passcode), "Require passcode")
    XCTAssertEqual(LockCopy.unlock(.touchID), "Unlock with Touch ID")
    XCTAssertEqual(LockCopy.unlock(.passcode), "Unlock")
    XCTAssertEqual(LockCopy.unlock(nil), "Unlock")
    XCTAssertTrue(LockCopy.required.contains("owner"))
    XCTAssertTrue(LockCopy.requiredNoPasscode.contains("passcode"))
  }
}

/// Called only from the main actor, as AppLock is.
private final class FakeOwner: OwnerCheck, @unchecked Sendable {
  var passes: Bool
  let checkKind: OwnerCheckKind?
  var reasons: [String] = []
  /// Runs while the check is "on screen".
  var onCheck: (@MainActor () -> Void)?

  init(passes: Bool = true, kind: OwnerCheckKind? = .faceID) {
    self.passes = passes
    checkKind = kind
  }

  func kind() -> OwnerCheckKind? { checkKind }

  func check(reason: String) async -> Bool {
    reasons.append(reason)
    if let onCheck { await onCheck() }
    return passes
  }
}
