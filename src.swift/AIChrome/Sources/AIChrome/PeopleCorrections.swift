// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Correcting people (2026-11-02, docs/design/17 "Correcting people"): what the
// person sheet, the unknown faces sheet and their undo work from, read from the
// pack's JSON (person_review_json, faces_reject_json, person_suggest_json,
// unknown_faces_json). Pure values: no table, no view, nothing logged (rule 6).
import Foundation

private func jsonDouble(_ v: Any?, _ fallback: Double = 0) -> Double {
  (v as? NSNumber)?.doubleValue ?? fallback
}

private func jsonID(_ v: Any?) -> UInt64 { UInt64(clamping: int64(v)) }

private func jsonBox(_ v: Any?) -> [Double] { (v as? [NSNumber] ?? []).map(\.doubleValue) }

/// One face in a person's sheet: person_review_json's row, or person_faces_json's
/// (an older pack) with nothing to say about doubt.
struct ReviewFace: Identifiable, Equatable {
  let id: UInt64
  let path: String
  let ptsMs: Int64
  let box: [Double]
  var score: Double = 0
  var pinned = false
  /// -1: unknown (an older pack): "Clearest" keeps the pack's order.
  var quality: Double = -1
  var support: Double = -1
  var rival: Double = -1
  var rivalID: UInt64 = 0
  var rivalName = ""
  /// Worth the user's look: the "Check these" group.
  var check = false
  var doubtfulPin = false

  /// Under a face in "Check these": why it is there.
  var caption: String {
    if !rivalName.isEmpty { return "Looks like \(rivalName)?" }
    if doubtfulPin { return "Pinned, but unlike the others" }
    return "Not sure"
  }

  /// person_review_json: [{"face","path","pts_ms","box","score","pinned",...}].
  static func parseReview(_ text: String?) -> [ReviewFace]? {
    guard let rows = parseJSON(text) as? [[String: Any]] else { return nil }
    return rows.map {
      ReviewFace(id: jsonID($0["face"]), path: $0["path"] as? String ?? "",
                 ptsMs: ($0["pts_ms"] as? NSNumber)?.int64Value ?? -1, box: jsonBox($0["box"]),
                 score: jsonDouble($0["score"]), pinned: $0["pinned"] as? Bool ?? false,
                 quality: jsonDouble($0["quality"], -1), support: jsonDouble($0["support"], -1),
                 rival: jsonDouble($0["rival"], -1), rivalID: jsonID($0["rival_id"]),
                 rivalName: $0["rival_name"] as? String ?? "", check: $0["check"] as? Bool ?? false,
                 doubtfulPin: $0["doubtful_pin"] as? Bool ?? false)
    }
  }

  /// person_faces_json (a pack without person_review_json).
  static func parsePlain(_ text: String?) -> [ReviewFace] {
    (parseJSON(text) as? [[String: Any]] ?? []).map {
      ReviewFace(id: jsonID($0["face"]), path: $0["path"] as? String ?? "",
                 ptsMs: ($0["pts_ms"] as? NSNumber)?.int64Value ?? -1, box: jsonBox($0["box"]))
    }
  }
}

/// The sheet's order: the pack's (doubtful first), or the clearest first.
enum ReviewSort: String, CaseIterable, Identifiable {
  case doubtful, clearest
  var id: String { rawValue }
  var label: String { self == .doubtful ? "Doubtful first" : "Clearest" }

  /// Stable: equal quality keeps the pack's order.
  func sorted(_ faces: [ReviewFace]) -> [ReviewFace] {
    switch self {
    case .doubtful: return faces
    case .clearest:
      return faces.enumerated().sorted { a, b in
        a.element.quality != b.element.quality ? a.element.quality > b.element.quality : a.offset < b.offset
      }.map(\.element)
    }
  }

  /// "Check these" (check = true) and the rest, each in this order.
  func groups(_ faces: [ReviewFace]) -> (check: [ReviewFace], rest: [ReviewFace]) {
    let all = sorted(faces)
    return (all.filter(\.check), all.filter { !$0.check })
  }
}

/// What faces_reject_json says became of the rejected faces.
struct RejectOutcome: Equatable {
  struct Destination: Equatable {
    let id: UInt64
    let name: String
    let faces: Int
  }
  /// A face that left, frames of the same clip included: where it was, and its pin.
  struct Left: Equatable {
    let face: UInt64
    let from: UInt64
    let pinned: Bool
  }
  var unassigned = 0
  /// Frames of the same clip that went with them.
  var spread = 0
  var to: [Destination] = []
  /// Every face that left (a pack before 2026-11-02's "faces" gives none).
  var left: [Left] = []

  /// {"unassigned":N,"spread":N,"to":[{"id":7,"name":"Ben","faces":2}],
  ///  "faces":[{"face":9,"from":3,"to":7,"pinned":false}]}
  static func parse(_ text: String?) -> RejectOutcome? {
    guard let obj = parseJSON(text) as? [String: Any] else { return nil }
    return RejectOutcome(
      unassigned: Int(int64(obj["unassigned"])), spread: Int(int64(obj["spread"])),
      to: (obj["to"] as? [[String: Any]] ?? []).map {
        Destination(id: jsonID($0["id"]), name: $0["name"] as? String ?? "", faces: Int(int64($0["faces"])))
      },
      left: (obj["faces"] as? [[String: Any]] ?? []).map {
        Left(face: jsonID($0["face"]), from: jsonID($0["from"]), pinned: ($0["pinned"] as? Bool) ?? false)
      })
  }

  /// Undo: every face back where it was, with its pin, the clip's frames
  /// that went too included. Nil when the pack listed none.
  var undo: PeopleUndo? {
    guard !left.isEmpty else { return nil }
    var groups: [AssignGroup] = []
    for (key, items) in Dictionary(grouping: left, by: { "\($0.from):\($0.pinned)" }).sorted(by: { $0.key < $1.key }) {
      _ = key
      groups.append(AssignGroup(faces: items.map(\.face), person: items[0].from, pin: items[0].pinned))
    }
    return .assign(groups)
  }

  /// "2 now with Carl, 1 unknown, 3 frames of the same clip went too".
  var summary: String {
    var parts = to.filter { $0.faces > 0 }.map {
      "\($0.faces) now with \($0.name.isEmpty ? "someone unnamed" : $0.name)"
    }
    if unassigned > 0 { parts.append("\(unassigned) unknown") }
    if parts.isEmpty { parts.append("Removed") }
    if spread > 0 {
      parts.append(spread == 1 ? "1 frame of the same clip went too" : "\(spread) frames of the same clip went too")
    }
    return parts.joined(separator: ", ")
  }
}

/// "Moved 2 to Carl".
func movedSummary(_ count: Int, to name: String) -> String {
  "Moved \(count) to \(name.isEmpty ? "someone unnamed" : name)"
}

/// A faces_assign call that puts faces back.
struct AssignGroup: Equatable {
  let faces: [UInt64]
  let person: UInt64
  let pin: Bool
}

/// The last action in a sheet, and how to put it back.
enum PeopleUndo: Equatable {
  /// faces_assign each group.
  case assign([AssignGroup])
  /// person_merge_undo each token, last first.
  case merge([UInt64])
  /// faces_pin these faces back to `pinned`.
  case pin([UInt64], Bool)

  /// Faces the user said Yes to that were nobody's: back to nobody.
  static func unknownAgain(_ faces: [UInt64]) -> PeopleUndo {
    .assign([AssignGroup(faces: faces, person: 0, pin: false)])
  }

  /// Faces back to `person`, each with the pin it had: one call per pin state.
  static func restoring(_ faces: [ReviewFace], to person: UInt64) -> PeopleUndo {
    let pinned = faces.filter(\.pinned).map(\.id), loose = faces.filter { !$0.pinned }.map(\.id)
    var groups: [AssignGroup] = []
    if !loose.isEmpty { groups.append(AssignGroup(faces: loose, person: person, pin: false)) }
    if !pinned.isEmpty { groups.append(AssignGroup(faces: pinned, person: person, pin: true)) }
    return .assign(groups)
  }
}

/// "Might also be <name>": person_suggest_json.
struct PersonSuggestions: Equatable {
  struct SuggestedPerson: Identifiable, Equatable {
    let id: UInt64
    let score: Double
    let faces: Int
    let coverFace: UInt64
    let coverBox: [Double]
  }
  struct SuggestedFace: Identifiable, Equatable {
    let id: UInt64
    let path: String
    let ptsMs: Int64
    let box: [Double]
    let score: Double
  }
  var people: [SuggestedPerson] = []
  var faces: [SuggestedFace] = []
  var isEmpty: Bool { people.isEmpty && faces.isEmpty }

  static func parse(_ text: String?) -> PersonSuggestions {
    guard let obj = parseJSON(text) as? [String: Any] else { return PersonSuggestions() }
    return PersonSuggestions(
      people: (obj["people"] as? [[String: Any]] ?? []).map {
        SuggestedPerson(id: jsonID($0["id"]), score: jsonDouble($0["score"]), faces: Int(int64($0["faces"])),
                        coverFace: jsonID($0["cover_face"]), coverBox: jsonBox($0["cover_box"]))
      },
      faces: (obj["faces"] as? [[String: Any]] ?? []).map {
        SuggestedFace(id: jsonID($0["face"]), path: $0["path"] as? String ?? "",
                      ptsMs: ($0["pts_ms"] as? NSNumber)?.int64Value ?? -1, box: jsonBox($0["box"]),
                      score: jsonDouble($0["score"]))
      })
  }
}

/// unknown_faces_json: a page of faces filed under nobody, clearest first.
struct UnknownFace: Identifiable, Equatable {
  let id: UInt64
  let path: String
  let ptsMs: Int64
  let box: [Double]
  let quality: Double
  /// The person it most resembles (0: nobody near).
  let suggest: UInt64
  let suggestName: String
  let score: Double

  var caption: String {
    guard suggest != 0 else { return "" }
    return suggestName.isEmpty ? "Someone unnamed?" : "\(suggestName)?"
  }
}

struct UnknownPage: Equatable {
  var total = 0
  var faces: [UnknownFace] = []

  static func parse(_ text: String?) -> UnknownPage? {
    guard let obj = parseJSON(text) as? [String: Any] else { return nil }
    return UnknownPage(
      total: Int(int64(obj["total"])),
      faces: (obj["faces"] as? [[String: Any]] ?? []).map {
        UnknownFace(id: jsonID($0["face"]), path: $0["path"] as? String ?? "",
                    ptsMs: ($0["pts_ms"] as? NSNumber)?.int64Value ?? -1, box: jsonBox($0["box"]),
                    quality: jsonDouble($0["quality"]), suggest: jsonID($0["suggest"]),
                    suggestName: $0["suggest_name"] as? String ?? "", score: jsonDouble($0["score"]))
      })
  }
}

/// "Move to…": the people a face can go to, named first, matching `query`
/// (a name containing it; unnamed people only while the query is empty).
func moveCandidates(_ people: [Person], excluding: UInt64?, query: String, limit: Int = 200) -> [Person] {
  let q = query.trimmingCharacters(in: .whitespacesAndNewlines)
  let others = people.filter { $0.id != excluding }
  let named = others.filter { !$0.name.isEmpty && (q.isEmpty || $0.name.localizedCaseInsensitiveContains(q)) }
    .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
  let unnamed = q.isEmpty ? others.filter { $0.name.isEmpty } : []
  return Array((named + unnamed).prefix(limit))
}

/// What VoiceOver reads for a face: the file it is in, and the moment for a clip.
func faceAccessibilityLabel(path: String, ptsMs: Int64) -> String {
  let name = (path as NSString).lastPathComponent
  guard ptsMs >= 0 else { return name.isEmpty ? "Face" : "Face in \(name)" }
  let s = ptsMs / 1000
  let at = s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
                     : String(format: "%d:%02d", s / 60, s % 60)
  return name.isEmpty ? "Face at \(at)" : "Face in \(name) at \(at)"
}
