// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the Import chrome's first test target. Headless: the pieces of the
// model that need no mv.import.1 table (a table-driven test, like AIChrome's
// SearchModelTests, is the next step).
//   cd src.swift/ImportChrome && swift test
import XCTest
@testable import ImportChrome

final class ImportModelTests: XCTestCase {
  func testDayLabelReadsAnIsoDay() {
    let label = ImportModel.dayLabel("2026-09-21")
    XCTAssertNotEqual(label, "2026-09-21")
    XCTAssertTrue(label.contains("2026"), label)
    XCTAssertTrue(label.contains("21"), label)
  }

  func testDayLabelKeepsWhatItCannotParse() {
    XCTAssertEqual(ImportModel.dayLabel("unknown"), "unknown")
    XCTAssertEqual(ImportModel.dayLabel(""), "")
  }

  func testParseTakesTheEnginesJson() throws {
    XCTAssertNil(parse(nil))
    XCTAssertNil(parse("{not json"))
    let object = try XCTUnwrap(parse(#"{"selected_files": 3, "days": ["2026-09-21"]}"#) as? [String: Any])
    XCTAssertEqual(object["selected_files"] as? Int, 3)
    XCTAssertEqual(object["days"] as? [String], ["2026-09-21"])
  }
}
