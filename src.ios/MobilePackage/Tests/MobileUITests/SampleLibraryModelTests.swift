// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §11.1: the sample library says what it is wherever the source is named.
import MobileKit
@testable import MobileUI
import XCTest

final class SampleLibraryModelTests: XCTestCase {
  func testTheSampleIsNamedWhereTheSynologyWouldBe() {
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.synologyUnreachable, sample: true), FailureCopy.sampleUnreachable)
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.synologyUnreachable), FailureCopy.synologyUnreachable)
    XCTAssertFalse(FailureCopy.line(for: CopyFailure.synologyUnreachable, sample: true).contains("Synology"))
    XCTAssertFalse(FailureCopy.line(for: CopyFailure.catalogueRejected, sample: true).contains("computer"))
    XCTAssertTrue(FailureCopy.enterCodeHelp.contains(FailureCopy.sampleCode))
    XCTAssertEqual(FailureCopy.sampleCode, "SAMPLE")
  }

  func testAnIdentityFromAComputerIsNotTheSample() {
    XCTAssertFalse(LibraryIdentity(name: "Family", fingerprint: "7F3A 92C1").isSample)
    XCTAssertTrue(LibraryIdentity(name: "Sample library", fingerprint: "1234 5678", isSample: true).isSample)
    XCTAssertFalse(ConnectionSnapshot.noLibrary.isSample)
  }

  /// mediaviewer://sample is opened as a pair link is: never over a pairing or a first copy.
  @MainActor
  func testTheSampleLinkFollowsThePairLinkRules() {
    XCTAssertEqual(AppModel.pairLinkAction(stage: .welcome, isScanning: false, hasLibrary: false), .open)
    XCTAssertEqual(AppModel.pairLinkAction(stage: .firstCopy, isScanning: false, hasLibrary: true), .ignore)
    XCTAssertEqual(AppModel.pairLinkAction(stage: .main, isScanning: false, hasLibrary: true), .confirm)
  }
}

/// Entering SAMPLE with a library paired asks before replacing it, by the library's name.
final class SampleReplacesTests: XCTestCase {
  private func snapshot(library: Bool, sample: Bool, name: String) -> ConnectionSnapshot {
    var s = ConnectionSnapshot(hasLibrary: library, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    s.isSample = sample
    s.libraryName = name
    return s
  }

  @MainActor
  func testAPairedLibraryIsNamedBeforeItIsReplaced() {
    XCTAssertEqual(AppModel.sampleWouldReplace(snapshot(library: true, sample: false, name: "Family")), "Family")
    XCTAssertEqual(AppModel.sampleWouldReplace(snapshot(library: true, sample: false, name: "")), "your library")
  }

  @MainActor
  func testNothingToLoseAsksNothing() {
    XCTAssertNil(AppModel.sampleWouldReplace(snapshot(library: false, sample: false, name: "")))
    XCTAssertNil(AppModel.sampleWouldReplace(snapshot(library: true, sample: true, name: "Sample library")))
  }
}
