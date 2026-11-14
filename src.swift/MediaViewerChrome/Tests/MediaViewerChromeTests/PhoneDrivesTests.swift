// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "This computer" → Drives (docs/plans/self-host-drives.md §6). PhoneDrivesModel.swift is
// compiled into this target through a link (this target cannot import the chrome); the
// view's own promises are read as text, as PhoneAccessTests does.
import Foundation
import XCTest

final class PhoneDrivesTests: XCTestCase {
  private func json(_ text: String) throws -> Any {
    try JSONSerialization.jsonObject(with: Data(text.utf8))
  }

  private func source(_ name: String) throws -> String {
    var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    while dir.path != "/" {
      let file = dir.appendingPathComponent("Sources/MediaViewerChrome/\(name)")
      if FileManager.default.fileExists(atPath: file.path) { return try String(contentsOf: file, encoding: .utf8) }
      dir.deleteLastPathComponent()
    }
    throw XCTSkip("\(name) not found")
  }

  func testTheListingReads() throws {
    let o = try json(#"""
    {"os":"mac","pool_ok":true,"not_a_backup":"RAID is not a backup.","disks":[
     {"id":"disk4","name":"Untitled (disk4)","model":"WD","size":2000000000000,"used":5,"folders":2,
      "facts":"Untitled (disk4) · 2 TB · WD","system":false,"internal":false,"image":true,"smart":"verified",
      "volumes":[{"label":"Untitled","size":2000000000000,"used":5,"folders":2,"mounted":true}]},
     {"id":"disk0","name":"Macintosh HD","model":"APPLE SSD","size":1,"used":1,"folders":1,"facts":"x",
      "system":true,"internal":true,"image":false,"refused":"it holds this computer's system","volumes":[]}],
     "sets":[{"id":"ABC","name":"Photos","kind":"mirror","state":"degraded","percent":0,"size":2,
      "line":"Photos (mirror): degraded","members":[{"id":"disk6","status":"Online","missing":false},
      {"id":"","status":"Missing","missing":true}]}]}
    """#)
    let l = try XCTUnwrap(PhoneDrives.listing(o))
    XCTAssertEqual(l.os, "mac")
    XCTAssertTrue(l.poolOK)
    XCTAssertEqual(l.disks.map(\.id), ["disk4", "disk0"])
    XCTAssertEqual(l.disks[0].volumes.first?.label, "Untitled")
    XCTAssertTrue(l.disks[0].volumes.first?.mounted ?? false)
    XCTAssertEqual(l.disks[0].smart, "verified")
    XCTAssertEqual(l.disks[1].refused, "it holds this computer's system")
    XCTAssertEqual(l.sets.first?.line, "Photos (mirror): degraded")
    XCTAssertTrue(l.sets.first?.canTakeADrive ?? false)
    XCTAssertEqual(l.sets.first?.members.count, 2)
    // A refused drive never takes a lost member's place, nor does one already in the set.
    XCTAssertEqual(PhoneDrives.replacements(for: l.sets[0], in: l).map(\.id), ["disk4"])
    XCTAssertNil(PhoneDrives.listing(nil))
  }

  func testTheFiveChoicesRead() throws {
    let o = try json(#"""
    {"not_a_backup":"RAID is not a backup.","options":[
     {"choice":"separate","title":"Separate","offered":true,"recommended":false,"warning":false,"space":4,"space_words":"a","dead_words":"b","speed_words":"c"},
     {"choice":"mirror","title":"Mirror (RAID 1)","offered":true,"recommended":true,"warning":false,"space":2,"space_words":"2 TB","dead_words":"Nothing is lost.","speed_words":"d"},
     {"choice":"span","title":"Span (JBOD)","offered":true,"recommended":false,"warning":false,"space":4,"space_words":"","dead_words":"","speed_words":""},
     {"choice":"stripe","title":"Stripe (RAID 0)","offered":true,"recommended":false,"warning":true,"space":4,"space_words":"","dead_words":"","speed_words":""},
     {"choice":"parity","title":"Parity (RAID 5)","offered":false,"why_not":"Needs 3 or more drives.","recommended":false,"warning":false,"space":0,"space_words":"","dead_words":"","speed_words":""}]}
    """#)
    let c = try XCTUnwrap(PhoneDrives.choices(o))
    XCTAssertEqual(c.notABackup, "RAID is not a backup.")
    XCTAssertEqual(c.options.map(\.choice), ["separate", "mirror", "span", "stripe", "parity"])
    XCTAssertTrue(c.options[1].recommended)
    XCTAssertTrue(c.options[3].warning)
    XCTAssertTrue(c.options[3].offered)  // warned, still offered
    XCTAssertFalse(c.options[4].offered)
    XCTAssertEqual(c.options[4].whyNot, "Needs 3 or more drives.")
    XCTAssertFalse(c.options[0].erases)
    XCTAssertTrue(c.options[1].erases)
  }

  func testEveryTypedNameMustMatchItsDrive() {
    let disks = [PhoneDrives.Disk(id: "disk4", name: "Untitled (disk4)"), PhoneDrives.Disk(id: "disk5", name: "Photos B")]
    XCTAssertTrue(PhoneDrives.namesMatch(typed: ["Untitled (disk4)", "  Photos B\n"], disks: disks))
    XCTAssertFalse(PhoneDrives.namesMatch(typed: ["Untitled (disk4)", "photos b"], disks: disks))
    XCTAssertFalse(PhoneDrives.namesMatch(typed: ["Photos B", "Untitled (disk4)"], disks: disks))  // in order
    XCTAssertFalse(PhoneDrives.namesMatch(typed: ["Untitled (disk4)"], disks: disks))
    XCTAssertFalse(PhoneDrives.namesMatch(typed: [], disks: []))
    XCTAssertFalse(PhoneDrives.namesMatch(typed: [""], disks: [PhoneDrives.Disk(id: "disk9", name: "")]))
  }

  func testTheCombinedDrivesName() {
    XCTAssertTrue(PhoneDrives.goodLabel("Photos"))
    XCTAssertTrue(PhoneDrives.goodLabel("My Photos_2-b"))
    XCTAssertTrue(PhoneDrives.goodLabel(String(repeating: "a", count: 27)))
    XCTAssertFalse(PhoneDrives.goodLabel(String(repeating: "a", count: 28)))
    XCTAssertFalse(PhoneDrives.goodLabel(""))
    XCTAssertFalse(PhoneDrives.goodLabel(" Photos"))
    XCTAssertFalse(PhoneDrives.goodLabel("Photos/2"))
    XCTAssertFalse(PhoneDrives.goodLabel("Fotós"))
  }

  func testTheRequests() {
    let disks = [PhoneDrives.Disk(id: "disk4", name: "A"), PhoneDrives.Disk(id: "disk5", name: "B")]
    let mirror = PhoneDrives.combineRequest(choice: "mirror", disks: disks, label: "Photos ", typed: [" A", "B"])
    XCTAssertEqual(mirror["do"] as? String, "host_drives_combine")
    XCTAssertEqual(mirror["ids"] as? [String], ["disk4", "disk5"])
    XCTAssertEqual(mirror["label"] as? String, "Photos")
    XCTAssertEqual(mirror["names"] as? [String], ["A", "B"])
    let separate = PhoneDrives.combineRequest(choice: "separate", disks: disks, label: "x", typed: [])
    XCTAssertNil(separate["label"])
    XCTAssertNil(separate["names"])
  }

  // The owner's call: five equal cards side by side, the backup line above them, words for every error.
  func testTheCardKeepsTheOwnersCall() throws {
    let view = try source("PhoneDrivesView.swift")
    XCTAssertTrue(view.contains("Text(c.notABackup)"))
    XCTAssertTrue(view.contains("ForEach(c.options) { o in optionCard(o) }"))
    XCTAssertTrue(view.contains("HStack(alignment: .top"))
    XCTAssertTrue(view.contains("words(o.whyNot)"))
    XCTAssertTrue(view.contains(#""If one dies: " + o.deadWords"#))
    XCTAssertTrue(view.contains(#""do": "host_drives_replace""#))
    let card = try source("PhoneAccessView.swift")
    for code in ["host_drives", "host_drives_failed"] {
      XCTAssertTrue(card.contains("case \"\(code)\":"), code)
    }
    XCTAssertTrue(card.contains("PhoneDrivesSection()"))
  }
}
