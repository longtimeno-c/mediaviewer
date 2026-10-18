// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model tier picker's logic (#482 T4): settings_json's tiers and machine
// into the picker's catalogue (recommended first, tiers.json's order kept),
// its size and time words, what Download installs, and the download line for
// each fetch_state.
//   cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter TierModelTests
import XCTest
@testable import AIChrome
import CAiApi

final class TierModelTests: XCTestCase {
  /// A settings_json as the pack writes it: profiles in display order, a
  /// machine that recommends `recommended`.
  private func settings(recommended: String = "default", accelerator: String = "apple",
                        tier: String = "") -> String {
    """
    {"compute":0,"tier":"\(tier)","tiers":{"default":"default","profiles":{
     "maxxed":{"name":"Maxxed","emoji":"🚀","for":"Apple silicon Pro, Max and Ultra",
      "summary":{"models":{"pictures":["CLIP ViT-H/14","CLIP ViT-B/32"],"speech":["Whisper turbo"],
       "sounds":["CLAP"],"faces":["YuNet + AdaFace IR-50"],"documents":["MiniLM"]},
       "licences":["MIT","Apache-2.0"],
       "download_bytes":{"ai":600000000,"ai-large":2000000000,"ai-audio":1500000000,"ai-faces":180000000,"ai-docs":90000000},
       "disk_bytes":{"ai":700000000,"ai-large":2000000000,"ai-audio":1500000000,"ai-faces":180000000,"ai-docs":90000000},
       "coreml_cache_bytes":4100000000}},
     "default":{"name":"Default","emoji":"⭐","for":"What ships today",
      "summary":{"models":{"documents":["MiniLM"],"pictures":["CLIP ViT-L/14","CLIP ViT-B/32"],
       "faces":["YuNet + AdaFace IR-50"],"speech":["Whisper small"],"sounds":["CLAP"]},
       "licences":["MIT"],
       "download_bytes":{"ai":1200000000,"ai-audio":900000000,"ai-faces":180000000,"ai-docs":90000000},
       "disk_bytes":{"ai":1300000000,"ai-audio":900000000,"ai-faces":180000000,"ai-docs":90000000},
       "coreml_cache_bytes":4100000000,"photos_per_s":{"accelerated":31,"cpu":40}}},
     "intel":{"name":"Intel","emoji":"💻","for":"Intel Macs","sounds_off_by_default":true,
      "summary":{"models":{"pictures":["CLIP ViT-B/32"],"speech":["Whisper base"],"sounds":["CLAP"]},
       "licences":["MIT"],"download_bytes":{"ai":350000000,"ai-audio":500000000},
       "disk_bytes":{"ai":350000000,"ai-audio":500000000},"photos_per_s":{"accelerated":499,"cpu":40}}},
     "potato":{"name":"Potato","emoji":"🥔","for":"Dual-core laptops",
      "summary":{"models":{"pictures":["TinyCLIP 8M"],"speech":["Whisper tiny"],"faces":["YuNet + SFace"]},
       "licences":["MIT"],"download_bytes":{"ai":40000000,"ai-audio":75000000,"ai-faces":40000000},
       "disk_bytes":{"ai":40000000,"ai-audio":75000000,"ai-faces":40000000}}}}},
     "machine":{"os":"macos","arch":"arm64","cores":10,"ram_bytes":17179869184,"free_bytes":1,
      "chip":"Apple M2 Pro","accelerator":"\(accelerator)","laptop":true,"recommended":"\(recommended)"}}
    """
  }

  private func catalogue(_ raw: String) -> TierCatalogue {
    let obj = try! JSONSerialization.jsonObject(with: Data(raw.utf8)) as! [String: Any]
    return TierCatalogue(settings: obj, raw: raw)
  }

  func testRecommendedFirstThenFileOrder() {
    XCTAssertEqual(catalogue(settings()).tiers.map(\.id), ["default", "maxxed", "intel", "potato"])
    let c = catalogue(settings(recommended: "potato"))
    XCTAssertEqual(c.tiers.map(\.id), ["potato", "maxxed", "default", "intel"])
    XCTAssertEqual(c.recommended?.id, "potato")
    XCTAssertEqual(c.others.map(\.id), ["maxxed", "default", "intel"])
    // An unknown recommendation falls back to tiers.json's default.
    XCTAssertEqual(catalogue(settings(recommended: "huge")).recommendedID, "default")
  }

  func testProfileOrderIgnoresTheDefaultKey() {
    // "default":"default" comes before "profiles": the order starts after it.
    let raw = #"{"default":"default","profiles":{"b":{},"default":{},"a":{}}}"#
    XCTAssertEqual(TierCatalogue.profileOrder(raw: raw, ids: ["a", "default", "b"]), ["b", "default", "a"])
  }

  func testMachineLine() {
    XCTAssertEqual(catalogue(settings()).machine.line, "We checked this Mac: Apple M2 Pro · 16 GB · Neural Engine")
    XCTAssertEqual(catalogue(settings(accelerator: "")).machine.line,
                   "We checked this Mac: Apple M2 Pro · 16 GB · CPU only")
  }

  func testModelsLineInRoleOrder() {
    let d = catalogue(settings()).tier("default")!
    XCTAssertEqual(d.title, "⭐ Default")
    XCTAssertEqual(d.modelsLine,
                   "CLIP ViT-L/14 · CLIP ViT-B/32 · Whisper small · CLAP · YuNet + AdaFace IR-50 · MiniLM")
  }

  func testSizesFollowThePieces() {
    let c = catalogue(settings())
    let d = c.tier("default")!
    XCTAssertEqual(d.downloadTotal([]), 1_200_000_000)
    XCTAssertEqual(d.downloadTotal(Set(TierPiece.allCases)), 2_370_000_000)
    XCTAssertEqual(TierText.size(d.downloadTotal([.people])), bytesText(1_380_000_000))
    // Disk counts the Core ML cache on the Mac.
    XCTAssertEqual(d.diskTotal([]), 5_400_000_000)
    XCTAssertEqual(d.diskTotal([], coreMLCache: false), 1_300_000_000)
    // Maxxed's large tower always comes with it.
    let m = c.tier("maxxed")!
    XCTAssertTrue(m.needsLarge)
    XCTAssertEqual(m.downloadTotal([]), 2_600_000_000)
    XCTAssertEqual(TierText.size(40_000_000), "40 MB")
  }

  func testTimePer10kPhotosForThisMachineClass() {
    let c = catalogue(settings())
    XCTAssertEqual(c.tier("default")!.timeText(accelerated: true), "~5 min per 10,000 photos")
    XCTAssertEqual(c.tier("intel")!.timeText(accelerated: true), "under a minute per 10,000 photos")
    XCTAssertEqual(c.tier("intel")!.timeText(accelerated: false), "~4 min per 10,000 photos")
    XCTAssertEqual(c.tier("potato")!.timeText(accelerated: true), "")  // not measured
    XCTAssertEqual(TierText.duration(3 * 3600), "~3 h")
  }

  func testPiecesOffered() {
    let c = catalogue(settings())
    let intel = c.tier("intel")!
    XCTAssertFalse(intel.offers(.people))
    XCTAssertTrue(intel.offers(.sounds))
    // Potato has no sound model, but speech: the piece is offered as Speech.
    let potato = c.tier("potato")!
    XCTAssertTrue(potato.offers(.sounds))
    XCTAssertEqual(potato.soundsLabel, "Speech")
    XCTAssertEqual(c.tier("default")!.soundsLabel, "Sounds")
    XCTAssertFalse(potato.offers(.documents))
  }

  func testFirstChoiceDefaults() {
    let c = catalogue(settings())
    // People is an opt-in; Documents follows File types "Show documents".
    var choice = TierChoice(catalogue: c, installed: [], documentFiles: false)
    XCTAssertEqual(choice.selected, "default")
    XCTAssertEqual(choice.buttonTitle, "Download")
    XCTAssertFalse(choice.isChecked(.people))
    XCTAssertTrue(choice.isChecked(.sounds))
    XCTAssertFalse(choice.isChecked(.documents))
    XCTAssertTrue(choice.canConfirm(c.tier("default")))
    choice = TierChoice(catalogue: c, installed: [], documentFiles: true)
    XCTAssertTrue(choice.isChecked(.documents))
  }

  func testInstallIDs() {
    let c = catalogue(settings())
    var choice = TierChoice(catalogue: c, installed: [.people], documentFiles: true)
    // Installed pieces stay checked and are not asked for again.
    choice.toggle(.people)
    XCTAssertTrue(choice.isChecked(.people))
    XCTAssertEqual(choice.installIDs(c.tier("default")!), ["ai-audio", "ai-docs"])
    choice.toggle(.documents)
    XCTAssertEqual(choice.installIDs(c.tier("default")!), ["ai-audio"])
    // Maxxed brings its large tower.
    choice.selected = "maxxed"
    XCTAssertEqual(choice.installIDs(c.tier("maxxed")!), ["ai-large", "ai-audio"])
    // A piece the tier does not offer is never asked for.
    choice.toggle(.documents)
    choice.selected = "potato"
    XCTAssertEqual(choice.installIDs(c.tier("potato")!), ["ai-audio"])
  }

  func testReopenedWithATierSwitches() {
    let c = catalogue(settings(tier: "potato"))
    var choice = TierChoice(catalogue: c, installed: [.sounds], documentFiles: true)
    XCTAssertEqual(choice.selected, "potato")  // the tier in force, not the recommendation
    XCTAssertEqual(choice.buttonTitle, "Switch")
    // Nothing new checked: Switch waits for another tier or a new piece.
    XCTAssertFalse(choice.isChecked(.documents))
    XCTAssertFalse(choice.canConfirm(c.tier("potato")))
    choice.toggle(.people)
    XCTAssertTrue(choice.canConfirm(c.tier("potato")))
    choice.toggle(.people)
    choice.selected = "default"
    XCTAssertTrue(choice.switching)
    XCTAssertTrue(choice.canConfirm(c.tier("default")))
    XCTAssertEqual(c.chosenTitle, "🥔 Potato")
  }

  func testDownloadLineForEachState() {
    func line(_ state: mv_ai_fetch_state, fetching: Bool = false, files: (UInt32, UInt32) = (5, 12),
              bytes: (UInt64, UInt64) = (1_200_000_000, 3_400_000_000)) -> FetchLine? {
      FetchLine.from(state: state.rawValue, fetching: fetching, filesDone: files.0, filesTotal: files.1,
                     doneBytes: bytes.0, totalBytes: bytes.1)
    }
    let down = line(MV_AI_FETCH_DOWNLOADING)!
    XCTAssertEqual(down.text, "Downloading models… 1.2 GB of 3.4 GB (5 of 12 files)")
    XCTAssertEqual(down.action, .cancel)
    XCTAssertEqual(down.fraction!, 1.2 / 3.4, accuracy: 1e-9)
    XCTAssertEqual(line(MV_AI_FETCH_DOWNLOADING, bytes: (120_000_000, 800_000_000))!.text,
                   "Downloading models… 120 MB of 800 MB (5 of 12 files)")
    XCTAssertEqual(line(MV_AI_FETCH_IDLE, fetching: true)!.text, down.text)
    let wait = line(MV_AI_FETCH_WAITING)!
    XCTAssertEqual(wait.text, "Waiting for Hugging Face… downloads resume automatically")
    XCTAssertEqual(wait.action, .cancel)
    XCTAssertEqual(line(MV_AI_FETCH_GONE)!.text,
                   "A model file is no longer on Hugging Face. An app update will fix this.")
    XCTAssertEqual(line(MV_AI_FETCH_GONE)!.action, .none)
    XCTAssertEqual(line(MV_AI_FETCH_FAILED)!.text, "A downloaded model did not match its checksum.")
    XCTAssertEqual(line(MV_AI_FETCH_FAILED)!.action, .retry)
    let paused = line(MV_AI_FETCH_IDLE)!
    XCTAssertEqual(paused.text, "Download paused")
    XCTAssertEqual(paused.action, .resume)
    XCTAssertNil(line(MV_AI_FETCH_IDLE, bytes: (0, 0)))
    XCTAssertNil(line(MV_AI_FETCH_IDLE, bytes: (5, 5)))
    XCTAssertNil(line(MV_AI_FETCH_DONE))
  }

  func testStatusLineCarriesTheDownload() {
    var s = mv_ai_status()
    s.fetch_state = MV_AI_FETCH_WAITING.rawValue
    XCTAssertEqual(StatusLine(s).detail, "Waiting for Hugging Face… downloads resume automatically")
    s.flags = MV_AI_STATUS_TIER_UNCHOSEN
    XCTAssertEqual(StatusLine(s).detail, "Choose your models in Settings → Local search to start")
  }
}
