// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model tiers (#482, docs/plans/model-tiers.md T4), as the picker reads
// them: settings_json's "tiers" (the pack's models/tiers.json), "machine" (the
// local check, never sent anywhere) and "tier" (the choice), and the
// downloader's progress from mv_ai_status. Pure logic, no view: the picker
// (TierPicker.swift), the Settings rows and the status line format from here,
// and TierModelTests check it.
import CAiApi
import Foundation

/// The local check: what the picker says it found, and the tier it suggests.
struct TierMachine: Equatable {
  var chip = ""
  var ramBytes: UInt64 = 0
  var accelerator = ""     // "apple" | "nvidia" | ""
  var recommended = ""

  init() {}

  init(_ obj: [String: Any]) {
    chip = obj["chip"] as? String ?? ""
    ramBytes = UInt64(max(0, int64(obj["ram_bytes"])))
    accelerator = obj["accelerator"] as? String ?? ""
    recommended = obj["recommended"] as? String ?? ""
  }

  var accelerated: Bool { !accelerator.isEmpty }

  /// "We checked this Mac: Apple M2 Pro · 16 GB · Neural Engine".
  var line: String {
    var parts: [String] = []
    if !chip.isEmpty { parts.append(chip) }
    if ramBytes > 0 {
      parts.append("\(max(1, Int((Double(ramBytes) / 1_073_741_824).rounded()))) GB")
    }
    parts.append(accelerator == "apple" ? "Neural Engine" : accelerator == "nvidia" ? "NVIDIA GPU" : "CPU only")
    return "We checked this Mac: " + parts.joined(separator: " · ")
  }
}

/// A piece the picker offers a checkbox for. Core (ai) always comes with a
/// tier, and Large pictures (ai-large) with the tier that needs it.
enum TierPiece: String, CaseIterable, Identifiable {
  case people = "ai-faces"
  case sounds = "ai-audio"
  case documents = "ai-docs"

  var id: String { rawValue }
  /// The roles whose models the piece holds.
  var roles: [String] {
    switch self {
    case .people: return ["faces"]
    case .sounds: return ["speech", "sounds"]
    case .documents: return ["documents"]
    }
  }
}

/// One tier of tiers.json "profiles", with its summary.
struct TierOption: Identifiable, Equatable {
  let id: String
  var name = ""
  var emoji = ""
  var forLine = ""
  var models: [String: [String]] = [:]
  var licences: [String] = []
  var downloadBytes: [String: UInt64] = [:]
  var diskBytes: [String: UInt64] = [:]
  var coremlCacheBytes: UInt64 = 0
  /// photos_per_s; nil where it was not measured.
  var photosPerSecondAccelerated: Double?
  var photosPerSecondCPU: Double?

  /// The order the models line follows.
  static let roles = ["pictures", "speech", "sounds", "faces", "documents"]
  /// The pieces a tier always brings: Core, and its large tower if it has one.
  static let basePieces = ["ai", "ai-large"]

  init(id: String, _ obj: [String: Any]) {
    self.id = id
    name = obj["name"] as? String ?? id.capitalized
    emoji = obj["emoji"] as? String ?? ""
    forLine = obj["for"] as? String ?? ""
    let summary = obj["summary"] as? [String: Any] ?? [:]
    for (role, list) in summary["models"] as? [String: Any] ?? [:] {
      models[role] = (list as? [Any] ?? []).compactMap { $0 as? String }
    }
    licences = (summary["licences"] as? [Any] ?? []).compactMap { $0 as? String }
    downloadBytes = Self.bytes(summary["download_bytes"])
    diskBytes = Self.bytes(summary["disk_bytes"])
    // On the Mac the profile's Core ML cache is disk too (summary first,
    // then the profile, as tools/package/ai-models.py copies it).
    coremlCacheBytes = UInt64(max(0, int64(summary["coreml_cache_bytes"] ?? obj["coreml_cache_bytes"])))
    let speed = (summary["photos_per_s"] ?? obj["photos_per_s"]) as? [String: Any] ?? [:]
    photosPerSecondAccelerated = Self.rate(speed["accelerated"])
    photosPerSecondCPU = Self.rate(speed["cpu"])
  }

  private static func bytes(_ v: Any?) -> [String: UInt64] {
    var out: [String: UInt64] = [:]
    for (k, n) in v as? [String: Any] ?? [:] { out[k] = UInt64(max(0, int64(n))) }
    return out
  }

  private static func rate(_ v: Any?) -> Double? {
    guard let n = v as? NSNumber, n.doubleValue > 0 else { return nil }
    return n.doubleValue
  }

  var title: String { emoji.isEmpty ? name : "\(emoji) \(name)" }
  /// The Maxxed tier's ViT-H/14 lives in its own piece (ai-large).
  var needsLarge: Bool { diskBytes["ai-large"] != nil || downloadBytes["ai-large"] != nil }

  /// "CLIP ViT-L/14 · CLIP ViT-B/32 · Whisper small · …", in role order.
  var modelsLine: String {
    Self.roles.flatMap { models[$0] ?? [] }.joined(separator: " · ")
  }

  /// Whether the tier has a model for `piece` to hold. Potato has no sound
  /// model but still speech, so its audio piece stays offered (as Speech).
  func offers(_ piece: TierPiece) -> Bool {
    piece.roles.contains { !(models[$0] ?? []).isEmpty }
  }

  /// The checkbox's word for the audio piece: what this tier's piece finds.
  var soundsLabel: String {
    (models["sounds"] ?? []).isEmpty ? "Speech" : "Sounds"
  }

  /// The pieces a choice installs: Core and the large tower, plus `pieces`
  /// the tier offers.
  func pieceIDs(_ pieces: Set<TierPiece>) -> [String] {
    Self.basePieces + TierPiece.allCases.filter { pieces.contains($0) && offers($0) }.map(\.rawValue)
  }

  func downloadTotal(_ pieces: Set<TierPiece>) -> UInt64 {
    pieceIDs(pieces).reduce(0) { $0 + (downloadBytes[$1] ?? 0) }
  }

  /// What it holds on disk, the Mac's Core ML cache included.
  func diskTotal(_ pieces: Set<TierPiece>, coreMLCache: Bool = true) -> UInt64 {
    pieceIDs(pieces).reduce(0) { $0 + (diskBytes[$1] ?? 0) } + (coreMLCache ? coremlCacheBytes : 0)
  }

  /// photos/s for this machine class; nil when not measured.
  func photosPerSecond(accelerated: Bool) -> Double? {
    accelerated ? photosPerSecondAccelerated : photosPerSecondCPU
  }

  /// "~5 min per 10,000 photos"; "" when the tier was not measured.
  func timeText(accelerated: Bool) -> String {
    guard let rate = photosPerSecond(accelerated: accelerated) else { return "" }
    return TierText.duration(10_000 / rate) + " per 10,000 photos"
  }
}

/// Sizes and times as the picker words them.
enum TierText {
  /// "1.2 GB" / "340 MB": the chrome's byte style (bytesText, as Finder counts).
  static func size(_ bytes: UInt64) -> String { bytesText(bytes) }

  /// "1.2 GB of 3.4 GB".
  static func sizePair(_ done: UInt64, _ total: UInt64) -> String {
    "\(bytesText(min(done, total))) of \(bytesText(total))"
  }

  /// "under a minute", "~25 min", "~3 h".
  static func duration(_ seconds: Double) -> String {
    if seconds < 60 { return "under a minute" }
    if seconds < 5400 { return "~\(Int((seconds / 60).rounded())) min" }
    return "~\(Int((seconds / 3600).rounded())) h"
  }
}

/// Everything the picker shows: the tiers, recommended first, and the check.
struct TierCatalogue: Equatable {
  /// The recommended tier first, then the others in tiers.json's order.
  var tiers: [TierOption] = []
  var machine = TierMachine()
  /// The tier in force ("" before one is chosen).
  var chosen = ""
  var recommendedID = ""

  init() {}

  /// From settings_json, parsed (`settings`) and as text (`raw`): JSON
  /// objects lose their key order in JSONSerialization, and tiers.json lists
  /// its profiles in display order.
  init(settings: [String: Any], raw: String) {
    chosen = settings["tier"] as? String ?? ""
    machine = TierMachine(settings["machine"] as? [String: Any] ?? [:])
    let tiersObj = settings["tiers"] as? [String: Any] ?? [:]
    let profiles = tiersObj["profiles"] as? [String: Any] ?? [:]
    let order = Self.profileOrder(raw: raw, ids: Array(profiles.keys))
    let all = order.compactMap { id in (profiles[id] as? [String: Any]).map { TierOption(id: id, $0) } }
    let fallback = tiersObj["default"] as? String ?? ""
    let rec = [machine.recommended, fallback].first { id in all.contains { $0.id == id } }
      ?? all.first?.id ?? ""
    recommendedID = rec
    tiers = all.filter { $0.id == rec } + all.filter { $0.id != rec }
  }

  var isEmpty: Bool { tiers.isEmpty }
  var recommended: TierOption? { tiers.first { $0.id == recommendedID } }
  var others: [TierOption] { tiers.filter { $0.id != recommendedID } }
  func tier(_ id: String) -> TierOption? { tiers.first { $0.id == id } }
  /// The name Settings shows for the tier in force: "⭐ Default".
  var chosenTitle: String { tier(chosen)?.title ?? (chosen.isEmpty ? "Not chosen" : chosen) }

  /// `ids` in the order their keys first appear after `"profiles"` in `raw`;
  /// any not found keep a stable order after them.
  static func profileOrder(raw: String, ids: [String]) -> [String] {
    let start = raw.range(of: "\"profiles\"")?.upperBound ?? raw.startIndex
    let tail = raw[start...]
    func position(_ id: String) -> Int {
      guard let r = tail.range(of: "\"\(id)\"") else { return Int.max }
      return tail.distance(from: tail.startIndex, to: r.lowerBound)
    }
    return ids.sorted { a, b in
      let pa = position(a), pb = position(b)
      return pa != pb ? pa < pb : a < b
    }
  }
}

/// The picker's choices, apart from the view: what is selected and checked,
/// what Download installs, and what its button and note say.
struct TierChoice: Equatable {
  var selected: String
  var checked: Set<TierPiece>
  /// Pieces the pack has loaded now: checked, and not unchecked here
  /// (removing one is Settings' job).
  var installed: Set<TierPiece>
  /// The tier in force when the picker opened ("" none).
  var current: String

  /// Preselects the tier in force, else the recommended one. The first
  /// choice checks Sounds, and Documents while File types "Show documents"
  /// is on (`documentFiles`); People is an opt-in and starts off (as the
  /// Windows picker). Re-opened with a tier in force, only what is installed
  /// is checked: Switch waits for another tier or a new piece.
  init(catalogue: TierCatalogue, installed: Set<TierPiece>, documentFiles: Bool = false) {
    current = catalogue.chosen
    selected = catalogue.tier(catalogue.chosen) != nil ? catalogue.chosen : catalogue.recommendedID
    self.installed = installed
    var pieces: Set<TierPiece> = []
    if current.isEmpty {
      pieces.insert(.sounds)
      if documentFiles { pieces.insert(.documents) }
    }
    checked = pieces.union(installed)
  }

  func isChecked(_ piece: TierPiece) -> Bool { checked.contains(piece) || installed.contains(piece) }

  mutating func toggle(_ piece: TierPiece) {
    guard !installed.contains(piece) else { return }
    if checked.contains(piece) { checked.remove(piece) } else { checked.insert(piece) }
  }

  var switching: Bool { !current.isEmpty && selected != current }

  /// "Download" for the first choice; "Switch" once a tier is in force.
  var buttonTitle: String { current.isEmpty ? "Download" : "Switch" }

  /// The first choice can always be made; Switch needs another tier or a
  /// new piece.
  func canConfirm(_ tier: TierOption?) -> Bool {
    guard let tier else { return false }
    return current.isEmpty || switching || !installIDs(tier).isEmpty
  }

  static let switchNote = "Switching re-indexes your folders; the current index keeps answering until the new one is ready."

  /// What the host's install hook gets: the large tower if the tier needs
  /// it, and the checked pieces the tier offers that are not installed
  /// (the host skips installed ones too).
  func installIDs(_ tier: TierOption) -> [String] {
    (tier.needsLarge ? ["ai-large"] : [])
      + TierPiece.allCases.filter { checked.contains($0) && !installed.contains($0) && tier.offers($0) }
      .map(\.rawValue)
  }
}

/// The downloader's progress (mv_ai_status fetch_*), as one line and the
/// action beside it.
struct FetchLine: Equatable {
  enum Action: Equatable { case none, cancel, retry, resume }

  var text = ""
  var action = Action.none
  /// The bar; nil for no bar.
  var fraction: Double?

  /// nil when there is nothing to say (idle, done, or no tier yet).
  static func from(_ s: mv_ai_status) -> FetchLine? {
    from(state: s.fetch_state, fetching: s.flags & MV_AI_STATUS_FETCHING != 0,
         filesDone: s.fetch_files_done, filesTotal: s.fetch_files_total,
         doneBytes: s.fetch_done_bytes, totalBytes: s.fetch_total_bytes)
  }

  static func from(state: UInt32, fetching: Bool, filesDone: UInt32, filesTotal: UInt32,
                   doneBytes: UInt64, totalBytes: UInt64) -> FetchLine? {
    switch state {
    case MV_AI_FETCH_WAITING.rawValue:
      return FetchLine(text: "Waiting for Hugging Face… downloads resume automatically", action: .cancel)
    case MV_AI_FETCH_GONE.rawValue:
      return FetchLine(text: "A model file is no longer on Hugging Face. An app update will fix this.")
    case MV_AI_FETCH_FAILED.rawValue:
      return FetchLine(text: "A downloaded model did not match its checksum.", action: .retry)
    case MV_AI_FETCH_DOWNLOADING.rawValue:
      break
    case MV_AI_FETCH_IDLE.rawValue where !fetching:
      // Cancelled with files still to come: kept, to resume.
      guard totalBytes > 0, doneBytes < totalBytes else { return nil }
      return FetchLine(text: "Download paused", action: .resume,
                       fraction: Double(doneBytes) / Double(totalBytes))
    case MV_AI_FETCH_IDLE.rawValue:
      break
    default:
      return nil
    }
    var text = "Downloading models…"
    if totalBytes > 0 { text += " " + TierText.sizePair(doneBytes, totalBytes) }
    if filesTotal > 0 { text += " (\(min(filesDone, filesTotal)) of \(filesTotal) files)" }
    return FetchLine(text: text, action: .cancel,
                     fraction: totalBytes > 0 ? min(1, Double(doneBytes) / Double(totalBytes)) : nil)
  }
}
