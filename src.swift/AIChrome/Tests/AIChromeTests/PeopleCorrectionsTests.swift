// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Correcting people (docs/design/17 "Reviewing a person"): the pack's JSON read
// into the sheet's values, the reject sentence, the orders, the undo groups.
import XCTest
@testable import AIChrome
import CAiApi

final class PeopleCorrectionsTests: XCTestCase {
  func testReviewParsesAndCaptions() throws {
    let json = """
    [{"face":9,"path":"/a/b.jpg","pts_ms":-1,"box":[0.1,0.2,0.3,0.4],"score":0.9,"pinned":false,
      "quality":0.6,"support":0.42,"rival":0.31,"rival_id":12,"rival_name":"Ben","check":true,"doubtful_pin":false},
     {"face":10,"path":"/a/c.mov","pts_ms":4000,"box":[0,0,1,1],"pinned":true,"quality":0.9,
      "check":true,"doubtful_pin":true,"rival_name":""},
     {"face":11,"path":"/a/d.jpg","pts_ms":-1,"box":[0,0,1,1],"quality":0.2,"check":true},
     {"face":12,"path":"/a/e.jpg","pts_ms":-1,"box":[0,0,1,1],"quality":0.95,"check":false}]
    """
    let faces = try XCTUnwrap(ReviewFace.parseReview(json))
    XCTAssertEqual(faces.map(\.id), [9, 10, 11, 12])
    XCTAssertEqual(faces[0].box, [0.1, 0.2, 0.3, 0.4])
    XCTAssertEqual(faces[0].rivalID, 12)
    XCTAssertEqual(faces[0].caption, "Looks like Ben?")
    XCTAssertTrue(faces[1].pinned)
    XCTAssertEqual(faces[1].ptsMs, 4000)
    XCTAssertEqual(faces[1].caption, "Pinned, but unlike the others")
    XCTAssertEqual(faces[2].caption, "Not sure")
    XCTAssertNil(ReviewFace.parseReview(nil))
    XCTAssertNil(ReviewFace.parseReview("{}"))
  }

  func testPlainFallbackHasNoDoubt() {
    let faces = ReviewFace.parsePlain(#"[{"face":3,"path":"/x.jpg","pts_ms":-1,"box":[0,0,1,1]}]"#)
    XCTAssertEqual(faces.count, 1)
    XCTAssertFalse(faces[0].check)
    XCTAssertEqual(faces[0].quality, -1)
  }

  func testSortsAndGroups() {
    func f(_ id: UInt64, _ q: Double, check: Bool) -> ReviewFace {
      var r = ReviewFace(id: id, path: "", ptsMs: -1, box: [])
      r.quality = q
      r.check = check
      return r
    }
    let faces = [f(1, 0.2, check: true), f(2, 0.9, check: false), f(3, 0.5, check: true), f(4, 0.5, check: false)]
    XCTAssertEqual(ReviewSort.doubtful.sorted(faces).map(\.id), [1, 2, 3, 4])
    XCTAssertEqual(ReviewSort.clearest.sorted(faces).map(\.id), [2, 3, 4, 1])  // ties keep the pack's order
    let g = ReviewSort.doubtful.groups(faces)
    XCTAssertEqual(g.check.map(\.id), [1, 3])
    XCTAssertEqual(g.rest.map(\.id), [2, 4])
    XCTAssertEqual(ReviewSort.clearest.groups(faces).check.map(\.id), [3, 1])
  }

  func testRejectSummary() throws {
    let o = try XCTUnwrap(RejectOutcome.parse(#"{"unassigned":1,"spread":3,"to":[{"id":7,"name":"Carl","faces":2}]}"#))
    XCTAssertEqual(o.to, [.init(id: 7, name: "Carl", faces: 2)])
    XCTAssertEqual(o.summary, "2 now with Carl, 1 unknown, 3 frames of the same clip went too")
    XCTAssertEqual(RejectOutcome(unassigned: 2).summary, "2 unknown")
    XCTAssertEqual(RejectOutcome(spread: 1).summary, "Removed, 1 frame of the same clip went too")
    XCTAssertEqual(RejectOutcome(to: [.init(id: 1, name: "", faces: 1)]).summary, "1 now with someone unnamed")
    XCTAssertNil(RejectOutcome.parse("[]"))
    XCTAssertEqual(movedSummary(2, to: "Carl"), "Moved 2 to Carl")
  }

  func testUndoRestoresEachPinState() {
    var a = ReviewFace(id: 1, path: "", ptsMs: -1, box: []); a.pinned = true
    let b = ReviewFace(id: 2, path: "", ptsMs: -1, box: [])
    let c = ReviewFace(id: 3, path: "", ptsMs: -1, box: [])
    XCTAssertEqual(PeopleUndo.restoring([a, b, c], to: 5),
                   .assign([AssignGroup(faces: [2, 3], person: 5, pin: false),
                            AssignGroup(faces: [1], person: 5, pin: true)]))
    XCTAssertEqual(PeopleUndo.restoring([b], to: 5), .assign([AssignGroup(faces: [2], person: 5, pin: false)]))
  }

  func testSuggestionsAndUnknownParse() throws {
    let s = PersonSuggestions.parse("""
    {"people":[{"id":4,"score":0.4,"faces":6,"cover_face":40,"cover_path":"/p.jpg","cover_ms":-1,"cover_box":[0,0,0.5,0.5]}],
     "faces":[{"face":8,"path":"/q.jpg","pts_ms":-1,"box":[0,0,1,1],"score":0.3}]}
    """)
    XCTAssertEqual(s.people.map(\.id), [4])
    XCTAssertEqual(s.people[0].coverFace, 40)
    XCTAssertEqual(s.people[0].faces, 6)
    XCTAssertEqual(s.faces.map(\.id), [8])
    XCTAssertTrue(PersonSuggestions.parse(nil).isEmpty)

    let page = try XCTUnwrap(UnknownPage.parse("""
    {"total":42,"faces":[{"face":1,"path":"/a.jpg","pts_ms":-1,"box":[0,0,1,1],"quality":0.8,"suggest":7,"suggest_name":"Anna","score":0.27},
                         {"face":2,"path":"/b.jpg","pts_ms":-1,"box":[0,0,1,1],"quality":0.7,"suggest":0,"suggest_name":"","score":0}]}
    """))
    XCTAssertEqual(page.total, 42)
    XCTAssertEqual(page.faces[0].caption, "Anna?")
    XCTAssertEqual(page.faces[1].caption, "")
  }

  func testMoveCandidatesNamedFirst() {
    let people = [Person(id: 1, name: "", faces: 9, coverFace: 0, coverBox: []),
                  Person(id: 2, name: "Zoe", faces: 1, coverFace: 0, coverBox: []),
                  Person(id: 3, name: "anna", faces: 2, coverFace: 0, coverBox: []),
                  Person(id: 4, name: "Carl", faces: 3, coverFace: 0, coverBox: [])]
    XCTAssertEqual(moveCandidates(people, excluding: 4, query: "").map(\.id), [3, 2, 1])
    XCTAssertEqual(moveCandidates(people, excluding: nil, query: "AN").map(\.id), [3])
  }

  func testArrowStepAndBoxRect() {
    XCTAssertEqual(step([5, 6, 7], from: nil, by: 1), 5)
    XCTAssertEqual(step([5, 6, 7], from: nil, by: -1), 7)
    XCTAssertEqual(step([5, 6, 7], from: 7, by: 1), 7)
    XCTAssertEqual(step([5, 6, 7], from: 6, by: -1), 5)
    // A 200x100 picture in a 100x100 frame: 100x50, 25 down.
    let r = boxRect([0.5, 0.5, 0.5, 0.5], image: CGSize(width: 200, height: 100), in: CGSize(width: 100, height: 100))
    XCTAssertEqual(r, CGRect(x: 50, y: 50, width: 50, height: 25))
    XCTAssertNil(boxRect([], image: CGSize(width: 1, height: 1), in: CGSize(width: 1, height: 1)))
  }

  /// An older pack: none of the new entries is reachable.
  func testOldTableHasNoCorrections() {
    let p = UnsafeMutablePointer<mv_ai_api>.allocate(capacity: 1)
    defer { p.deallocate() }
    p.initialize(to: mv_ai_api())
    p.pointee.struct_size = UInt32(MemoryLayout<mv_ai_api>.offset(of: \mv_ai_api.faces_assign)!)
    let t = AITable(UnsafePointer(p))
    XCTAssertFalse(t.canAssign)
    XCTAssertFalse(t.canReview)
    XCTAssertNil(t.facesAssign([1], to: 2, pin: true))
    XCTAssertNil(t.personReviewJSON(1))
    XCTAssertNil(t.unknownFacesJSON(offset: 0, limit: 1))
  }
}
