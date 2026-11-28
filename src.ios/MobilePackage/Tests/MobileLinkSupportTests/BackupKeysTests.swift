// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileLinkSupport
import XCTest

final class BackupKeysTests: XCTestCase {
  func testKnownICloudIdsAreNotAskedAgain() {
    var asked: [[String]] = []
    let keys = BackupKeys.keys(for: ["a", "b"], known: ["a": "cloud-a", "b": "cloud-b"]) { ids in
      asked.append(ids)
      return [:]
    }
    XCTAssertEqual(keys, ["a": "cloud-a", "b": "cloud-b"])
    XCTAssertTrue(asked.isEmpty)
  }

  func testOnlyNewPhotosAndThoseWithoutAnICloudIdAreAsked() {
    var asked: [[String]] = []
    let keys = BackupKeys.keys(for: ["a", "b", "c"], known: ["a": "cloud-a", "b": "local:b"]) { ids in
      asked.append(ids)
      return ["b": "cloud-b"]
    }
    XCTAssertEqual(asked, [["b", "c"]])
    XCTAssertEqual(keys, ["a": "cloud-a", "b": "cloud-b", "c": "local:c"])
  }

  func testNoAnswerKeepsTheLocalKey() {
    let keys = BackupKeys.keys(for: ["a"], known: [:]) { _ in [:] }
    XCTAssertEqual(keys, ["a": "local:a"])
  }
}
