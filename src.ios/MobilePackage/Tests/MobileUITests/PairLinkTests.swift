// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileUI
import XCTest

@MainActor
final class PairLinkTests: XCTestCase {
  private func action(_ stage: AppModel.Stage, scanning: Bool = false, library: Bool) -> AppModel.PairLinkAction {
    AppModel.pairLinkAction(stage: stage, isScanning: scanning, hasLibrary: library)
  }

  func testNothingPairedOpensIt() {
    XCTAssertEqual(action(.welcome, library: false), .open)
    XCTAssertEqual(action(.main, library: false), .open)
  }

  func testTheScanScreenOpensIt() {
    XCTAssertEqual(action(.main, scanning: true, library: true), .open)
  }

  /// Another app must not cancel a running first copy or a pairing.
  func testNeverInterruptsPairingOrTheFirstCopy() {
    XCTAssertEqual(action(.firstCopy, library: true), .ignore)
    XCTAssertEqual(action(.connect(code: "mediaviewer://pair#x"), library: false), .ignore)
  }

  func testALibraryInUseAsksFirst() {
    XCTAssertEqual(action(.main, library: true), .confirm)
  }
}
