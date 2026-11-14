// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "This computer" → Drives (docs/plans/self-host-drives.md §6): the pure half of the
// card's Drives step. The link's state_json carries the driftwire program's own
// `host drives list --json` ("drives") and `host drives choices --json`
// ("drives_choices"); this reads them and holds the rules the card enforces before
// it sends anything (the typed names, the combined drive's name). Foundation only:
// the test target compiles this same file (a link in Tests/), since it cannot
// import the chrome. The Windows twin is PhoneDrives.cs.
import Foundation

enum PhoneDrives {
  struct Volume: Equatable {
    var label = ""
    var size: Int64 = 0
    var used: Int64 = 0
    var folders: Int64 = 0
    var mounted = false
  }

  struct Disk: Equatable, Identifiable {
    var id = ""
    var name = ""   // what the person types to erase it (unique in the list)
    var model = ""
    var size: Int64 = 0
    var used: Int64 = 0
    var folders: Int64 = 0
    var facts = ""  // one line for display
    var system = false
    var `internal` = false
    var image = false
    var smart = ""
    var refused = ""  // why it cannot be erased; empty when it can
    var volumes: [Volume] = []
  }

  struct Member: Equatable {
    var id = ""
    var status = ""
    var missing = false
  }

  struct DriveSet: Equatable, Identifiable {
    var id = ""
    var name = ""
    var kind = ""   // mirror | span | stripe | parity
    var state = ""  // healthy | rebuilding | degraded | offline | failing
    var percent: Int64 = 0
    var size: Int64 = 0
    var line = ""
    var members: [Member] = []
    /// A mirror (or parity set) that lost a member can take a new drive; a stripe or span cannot.
    var canTakeADrive: Bool {
      (kind == "mirror" || kind == "parity") && (state == "degraded" || state == "failing")
    }
  }

  struct Listing: Equatable {
    var os = ""
    var poolOK = false
    var error = ""
    var notABackup = ""
    var disks: [Disk] = []
    var sets: [DriveSet] = []
  }

  struct Option: Equatable, Identifiable {
    var choice = ""
    var title = ""
    var offered = false
    var whyNot = ""
    var recommended = false
    var warning = false
    var space: Int64 = 0
    var spaceWords = ""
    var deadWords = ""
    var speedWords = ""
    var id: String { choice }
    /// Every choice but Separate erases the drives.
    var erases: Bool { choice != "separate" }
  }

  struct Choices: Equatable {
    var notABackup = ""
    var options: [Option] = []
  }

  private static func str(_ o: [String: Any], _ k: String) -> String { o[k] as? String ?? "" }
  private static func int(_ o: [String: Any], _ k: String) -> Int64 { (o[k] as? NSNumber)?.int64Value ?? 0 }
  private static func bool(_ o: [String: Any], _ k: String) -> Bool { o[k] as? Bool ?? false }

  static func listing(_ any: Any?) -> Listing? {
    guard let o = any as? [String: Any] else { return nil }
    var l = Listing()
    l.os = str(o, "os")
    l.poolOK = bool(o, "pool_ok")
    l.error = str(o, "error")
    l.notABackup = str(o, "not_a_backup")
    l.disks = (o["disks"] as? [[String: Any]] ?? []).compactMap { d in
      guard let id = d["id"] as? String, !id.isEmpty else { return nil }
      var k = Disk()
      k.id = id
      k.name = str(d, "name")
      k.model = str(d, "model")
      k.size = int(d, "size")
      k.used = int(d, "used")
      k.folders = int(d, "folders")
      k.facts = str(d, "facts")
      k.system = bool(d, "system")
      k.internal = bool(d, "internal")
      k.image = bool(d, "image")
      k.smart = str(d, "smart")
      k.refused = str(d, "refused")
      k.volumes = (d["volumes"] as? [[String: Any]] ?? []).map { v in
        Volume(label: str(v, "label"), size: int(v, "size"), used: int(v, "used"),
               folders: int(v, "folders"), mounted: bool(v, "mounted"))
      }
      return k
    }
    l.sets = (o["sets"] as? [[String: Any]] ?? []).compactMap { e in
      guard let id = e["id"] as? String else { return nil }
      var s = DriveSet()
      s.id = id
      s.name = str(e, "name")
      s.kind = str(e, "kind")
      s.state = str(e, "state")
      s.percent = int(e, "percent")
      s.size = int(e, "size")
      s.line = str(e, "line")
      s.members = (e["members"] as? [[String: Any]] ?? []).map {
        Member(id: str($0, "id"), status: str($0, "status"), missing: bool($0, "missing"))
      }
      return s
    }
    return l
  }

  static func choices(_ any: Any?) -> Choices? {
    guard let o = any as? [String: Any] else { return nil }
    var c = Choices()
    c.notABackup = str(o, "not_a_backup")
    c.options = (o["options"] as? [[String: Any]] ?? []).compactMap { e in
      guard let choice = e["choice"] as? String else { return nil }
      var p = Option()
      p.choice = choice
      p.title = str(e, "title")
      p.offered = bool(e, "offered")
      p.whyNot = str(e, "why_not")
      p.recommended = bool(e, "recommended")
      p.warning = bool(e, "warning")
      p.space = int(e, "space")
      p.spaceWords = str(e, "space_words")
      p.deadWords = str(e, "dead_words")
      p.speedWords = str(e, "speed_words")
      return p
    }
    return c
  }

  /// The guard the program applies again (drives::confirmed): one typed name per drive,
  /// in order, each equal to that drive's name once trimmed.
  static func namesMatch(typed: [String], disks: [Disk]) -> Bool {
    guard !disks.isEmpty, typed.count == disks.count else { return false }
    for (t, d) in zip(typed, disks) {
      if d.name.isEmpty || t.trimmingCharacters(in: .whitespacesAndNewlines) != d.name { return false }
    }
    return true
  }

  /// The combined drive's name (drives::good_label): 1 to 27 ASCII letters, digits,
  /// spaces, '-' or '_', not starting or ending with a space.
  static func goodLabel(_ label: String) -> Bool {
    let u = Array(label.utf8)
    guard !u.isEmpty, u.count <= 27, u.first != 0x20, u.last != 0x20 else { return false }
    return u.allSatisfy { c in
      (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) ||
        c == 0x20 || c == 0x2D || c == 0x5F
    }
  }

  /// The request that erases and combines (or, for Separate, shares each as it is).
  static func combineRequest(choice: String, disks: [Disk], label: String, typed: [String]) -> [String: Any] {
    var r: [String: Any] = ["do": "host_drives_combine", "choice": choice, "ids": disks.map(\.id)]
    if choice != "separate" {
      r["label"] = label.trimmingCharacters(in: .whitespaces)
      r["names"] = typed.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
    }
    return r
  }

  /// The drives that could take a lost member's place: not refused, not already in a set.
  static func replacements(for set: DriveSet, in listing: Listing) -> [Disk] {
    let members = Set(set.members.map(\.id))
    return listing.disks.filter { $0.refused.isEmpty && !members.contains($0.id) }
  }
}
