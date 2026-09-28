// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Local search, once Core is loaded (the chrome brief, "Management
// panel"; plan/17 PRs 20, 21, 23, 24): status, Compute (Auto / Core ML / CPU
// only), Precision (the one quality scale; the model stays on Auto), the indexed folders, the index size and Clear, the
// battery rule, and People. The base app embeds this view (MVAIChrome
// -settingsView) under its install / remove rows.
//
// Reads of the index (roots_json, people_json, person_faces_json) and the
// writes that delete rows or regroup faces (root_remove, clear_index, faces
// off, merge, split, reject: they may wait for the indexer to let go) are
// [worker-thread] and run detached; settings and status are [no-block].
// Status is polled at ≤ 4 Hz only while this view is on screen.
import AppKit
import CAiApi
import Photos
import SwiftUI

struct RootRow: Identifiable, Equatable {
  let id: UInt64
  let path: String
  let recursive: Bool
  let enabled: Bool
  let assets: Int64
  let done: Int64
  let bytes: Int64
  let media: UInt32       // MV_AI_MEDIA_*: what its videos are indexed for (0 = the setting)
  // The Photos library root (issue #72): "photos"; its PhotoKit access as the
  // pack sees it; its assets only iCloud has.
  var kind = "folder"
  var access = ""
  var unavailable: Int64 = 0
  var isPhotos: Bool { kind == "photos" }
}

struct Person: Identifiable, Equatable {
  let id: UInt64
  var name: String
  var faces: Int
  let coverFace: UInt64     // face_thumb(cover_face), cropped with coverBox
  let coverBox: [Double]
}

struct Face: Identifiable, Equatable {
  let id: UInt64
  let path: String
  let ptsMs: Int64
  let box: [Double]
}

@MainActor
final class ManagementModel: ObservableObject {
  let table: AITable
  weak var chrome: MVAIChrome?

  @Published private(set) var status = StatusLine()
  @Published private(set) var indexBytes: UInt64 = 0
  @Published private(set) var flags: UInt32 = 0
  @Published private(set) var compute: Int = 0
  @Published private(set) var quality: Int = 0
  /// Settings "Precision": 0 broader … 2 as calibrated … 4 stricter.
  @Published private(set) var precision: Int = 2
  @Published private(set) var batteryPercent: Int = 30
  @Published private(set) var capBytes: Int64 = 0
  @Published private(set) var facesOn = false
  @Published private(set) var coreMLAvailable = true
  /// Settings "Index videos for" (MV_AI_MEDIA_*: 1 Pictures, 2 Sound, 3 Both).
  @Published private(set) var videoIndex: Int = 1
  /// The Sound piece (ai-audio) is installed and loaded.
  @Published private(set) var audioReady = false
  @Published private(set) var models: [(quality: Int, name: String)] = []
  @Published private(set) var roots: [RootRow] = []
  var folderRoots: [RootRow] { roots.filter { !$0.isPhotos } }
  var photosRoot: RootRow? { roots.first { $0.isPhotos } }
  /// The pack has a Photos library source (macOS, issue #72).
  let photosSupported: Bool
  /// PhotoKit's answer, read when Settings shows and after the prompt.
  @Published private(set) var photosAccess: PHAuthorizationStatus = .notDetermined
  @Published private(set) var photosAdding = false
  @Published private(set) var people: [Person] = []
  @Published var confirming: Confirm?
  @Published var message = ""
  /// What the last People action did ("Merged 2 people into Sam."), shown in
  /// the People section itself: the top line is out of sight down there.
  @Published private(set) var peopleNote = ""

  enum Confirm: Equatable {
    case clearIndex, removeRoot(UInt64), facesOff
  }

  private var timer: Timer?
  private var visible = 0
  /// people_json in flight; another request while it runs is folded into one
  /// more read after it (a face landing posts AI_PEOPLE for every face).
  private var peopleLoading = false
  private var peopleAgain = false
  /// faces_total and people as the last status saw them: a change reloads
  /// the grid even if an AI_PEOPLE event was missed.
  private var faceCounts: (UInt64, UInt32) = (0, 0)
  private var noteGeneration = 0

  init(table: AITable) {
    self.table = table
    photosSupported = table.hasPhotos
    photosAccess = PhotosLibrary.status
    reloadSettings()
    pollStatus()
  }

  var facesReady: Bool { flags & MV_AI_STATUS_FACES_READY != 0 }

  func appeared() {
    visible += 1
    guard visible == 1 else { return }
    reloadSettings()
    photosAccess = PhotosLibrary.status  // it may have changed in System Settings
    reloadRoots()
    reloadPeople()
    pollStatus()
    timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.pollStatus() }
    }
  }

  func disappeared() {
    visible = max(0, visible - 1)
    guard visible == 0 else { return }
    timer?.invalidate()
    timer = nil
  }

  func stop() {
    timer?.invalidate()
    timer = nil
    visible = 0
  }

  func statusChanged() { pollStatus() }

  private func pollStatus() {
    guard let s = table.status() else { return }
    if s.index_bytes != indexBytes { indexBytes = s.index_bytes }
    if s.flags != flags {
      let facesChanged = (s.flags ^ flags) & (MV_AI_STATUS_FACES_READY | MV_AI_STATUS_FACES_ON) != 0
      let audioChanged = (s.flags ^ flags) & MV_AI_STATUS_AUDIO_READY != 0
      flags = s.flags
      // The People or Sound piece came or went (installed, removed, or the
      // opt-in flipped): the view follows now, not at the next start.
      if facesChanged || audioChanged { reloadSettings() }
      if facesChanged { reloadPeople() }
    }
    if s.faces_total != faceCounts.0 || s.people != faceCounts.1 {
      faceCounts = (s.faces_total, s.people)
      if facesReady { reloadPeople() }
    }
    let line = StatusLine(s)
    if line != status { status = line }
  }

  func setPaused(_ paused: Bool) {
    _ = table.a.pause?(table.ctx, paused ? 1 : 0)
    pollStatus()
  }

  /// "Index anyway" while paused on battery: until the Mac is next on power.
  func indexAnyway() {
    table.indexAnyway()
    pollStatus()
  }

  // MARK: settings

  func reloadSettings() {
    guard let obj = parseJSON(table.json { table.a.settings_json?(table.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG })
            as? [String: Any] else { return }
    compute = Int(int64(obj["compute"]))
    quality = Int(int64(obj["quality"]))
    precision = obj["precision"] == nil ? 2 : min(4, max(0, Int(int64(obj["precision"]))))
    batteryPercent = Int(int64(obj["pause_on_battery_percent"]))
    capBytes = int64(obj["index_cap_bytes"])
    facesOn = obj["faces"] as? Bool ?? false
    coreMLAvailable = (obj["available"] as? [String: Any])?["coreml"] as? Bool ?? false
    let index = Int(int64(obj["video_index"]))
    videoIndex = index == 0 ? Int(MV_AI_MEDIA_PICTURES) : index
    audioReady = obj["audio_ready"] as? Bool ?? false
    models = (obj["models"] as? [[String: Any]] ?? []).map {
      (quality: Int(int64($0["quality"])), name: $0["name"] as? String ?? "")
    }
  }

  func set(_ key: String, _ value: Int64) {
    table.setSetting(key, String(value))
    reloadSettings()
    pollStatus()
  }

  /// Settings "Precision": read by each search as it starts, so the open
  /// search answers again at once; nothing is re-indexed.
  func setPrecision(_ level: Int) {
    let clamped = min(4, max(0, level))
    guard clamped != precision else { return }
    set("precision", Int64(clamped))
    chrome?.precisionChanged()
  }

  /// Settings "Index videos for". Sound and Both need the Sound piece loaded:
  /// the control disables them, and this refuses them as well.
  func setVideoIndex(_ media: Int) {
    guard media == Int(MV_AI_MEDIA_PICTURES) || (audioReady && (media == Int(MV_AI_MEDIA_SOUND) ||
                                                                 media == Int(MV_AI_MEDIA_BOTH))) else {
      return
    }
    set("video_index", Int64(media))
  }

  func modelName(_ q: Int) -> String { models.first(where: { $0.quality == q })?.name ?? "" }

  // MARK: roots

  func reloadRoots() {
    let t = table
    Task.detached {
      let json = t.json { t.a.roots_json?(t.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG }
      let rows: [RootRow] = (parseJSON(json) as? [[String: Any]] ?? []).map {
        RootRow(id: UInt64(clamping: int64($0["id"])), path: $0["path"] as? String ?? "",
                recursive: $0["recursive"] as? Bool ?? false, enabled: $0["enabled"] as? Bool ?? true,
                assets: int64($0["assets"]), done: int64($0["done"]), bytes: int64($0["bytes"]),
                media: UInt32(clamping: int64($0["media"])),
                kind: $0["kind"] as? String ?? "folder", access: $0["access"] as? String ?? "",
                unavailable: int64($0["unavailable"]))
      }
      await MainActor.run { if rows != self.roots { self.roots = rows } }
    }
  }

  func setRootEnabled(_ id: UInt64, _ on: Bool) {
    _ = table.a.root_set_enabled?(table.ctx, id, on ? 1 : 0)
    reloadRoots()
  }

  /// Per folder: Default / Pictures / Sound / Both (root_set_media).
  func setRootMedia(_ id: UInt64, _ media: UInt32) {
    guard table.has(\mv_ai_api.root_set_media), media & MV_AI_MEDIA_SOUND == 0 || audioReady else { return }
    _ = table.a.root_set_media?(table.ctx, id, media)
    reloadRoots()
    pollStatus()
  }

  func rescan(_ id: UInt64) {
    _ = table.a.root_rescan?(table.ctx, id)
    reloadRoots()
  }

  func removeRoot(_ id: UInt64) {
    confirming = nil
    // Gone from the list at once; the rows are deleted on a worker.
    roots.removeAll { $0.id == id }
    let t = table
    Task.detached {
      t.call { t.a.root_remove?(t.ctx, id) }
      await MainActor.run {
        self.reloadRoots()
        self.pollStatus()
      }
    }
  }

  func addFolder(recursive: Bool) {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.prompt = "Index"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    var root: UInt64 = 0
    _ = table.a.index_folder?(table.ctx, url.path, recursive ? 1 : 0, &root)
    reloadRoots()
    pollStatus()
  }

  /// Settings → Photos Library → Add: the system's prompt (only here, only
  /// from this click), then the pack remembers the library and indexes it.
  func addPhotosLibrary() {
    guard photosSupported, !photosAdding else { return }
    photosAdding = true
    Task { @MainActor in
      let answer = await PhotosLibrary.requestAccess()
      photosAccess = answer
      if answer == .authorized || answer == .limited {
        var root: UInt64 = 0
        let st = table.call { table.a.index_photos_library?(table.ctx, &root) ?? MV_ERR_INVALID_ARG }
        if st != MV_OK { note("The Photos library could not be added.") }
      }
      photosAdding = false
      reloadRoots()
      pollStatus()
    }
  }

  func clearIndex() {
    confirming = nil
    message = "Clearing the search index…"
    let t = table
    Task.detached {
      // It waits for the indexer to stop (seconds): never on the main thread.
      let ok = t.call { t.a.clear_index?(t.ctx) } == MV_OK
      await MainActor.run {
        self.message = ok ? "The search index was cleared. Thumbnails were kept." : "The index could not be cleared."
        self.reloadRoots()
        self.pollStatus()
      }
    }
  }

  // MARK: people (PR 24)

  func setFaces(_ on: Bool) {
    if !on {
      confirming = .facesOff
      return
    }
    _ = table.a.faces_enable?(table.ctx, 1)
    reloadSettings()
    pollStatus()
  }

  func facesOffConfirmed() {
    confirming = nil
    people = []
    message = "Deleting face data…"
    let t = table
    Task.detached {
      let ok = t.call { t.a.faces_enable?(t.ctx, 0) } == MV_OK
      await MainActor.run {
        self.message = ok ? "All face data was deleted." : "Face data could not be deleted."
        self.reloadSettings()
        self.pollStatus()
      }
    }
  }

  func reloadPeople() {
    guard facesOn else {
      if !people.isEmpty { people = [] }
      return
    }
    if peopleLoading {
      peopleAgain = true
      return
    }
    peopleLoading = true
    let t = table
    Task.detached {
      let json = t.json { t.a.people_json?(t.ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG }
      let list: [Person] = (parseJSON(json) as? [[String: Any]] ?? []).map {
        Person(id: UInt64(clamping: int64($0["id"])), name: $0["name"] as? String ?? "",
               faces: Int(int64($0["faces"])), coverFace: UInt64(clamping: int64($0["cover_face"])),
               coverBox: ($0["cover_box"] as? [NSNumber] ?? []).map { $0.doubleValue })
      }
      await MainActor.run {
        if list != self.people { self.people = list }
        guard self.peopleAgain else {
          self.peopleLoading = false
          return
        }
        // While faces stream in: at most two reads a second.
        self.peopleAgain = false
        Task { @MainActor in
          try? await Task.sleep(nanoseconds: 500_000_000)
          self.peopleLoading = false
          self.reloadPeople()
        }
      }
    }
  }

  /// A line under People that clears itself after a few seconds.
  func note(_ text: String) {
    noteGeneration &+= 1
    let g = noteGeneration
    peopleNote = text
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 5_000_000_000)
      if self.noteGeneration == g { self.peopleNote = "" }
    }
  }

  func displayName(_ p: Person) -> String {
    p.name.isEmpty ? "Unnamed (\(p.faces == 1 ? "1 photo" : "\(p.faces) photos"))" : p.name
  }

  func rename(_ id: UInt64, _ name: String) {
    _ = table.a.person_rename?(table.ctx, id, name)
    if let i = people.firstIndex(where: { $0.id == id }) { people[i].name = name }
  }

  func merge(into: UInt64, from: UInt64) { merge(into: into, from: [from]) }

  /// Several people are one: their faces move to `into`, which keeps its name
  /// (or takes the first name among them if it has none, as faces_db does).
  /// The grid changes now; the rows are rewritten on a worker.
  func merge(into: UInt64, from ids: [UInt64]) {
    let from = ids.filter { $0 != into }
    guard !from.isEmpty, let i = people.firstIndex(where: { $0.id == into }) else { return }
    let moved = people.filter { from.contains($0.id) }
    if people[i].name.isEmpty, let named = moved.first(where: { !$0.name.isEmpty }) {
      people[i].name = named.name
    }
    people[i].faces += moved.reduce(0) { $0 + $1.faces }
    let name = people[i].name.isEmpty ? "one person" : people[i].name
    people.removeAll { from.contains($0.id) }
    note(moved.count == 1 ? "Merged into \(name)." : "Merged \(moved.count + 1) people into \(name).")
    let t = table
    Task.detached {
      let failed = from.map { f in t.call { t.a.person_merge?(t.ctx, into, f) } }.contains { $0 != MV_OK }
      await MainActor.run {
        if failed { self.note("Some faces could not be merged. Try again.") }
        self.reloadPeople()
      }
    }
  }

  func faces(of person: UInt64) async -> [Face] {
    let t = table
    return await Task.detached {
      let json = t.json { t.a.person_faces_json?(t.ctx, person, $0, $1, $2) ?? MV_ERR_INVALID_ARG }
      return (parseJSON(json) as? [[String: Any]] ?? []).map {
        Face(id: UInt64(clamping: int64($0["face"])), path: $0["path"] as? String ?? "",
             ptsMs: ($0["pts_ms"] as? NSNumber)?.int64Value ?? -1,
             box: ($0["box"] as? [NSNumber] ?? []).map { $0.doubleValue })
      }
    }.value
  }

  /// A pack built before person_refine was appended has no "Refine faces".
  var canRefine: Bool { table.has(\mv_ai_api.person_refine) }

  /// "Refine faces" (plan/17 "People refinement"): the pack re-checks this
  /// person's faces and files the misplaced ones out. Only ever on request.
  /// How many left the person; nil when it could not run.
  func refine(_ person: UInt64) async -> UInt32? {
    guard canRefine else { return nil }
    let t = table
    let removed: UInt32? = await Task.detached {
      var n: UInt32 = 0
      return t.call { t.a.person_refine?(t.ctx, person, &n) } == MV_OK ? n : nil
    }.value
    reloadPeople()
    return removed
  }

  /// "Not this person" for each face, then one reload.
  func reject(_ faces: [UInt64]) {
    guard !faces.isEmpty else { return }
    let t = table
    Task.detached {
      for face in faces { t.call { t.a.face_reject?(t.ctx, face) } }
      await MainActor.run { self.reloadPeople() }
    }
  }

  func split(_ faces: [UInt64]) {
    guard !faces.isEmpty else { return }
    let t = table
    Task.detached {
      var person: UInt64 = 0
      t.call { faces.withUnsafeBufferPointer { t.a.face_split?(t.ctx, $0.baseAddress, UInt32($0.count), &person) } }
      await MainActor.run { self.reloadPeople() }
    }
  }

  /// A person's photos, in the gallery (Settings closes as it opens). The
  /// search takes a moment: the card says so, and says if nothing came back.
  @Published private(set) var opening: UInt64?

  func showPhotos(of person: Person) {
    guard let chrome, opening == nil else { return }
    opening = person.id
    let name = person.name.isEmpty ? "this person" : person.name
    chrome.showPerson(id: person.id, name: person.name) { [weak self] outcome in
      guard let self else { return }
      self.opening = nil
      switch outcome {
      case .opened: break
      case .nothing: self.note("No photos of \(name) are indexed yet.")
      case .failed: self.note("Photos of \(name) could not be opened. Try again.")
      }
    }
  }
}

/// Settings "Precision": a five-step slider, Broader … Stricter, the middle
/// the calibrated rule. The arrow keys step it (the slider's own keyboard).
struct PrecisionControl: View {
  let level: Int
  let changed: (Int) -> Void

  private static let names = ["Broadest", "Broader", "Balanced", "Stricter", "Strictest"]

  var body: some View {
    HStack(spacing: 8) {
      Text("Broader").font(AITheme.font(11)).foregroundStyle(AITheme.body)
      Slider(value: Binding(get: { Double(level) }, set: { changed(Int($0.rounded())) }),
             in: 0...4, step: 1) {
        Text("Precision")
      }
      .labelsHidden()
      .accessibilityValue(Self.names[min(4, max(0, level))])
      Text("Stricter").font(AITheme.font(11)).foregroundStyle(AITheme.body)
    }
  }
}

struct ManagementView: View {
  @ObservedObject var model: ManagementModel
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var subfolders = true
  @State private var openPerson: Person?

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      StatusPill(line: model.status, onIndexAnyway: { model.indexAnyway() }) { model.setPaused($0) }
      if !model.status.sound.isEmpty {
        // "Sound: 12 of 40 clips · Speech: 8 of 40", also once it is done.
        Label(model.status.sound, systemImage: "speaker.wave.2")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .contentTransition(.numericText())
      }
      if !model.message.isEmpty {
        Text(model.message).font(AITheme.font(12)).foregroundStyle(AITheme.body)
      }
      section("Compute") {
        row("Where search runs",
            detail: "Auto uses Core ML (the Neural Engine and GPU) when it is faster than the CPU, and falls back on its own. Changing it does not re-index.") {
          Picker("Compute", selection: Binding(get: { model.compute }, set: { model.set("compute", Int64($0)) })) {
            Text("Auto").tag(Int(MV_AI_COMPUTE_AUTO.rawValue))
            Text("Core ML").tag(Int(MV_AI_COMPUTE_COREML.rawValue)).disabled(!model.coreMLAvailable)
            Text("CPU only").tag(Int(MV_AI_COMPUTE_CPU_ONLY.rawValue))
          }
          .pickerStyle(.menu).frame(width: 160)
        }
        // No "Search quality" picker (owner, 2026-09-28: one scale, not
        // two): the engine's Auto picks the larger model where Core ML runs
        // it quickly, the smaller one on CPU only. A Fast or High chosen
        // before stays as it was, said here with a way back to Auto; nothing
        // rewrites it behind the person's back.
        if model.quality != Int(MV_AI_QUALITY_AUTO.rawValue) {
          row("Search model", detail: legacyQualityDetail) {
            Button("Use Auto") { model.set("quality", Int64(MV_AI_QUALITY_AUTO.rawValue)) }
          }
        }
      }
      section("Search") {
        row("Precision",
            detail: "How closely a result must match what you type. Stricter shows only close matches and says “nothing found” rather than a near miss (a plane for “helicopter”); Broader shows more, including looser matches.") {
          PrecisionControl(level: model.precision) { model.setPrecision($0) }
            .frame(width: 240)
        }
      }
      section("Videos") {
        row("Index videos for",
            detail: model.audioReady
              ? "Pictures finds what a clip shows. Sound also finds what it sounds like (“dog barking”) and what is said. Each folder can differ, from its menu below."
              : "Pictures finds what a clip shows. To find videos by sound and speech, install Sound above.") {
          // Not a .segmented Picker: macOS ignores .disabled on its items, so
          // Sound and Both stayed clickable before the piece was in (owner
          // report, 2026-09-27). NSSegmentedControl disables per segment.
          VideoIndexControl(selection: model.videoIndex, audioReady: model.audioReady) {
            model.setVideoIndex($0)
          }
          .frame(width: 240)
          .help(model.audioReady ? "" : "Sound and Both need the Sound piece: install it above.")
        }
      }
      if model.photosSupported {
        section("Photos Library") { photosSection }
      }
      section("Indexed folders") {
        if model.folderRoots.isEmpty {
          Text("No folders yet. Open a folder and press ⌘F to index it, or add one here.")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
            .padding(12)
        }
        ForEach(model.folderRoots) { root in
          rootRow(root)
            .transition(.opacity.combined(with: .move(edge: .top)))
          Rectangle().fill(AITheme.hairline).frame(height: 1)
        }
        HStack {
          Button("Add a folder…") { model.addFolder(recursive: subfolders) }
          Toggle("and its subfolders", isOn: $subfolders).toggleStyle(.checkbox)
          Spacer()
        }
        .font(AITheme.font(12))
        .padding(12)
      }
      section("Index") {
        row("Search index",
            detail: "\(bytesText(model.indexBytes)) on disk" +
              (model.capBytes > 0 ? " of at most \(bytesText(UInt64(model.capBytes)))" : "") +
              ". Your thumbnails are kept when it is cleared.") {
          Picker("Index size limit", selection: Binding(get: { model.capBytes }, set: { model.set("index_cap_bytes", $0) })) {
            ForEach([Int64(2), 5, 10, 20, 50], id: \.self) { gb in
              Text("\(gb) GB").tag(gb * 1_000_000_000)
            }
          }
          .pickerStyle(.menu).frame(width: 110)
          Button("Clear index…") { model.confirming = .clearIndex }
        }
        if model.confirming == .clearIndex {
          confirmRow("Delete every indexed moment? Folders stay remembered and will be indexed again.",
                     action: "Clear index", destructive: true) { model.clearIndex() }
        }
        row("On battery",
            detail: "Indexing pauses on battery below this charge and resumes on power. "
                    + "“Index anyway” beside the status carries on until the Mac is next on power.") {
          Picker("Pause on battery", selection: Binding(get: { model.batteryPercent },
                                                        set: { model.set("pause_on_battery_percent", Int64($0)) })) {
            Text("Always pause").tag(100)
            Text("Below 50 %").tag(50)
            Text("Below 30 %").tag(30)
            Text("Below 20 %").tag(20)
            Text("Never pause").tag(0)
          }
          .pickerStyle(.menu).frame(width: 160)
        }
      }
      section("People") {
        row("Find people in your photos",
            detail: "Face data stays on this computer, is never shared, and can be deleted at any time. Off by default.") {
          Toggle("Find people in your photos", isOn: Binding(get: { model.facesOn }, set: { model.setFaces($0) }))
            .toggleStyle(.switch)
        }
        if model.confirming == .facesOff {
          confirmRow("Turn off people search? Every face, crop and name is deleted now.",
                     action: "Delete face data", destructive: true) { model.facesOffConfirmed() }
        }
        if model.facesOn && !model.facesReady {
          Text("Install People above to find faces. Until then nothing about faces is computed.")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
            .padding(.horizontal, 12).padding(.bottom, 10)
        }
        if model.facesOn && model.facesReady {
          PeopleGrid(model: model, open: { openPerson = $0 })
        }
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .animation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.86), value: model.roots)
    .animation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.86), value: model.confirming)
    .onAppear { model.appeared() }
    .onDisappear { model.disappeared() }
    .sheet(item: $openPerson) { person in
      PersonSheet(model: model, person: person) { openPerson = nil }
    }
  }

  struct MediaChoice: Identifiable {
    let value: UInt32
    let label: String
    var id: UInt32 { value }
  }
  static let mediaChoices = [
    MediaChoice(value: MV_AI_MEDIA_DEFAULT, label: "Default"),
    MediaChoice(value: MV_AI_MEDIA_PICTURES, label: "Pictures"),
    MediaChoice(value: MV_AI_MEDIA_SOUND, label: "Sound"),
    MediaChoice(value: MV_AI_MEDIA_BOTH, label: "Both"),
  ]

  static func mediaLabel(_ media: UInt32) -> String {
    switch media {
    case MV_AI_MEDIA_PICTURES: return "Videos: Pictures"
    case MV_AI_MEDIA_SOUND: return "Videos: Sound"
    case MV_AI_MEDIA_BOTH: return "Videos: Both"
    default: return "Videos: Default"
    }
  }

  /// A Fast or High set before the choice left Settings.
  private var legacyQualityDetail: String {
    let high = model.quality == Int(MV_AI_QUALITY_HIGH.rawValue)
    let name = model.modelName(model.quality)
    return "Set to \(high ? "High" : "Fast")" + (name.isEmpty ? "" : " (\(name))") + " earlier. "
      + "Auto chooses the model for this Mac and re-indexes in the background if it changes; "
      + "the old index answers until the new one is ready."
  }

  // MARK: the Photos library (issue #72)

  private var photosDenied: Bool { model.photosAccess == .denied || model.photosAccess == .restricted }

  @ViewBuilder
  private var photosSection: some View {
    if let root = model.photosRoot {
      photosRow(root)
    } else if !PhotosLibrary.declared {
      row("Search your Photos library",
          detail: "This version of MediaViewer cannot ask for access to Photos. Update MediaViewer to search your Photos library.") {
        EmptyView()
      }
    } else if photosDenied {
      row("Search your Photos library",
          detail: model.photosAccess == .restricted
            ? "Access to Photos is restricted on this Mac (Screen Time or a profile)."
            : "MediaViewer is not allowed to read your Photos library. Turn it on in System Settings → Privacy & Security → Photos, then come back here.") {
        if model.photosAccess == .denied {
          Button("Open Privacy Settings") { PhotosLibrary.openPrivacySettings() }
        }
      }
    } else {
      row("Search your Photos library",
          detail: "Find photos and videos in the Photos app, iCloud Photos included, by describing them. "
            + "Only what is already on this Mac is read: nothing is downloaded, nothing is sent anywhere, "
            + "and your library is never changed.") {
        Button(model.photosAdding ? "Adding…" : "Add Photos Library") { model.addPhotosLibrary() }
          .disabled(model.photosAdding)
          .help(model.photosAccess == .notDetermined
                ? "macOS asks once whether MediaViewer may read your Photos library."
                : "Index your Photos library in the background.")
      }
    }
  }

  private func photosRow(_ root: RootRow) -> some View {
    // Access as the pack last saw it, or as PhotoKit says now (a change in
    // System Settings shows before the next scan).
    let off = photosDenied || root.access == "denied" || root.access == "restricted"
    // "done" is what was read and embedded; an iCloud-only asset is handled
    // too (nothing on this Mac to read), so the bar counts both.
    let indexed = max(0, root.done)
    let handled = indexed + max(0, root.unavailable)
    return VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: 12) {
        VStack(alignment: .leading, spacing: 4) {
          HStack(spacing: 6) {
            Image(systemName: "photo.on.rectangle.angled").foregroundStyle(AITheme.body)
            Text("Photos Library").font(AITheme.font(13)).foregroundStyle(AITheme.title)
            if root.access == "limited" {
              Text("selected photos only").font(AITheme.font(11)).foregroundStyle(AITheme.body)
            }
          }
          ProgressView(value: root.assets == 0 ? 0 : min(1, Double(handled) / Double(root.assets)))
            .progressViewStyle(.linear)
            .tint(root.enabled && !off ? .accentColor : .secondary)
          HStack(spacing: 0) {
            Text("\(countText(UInt64(indexed))) of \(countText(UInt64(max(0, root.assets)))) indexed")
            if root.unavailable > 0 {
              Text(" · \(countText(UInt64(root.unavailable))) only in iCloud")
                .help("Optimize Mac Storage keeps these originals in iCloud and no picture of them on this Mac, "
                      + "so there is nothing to read without downloading. They are indexed once Photos has them "
                      + "here (MediaViewer checks each of them when it starts). An iCloud-only video is found by its poster.")
            }
            Text(root.enabled ? "" : " · paused")
          }
          .font(AITheme.font(11)).foregroundStyle(AITheme.body)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        Button(root.enabled ? "Pause" : "Resume") { model.setRootEnabled(root.id, !root.enabled) }
          .disabled(off)
        Menu {
          ForEach(Self.mediaChoices) { choice in
            Button {
              model.setRootMedia(root.id, choice.value)
            } label: {
              if root.media == choice.value { Label(choice.label, systemImage: "checkmark") } else { Text(choice.label) }
            }
            .disabled(choice.value & MV_AI_MEDIA_SOUND != 0 && !model.audioReady)
          }
        } label: {
          Text(Self.mediaLabel(root.media))
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("What the library's videos are indexed for")
        Button("Rescan") { model.rescan(root.id) }.disabled(off)
        if model.confirming == .removeRoot(root.id) {
          Button("Remove from index", role: .destructive) { model.removeRoot(root.id) }
          Button("Cancel") { model.confirming = nil }
        } else {
          Button("Remove…") { model.confirming = .removeRoot(root.id) }
            .help("Stop searching your Photos library and delete its rows from the index. Your library is not touched.")
        }
      }
      if off {
        HStack(spacing: 8) {
          Image(systemName: "exclamationmark.triangle").foregroundStyle(.orange)
          Text("MediaViewer can no longer read your Photos library. What was indexed can still be found, "
               + "but nothing new is added until access is back.")
            .fixedSize(horizontal: false, vertical: true)
          Spacer(minLength: 0)
          Button("Open Privacy Settings") { PhotosLibrary.openPrivacySettings() }
        }
        .font(AITheme.font(11)).foregroundStyle(AITheme.body)
        .padding(.top, 8)
      }
    }
    .font(AITheme.font(12))
    .padding(12)
  }

  private func rootRow(_ root: RootRow) -> some View {
    HStack(spacing: 12) {
      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 6) {
          Image(systemName: "folder").foregroundStyle(AITheme.body)
          Text(root.path).font(AITheme.font(13)).foregroundStyle(AITheme.title)
            .lineLimit(1).truncationMode(.middle)
          if root.recursive {
            Text("and subfolders").font(AITheme.font(11)).foregroundStyle(AITheme.body)
          }
        }
        ProgressView(value: root.assets == 0 ? 0 : min(1, Double(root.done) / Double(root.assets)))
          .progressViewStyle(.linear)
          .tint(root.enabled ? .accentColor : .secondary)
        Text("\(countText(UInt64(max(0, root.done)))) of \(countText(UInt64(max(0, root.assets)))) · \(bytesText(UInt64(max(0, root.bytes))))" +
             (root.enabled ? "" : " · paused"))
          .font(AITheme.font(11)).foregroundStyle(AITheme.body)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      Button(root.enabled ? "Pause" : "Resume") { model.setRootEnabled(root.id, !root.enabled) }
      Menu {
        ForEach(Self.mediaChoices) { choice in
          Button {
            model.setRootMedia(root.id, choice.value)
          } label: {
            if root.media == choice.value { Label(choice.label, systemImage: "checkmark") } else { Text(choice.label) }
          }
          .disabled(choice.value & MV_AI_MEDIA_SOUND != 0 && !model.audioReady)
        }
      } label: {
        Text(Self.mediaLabel(root.media))
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
      .help("What this folder's videos are indexed for")
      Button("Rescan") { model.rescan(root.id) }
      if model.confirming == .removeRoot(root.id) {
        Button("Remove from index", role: .destructive) { model.removeRoot(root.id) }
        Button("Cancel") { model.confirming = nil }
      } else {
        Button("Remove…") { model.confirming = .removeRoot(root.id) }
          .help("Forget this folder and delete its rows from the index. Your files are not touched.")
      }
    }
    .font(AITheme.font(12))
    .padding(12)
  }

  private func section<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
    VStack(alignment: .leading, spacing: 0) {
      Text(title).font(AITheme.font(15)).fontWeight(.semibold).foregroundStyle(AITheme.title)
        .padding(.top, 8).padding(.bottom, 6)
      VStack(alignment: .leading, spacing: 0) { content() }
        .background(RoundedRectangle(cornerRadius: 8).fill(AITheme.surface))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(AITheme.hairline, lineWidth: 1))
    }
  }

  private func row<Content: View>(_ title: String, detail: String,
                                  @ViewBuilder _ content: () -> Content) -> some View {
    HStack(spacing: 20) {
      VStack(alignment: .leading, spacing: 3) {
        Text(title).font(AITheme.font(14)).foregroundStyle(AITheme.title)
        Text(detail).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      content().labelsHidden()
    }
    .padding(12)
  }

  private func confirmRow(_ text: String, action: String, destructive: Bool,
                          perform: @escaping () -> Void) -> some View {
    HStack(spacing: 10) {
      Text(text).font(AITheme.font(12)).foregroundStyle(AITheme.title)
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, alignment: .leading)
      Button(action, role: destructive ? .destructive : nil, action: perform)
      Button("Cancel") { model.confirming = nil }.keyboardShortcut(.cancelAction)
    }
    .padding(12)
    .background(Color.accentColor.opacity(0.08))
    .transition(.opacity)
  }
}

/// Settings "Index videos for": Pictures · Sound · Both. An NSSegmentedControl
/// so Sound and Both are really disabled until the Sound piece is loaded
/// (setEnabled(_:forSegment:)); a click the control lets through is still
/// checked against `audioReady` before it reaches `choose`.
struct VideoIndexControl: NSViewRepresentable {
  let selection: Int      // MV_AI_MEDIA_PICTURES / _SOUND / _BOTH
  let audioReady: Bool
  let choose: (Int) -> Void

  fileprivate static let values = [Int(MV_AI_MEDIA_PICTURES), Int(MV_AI_MEDIA_SOUND), Int(MV_AI_MEDIA_BOTH)]

  func makeCoordinator() -> Coordinator { Coordinator(self) }

  func makeNSView(context: Context) -> NSSegmentedControl {
    let control = NSSegmentedControl(labels: ["Pictures", "Sound", "Both"], trackingMode: .selectOne,
                                     target: context.coordinator, action: #selector(Coordinator.changed(_:)))
    control.segmentDistribution = .fillEqually
    control.setAccessibilityLabel("Index videos for")
    return control
  }

  func updateNSView(_ control: NSSegmentedControl, context: Context) {
    context.coordinator.parent = self
    for i in 1..<Self.values.count { control.setEnabled(audioReady, forSegment: i) }
    control.selectedSegment = Self.values.firstIndex(of: selection) ?? 0
  }

  @MainActor
  final class Coordinator: NSObject {
    var parent: VideoIndexControl
    init(_ parent: VideoIndexControl) { self.parent = parent }

    @objc func changed(_ sender: NSSegmentedControl) {
      let i = sender.selectedSegment
      guard i >= 0, i < VideoIndexControl.values.count, i == 0 || parent.audioReady else {
        sender.selectedSegment = VideoIndexControl.values.firstIndex(of: parent.selection) ?? 0
        return
      }
      parent.choose(VideoIndexControl.values[i])
    }
  }
}
