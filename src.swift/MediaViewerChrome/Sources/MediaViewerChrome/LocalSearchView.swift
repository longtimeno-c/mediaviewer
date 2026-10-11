// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Local search and the command bar's indexing pill (docs/design/17 "The AI
// pack", Milestone H PRs 20–24), the Mac twin of the Windows Local search page.
//
// The base app only installs, removes and loads the pack: everything the pack
// does (the search panel, the management view embedded below) is its own
// chrome, AI.bundle, loaded by src/shell/addons_mac.mm. With no piece installed
// this section is the only surface — no command, key, menu or bar item exists
// (the chrome brief's "the base app must not change at all"). On an Intel Mac
// the whole section is absent: the pack is arm64 only.
//
// Downloads go through AddonChannel (AddonsView.swift): the same fixed release
// URLs, signed manifest first, archive hash before it is opened, nothing about
// the user or their files in the request (rule 6).
import AppKit
import Foundation
import SwiftUI
import MVChromeBridge

@MainActor
final class LocalSearchStore: ObservableObject {
  static let shared = LocalSearchStore()

  /// One installable piece of the AI family (docs/design/17 per-piece Install/Remove).
  struct Piece: Identifiable, Equatable {
    let id: String          // "ai" (Core), "ai-faces" (People), "ai-audio" (Sound)
    let title: String
    let detail: String
    let required: Bool
    var installed = false
    var version = ""
    var size = 0            // installed bytes, from the store
    var state = ""          // ok | needs_update | invalid
    var probe: AddonChannel.Probe?
    var checking = false

    var channel: AddonChannel { AddonChannel(name: "mediaviewer-addon-\(id)-macos") }
    var offeredBytes: (archive: Int, installed: Int)? {
      if case .available(let a, let i, _, _, _) = probe { return (a, i) }
      return nil
    }
    /// A newer published version of an installed piece, or nil.
    var updateVersion: String? {
      guard installed, let v = probe?.version, !v.isEmpty, AddonChannel.isNewer(v, than: version) else { return nil }
      return v
    }
  }

  /// Whether this Mac can run the pack at all (arm64). Constant for a run.
  let supported: Bool = mv_addon2_supported("ai")
  /// Another window's process hosts the add-ons (2026-10-07, docs/design/18 "One
  /// host process"): this one searches through the pack's read-only reader,
  /// started on the first ⌘F or when Settings opens, and installs, updates
  /// and removes nothing. Nothing is read at launch: reading what is
  /// installed hashes over a gigabyte. Constant for a run.
  let elsewhere: Bool = mv_addon2_elsewhere()

  @Published private(set) var pieces: [Piece] = [
    Piece(id: "ai", title: "Core",
          detail: "The search engine and the picture and text models. Required.", required: true),
    Piece(id: "ai-faces", title: "People",
          detail: "Face models for “photos of Sam”. Optional; off until you turn it on.", required: false),
    // 2026-09-27: sounds (“dog barking”) and speech (“happy birthday”) in videos.
    Piece(id: "ai-audio", title: "Sound",
          detail: "Find videos by what you hear: sounds like “dog barking” and words that are said. Optional.",
          required: false),
  ]
  /// The first installed-state read has landed. It verifies every installed
  /// file (seconds for the 2.3 GB pack), and the channel probe used to answer
  /// first: Settings offered "Install local search" on every start and a click
  /// downloaded it all again (owner report, 2026-09-27). Nothing offers an
  /// install or a download until this is true.
  @Published private(set) var stateKnown = false
  @Published private(set) var used: UInt64 = 0
  @Published private(set) var ceiling: UInt64 = 3_000_000_000
  @Published private(set) var loaded = false
  @Published private(set) var loading = false
  @Published private(set) var loadError = ""
  /// The same, in words (mv_addon2_load_reason): Settings' status line.
  @Published private(set) var loadReason = ""
  /// The piece downloading or installing now. Installs run one at a time (no
  /// fight over bandwidth, and each piece's 3 GB check sees the one before it
  /// landed); more clicks queue behind it in `queued`.
  @Published private(set) var busyPiece: String?
  /// The busy piece's install progress (Settings' bar).
  @Published private(set) var phase: AddonChannel.Phase?
  /// Clicked Install, waiting their turn: Core first, then the others in the
  /// order clicked (owner request, 2026-09-28: Install on all three at once).
  @Published private(set) var queued: [String] = []
  /// What each install of the current batch said, shown together.
  private var batchNotes: [String] = []
  /// Between one install and the next: the installed sizes are being read
  /// again, so the next piece's 3 GB check counts the one that just landed.
  @Published private(set) var advancing = false
  @Published var message = ""
  @Published var confirmingRemove: String?
  /// Pieces whose removal is queued on the host's add-on queue (it unloads,
  /// then deletes up to 3 GB). Their rows read "Removing…" at once and offer
  /// Install again the moment the store no longer lists them: before, the
  /// row still read Installed until the next start (owner report, 2026-09-27).
  @Published private(set) var removing: Set<String> = []
  /// Moves whenever the chrome is (re)attached, so the embedded view is rebuilt.
  @Published private(set) var chromeGeneration = 0
  /// docs/design/23: Final Cut Pro search (mv_fcp_state): 0 not in this app or Mac,
  /// 1 off, 2 on, 3 on once allowed in Login Items. The agent and extension
  /// ship in MediaViewer.app; turning this on only registers them.
  @Published private(set) var finalCut: Int32 = 0
  @Published private(set) var finalCutBusy = false

  // The command bar pill (mv.ai.1 status, no-block).
  @Published private(set) var pillVisible = false
  @Published private(set) var pillText = ""
  @Published private(set) var pillProgress: Double = 0
  @Published private(set) var pillSpinning = false
  /// Waiting on battery: the pill offers "Index anyway".
  @Published private(set) var pillOnBattery = false

  private var timer: Timer?
  private var ticks = 0
  private var probed = false
  private var checkingRemovals = false
  /// The search icon was clicked while the pack was still starting.
  private var openWhenLoaded = false
  /// What to say once each queued removal has landed.
  private var removedText: [String: String] = [:]

  private init() {
    guard supported else { return }
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    if !elsewhere { refresh() }  // a later window reads it when Settings opens
  }

  var core: Piece { pieces[0] }
  var coreInstalled: Bool { core.installed }

  private func poll() {
    ticks += 1
    // Waiting on Login Items approval: notice it being given while Settings is open.
    if finalCut == 3, !finalCutBusy, SettingsStore.shared.visible, ticks % 8 == 0 { refreshFinalCut() }
    if !removing.isEmpty, ticks % 2 == 0 { checkRemovals() }
    // Nothing loaded and nothing loading: every 2 s is enough, unless
    // Settings is open or a piece is being installed (it shows the change).
    if !loaded, !loading, busyPiece == nil, !SettingsStore.shared.visible, ticks % 4 != 0 { return }
    let l = mv_addon2_loaded("ai")
    let busy = mv_addon2_loading("ai")
    if l != loaded {
      loaded = l
      chromeGeneration += 1
    }
    if busy != loading { loading = busy }
    if openWhenLoaded, loaded || !loading {
      // Attached: open as the click asked. The load failed: drop it.
      openWhenLoaded = false
      if loaded { openSearch() }
    }
    let err = AddonStore.readString { mv_addon2_load_error("ai", $0, $1) }
    if err != loadError { loadError = err }
    let why = AddonStore.readString { mv_addon2_load_reason("ai", $0, $1) }
    if why != loadReason { loadReason = why }
    // The pill: at most 2 Hz, and only while the pack is loaded.
    guard loaded, ticks % 2 == 0 else {
      if !loaded && pillVisible { pillVisible = false }
      return
    }
    var s = mv_chrome_ai_status()
    guard mv_addon2_ai_status(&s) else {
      if pillVisible { pillVisible = false }
      return
    }
    // Indexing (1) or waiting for the viewer / battery (3): work in progress.
    let visible = s.state == 1 || s.state == 3
    if visible != pillVisible {
      // The pill fades (and, without Reduce Motion, scales) in and out.
      let reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
      withAnimation(reduce ? .easeOut(duration: 0.15) : .spring(response: 0.32, dampingFraction: 0.86)) {
        pillVisible = visible
      }
    }
    guard visible else { return }
    let text = Self.pillLine(s)
    if text != pillText { pillText = text }
    let p = s.assets_total == 0 ? 0 : min(1, Double(s.assets_done) / Double(s.assets_total))
    if abs(p - pillProgress) > 0.001 { pillProgress = p }
    let spin = s.state == 1
    if spin != pillSpinning { pillSpinning = spin }
    let battery = s.state == 3 && s.yield_reason == 2
    if battery != pillOnBattery { pillOnBattery = battery }
  }

  nonisolated static func count(_ n: UInt64) -> String {
    let f = NumberFormatter()
    f.numberStyle = .decimal
    return f.string(from: NSNumber(value: n)) ?? String(n)
  }

  nonisolated static func pillLine(_ s: mv_chrome_ai_status) -> String {
    if s.state == 3 {
      switch s.yield_reason {
      case 1: return "Indexing paused while a video plays"
      case 2: return "Indexing paused on battery"
      case 3: return "Indexing paused to keep playback smooth"
      default: return "Indexing paused"
      }
    }
    // Pictures first; then the Sound piece's clips (sounds, then speech).
    if s.assets_done >= s.assets_total, s.sound_done < s.sound_total {
      return "Indexing sound \(count(s.sound_done)) of \(count(s.sound_total)) clips"
    }
    if s.assets_done >= s.assets_total, s.speech_done < s.speech_total {
      return "Indexing speech \(count(s.speech_done)) of \(count(s.speech_total)) clips"
    }
    return "Indexing \(count(s.assets_done)) of \(count(s.assets_total))"
  }

  /// "downloads ~1.2 GB": decimal units, as Finder counts.
  nonisolated static func sizeText(_ bytes: Int) -> String {
    let b = Double(max(bytes, 0))
    if b >= 950_000_000 { return String(format: "%.1f GB", b / 1_000_000_000) }
    return "\(max(1, Int((b / 1_000_000).rounded()))) MB"
  }

  /// `then` runs on the main actor once the read has landed.
  func refresh(then: (@MainActor @Sendable () -> Void)? = nil) {
    guard supported else { return }
    Task.detached {
      var read: [String: [String: Any]] = [:]
      for id in ["ai", "ai-faces", "ai-audio"] {
        let json = AddonStore.readString { mv_addon2_state_json(id, $0, $1) }
        read[id] = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      }
      var u: UInt64 = 0, c: UInt64 = 0
      let room = mv_addon2_family_usage("ai", &u, &c)
      let fcp = mv_fcp_state()
      let states = read, used = u, ceiling = c
      await MainActor.run {
        if !self.finalCutBusy { self.finalCut = fcp }
        for i in self.pieces.indices {
          // A queued removal is not undone by a read that raced it.
          if self.removing.contains(self.pieces[i].id) { continue }
          let obj = states[self.pieces[i].id] ?? [:]
          self.pieces[i].installed = obj["installed"] as? Bool ?? false
          self.pieces[i].version = obj["version"] as? String ?? ""
          self.pieces[i].size = obj["size"] as? Int ?? 0
          self.pieces[i].state = obj["state"] as? String ?? ""
        }
        if room {
          self.used = used
          if ceiling > 0 { self.ceiling = ceiling }
        }
        self.stateKnown = true
        then?()
      }
    }
  }

  /// Final Cut Pro's state alone (an XPC call to the service manager).
  func refreshFinalCut() {
    Task.detached {
      let fcp = mv_fcp_state()
      await MainActor.run { if !self.finalCutBusy { self.finalCut = fcp } }
    }
  }

  /// Registers or unregisters the search agent and shows or hides the
  /// extension in Final Cut Pro. Nothing is downloaded: the pieces are in the
  /// app, and the bulk (Core) is already installed.
  func setFinalCut(_ on: Bool) {
    guard !finalCutBusy else { return }
    finalCutBusy = true
    Task.detached {
      let after = mv_fcp_set_enabled(on)
      await MainActor.run {
        self.finalCutBusy = false
        self.finalCut = after
      }
    }
  }

  /// Settings opening asks the channel for each piece (two small GETs each).
  /// Nothing is downloaded until a button is clicked.
  func probe(force: Bool = false) {
    // A later window offers nothing to install: no channel request either.
    guard supported, !elsewhere, force || !probed else { return }
    probed = true
    for piece in pieces { probe(piece: piece.id) }
  }

  /// One piece's channel, e.g. after it was removed and no size is known to
  /// offer an Install with.
  func probe(piece id: String) {
    guard supported, let i = pieces.firstIndex(where: { $0.id == id }), !pieces[i].checking else { return }
    pieces[i].checking = true
    let channel = pieces[i].channel
    Task.detached {
      let result = await channel.probe()
      await MainActor.run {
        guard let j = self.pieces.firstIndex(where: { $0.id == id }) else { return }
        self.pieces[j].checking = false
        self.pieces[j].probe = result
      }
    }
  }

  /// Reads only the pieces being removed (nothing left to hash once a piece
  /// is gone), at most one read in flight.
  private func checkRemovals() {
    guard !checkingRemovals, !removing.isEmpty else { return }
    checkingRemovals = true
    let ids = Array(removing)
    Task.detached {
      var gone: [String] = []
      for id in ids {
        let json = AddonStore.readString { mv_addon2_state_json(id, $0, $1) }
        let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
        if !(obj["installed"] as? Bool ?? false) { gone.append(id) }
      }
      var u: UInt64 = 0, c: UInt64 = 0
      let room = !gone.isEmpty && mv_addon2_family_usage("ai", &u, &c)
      let done = gone, used = u
      await MainActor.run {
        self.checkingRemovals = false
        guard !done.isEmpty else { return }
        for id in done {
          self.removing.remove(id)
          if let i = self.pieces.firstIndex(where: { $0.id == id }) {
            self.pieces[i].installed = false
            self.pieces[i].size = 0
            self.pieces[i].version = ""
            self.pieces[i].state = ""
            if self.pieces[i].offeredBytes == nil { self.probe(piece: id) }
          }
          if let text = self.removedText.removeValue(forKey: id) { self.message = text }
        }
        if room { self.used = used }
      }
    }
  }

  /// The 3 GB rule (docs/design/17), before anything downloads: the piece's installed
  /// size, plus what the family already uses minus the copy it replaces.
  func refusal(for piece: Piece) -> String? {
    guard let bytes = piece.offeredBytes?.installed, ceiling > 0 else { return nil }
    let after = Int64(used) - Int64(piece.installed ? piece.size : 0) + Int64(bytes)
    guard after > Int64(ceiling) else { return nil }
    return "\(piece.title) needs about \(Self.sizeText(bytes)), and Local search may use at most "
      + "\(Self.sizeText(Int(ceiling))) in all. Remove another piece first."
  }

  /// Downloading, installing, or waiting its turn.
  func isPending(_ id: String) -> Bool { busyPiece == id || queued.contains(id) }
  /// Core is installed or on its way in this batch: a piece may queue behind it.
  var coreComing: Bool { coreInstalled || isPending("ai") }
  /// Anything installing or queued.
  var anyPending: Bool { busyPiece != nil || !queued.isEmpty || advancing }

  /// What "Install all" fetches: every piece not installed that the channel offers.
  var installAllIDs: [String] {
    pieces.filter { !$0.installed && $0.offeredBytes != nil && !removing.contains($0.id) && !isPending($0.id) }
      .map(\.id)
  }

  func installAll() {
    for id in installAllIDs { install(id) }
  }

  /// What "Update all" fetches: every installed piece the channel has a newer
  /// version of (Core first, as the queue orders it). Owner, 2026-10-03: the
  /// pieces had been updated one by one and the 1 GB Core left behind, so the
  /// People grid ran a chrome whose fix shipped in the Core it did not have.
  var updateAllIDs: [String] {
    pieces.filter { $0.updateVersion != nil && !removing.contains($0.id) && !isPending($0.id) }.map(\.id)
  }

  /// The version "Update all" goes to, when every piece agrees; "" otherwise.
  var updateAllVersion: String {
    let versions = Set(pieces.filter { updateAllIDs.contains($0.id) }.compactMap(\.updateVersion))
    return versions.count == 1 ? versions.first! : ""
  }

  /// Core is behind the channel and not yet on its way: a piece update or
  /// install queues it first, so the pieces never run ahead of the engine
  /// they were built with.
  var coreBehind: Bool { core.updateVersion != nil && !isPending("ai") && !removing.contains("ai") }

  func updateAll() {
    for id in updateAllIDs { install(id) }
  }

  /// Queues the piece and returns at once; the queue runs one install at a
  /// time, Core first. A piece clicked before Core is installed waits for
  /// Core, and is dropped with a note if Core does not install.
  /// Settings opened: what is installed, and in a later window its reader
  /// (mv_addon2_reader_start; at most every 10 s), so the pack's view shows.
  func settingsShown() {
    refresh()
    probe()
    if elsewhere { _ = mv_addon2_reader_start() }
  }

  func install(_ id: String) {
    guard stateKnown, !removing.contains(id), !isPending(id),
          let piece = pieces.first(where: { $0.id == id }) else { return }
    if !piece.required && !coreComing {
      message = "Install Core first."
      return
    }
    // Checked again before its download starts, against what is installed then.
    if let refusal = refusal(for: piece) {
      message = refusal
      return
    }
    if !anyPending { batchNotes = [] }
    // A piece behind a Core that is itself behind: Core first (its 3 GB check
    // runs when its turn comes, like every queued piece's).
    if !piece.required && coreBehind { queued.insert("ai", at: 0) }
    if piece.required { queued.insert(id, at: 0) } else { queued.append(id) }
    startNext()
  }

  /// Takes a piece out of the queue before it starts. Without Core on its
  /// way, the pieces waiting for it go too.
  func cancelQueued(_ id: String) {
    queued.removeAll { $0 == id }
    if !coreComing, queued.contains(where: { $0 != "ai" }) {
      queued.removeAll()
      note("Queued pieces were cancelled: they need Core.")
    }
  }

  private func note(_ text: String) {
    batchNotes.append(text)
    message = batchNotes.joined(separator: " ")
  }

  /// Starts the next queued piece if nothing is installing.
  private func startNext() {
    guard busyPiece == nil, !advancing else { return }
    while !queued.isEmpty {
      let id = queued.removeFirst()
      guard let piece = pieces.first(where: { $0.id == id }), !removing.contains(id) else { continue }
      if !piece.required && !coreInstalled {
        note("\(piece.title) was not installed: it needs Core.")
        continue
      }
      // The 3 GB rule, now that the pieces before it in the batch have landed.
      if let refusal = refusal(for: piece) {
        note(refusal)
        continue
      }
      begin(piece)
      return
    }
  }

  private func begin(_ piece: Piece) {
    let id = piece.id
    busyPiece = id
    phase = nil
    // An update installs beside the running copy (the store keeps it until the
    // next start). A new Core takes over then: its chrome cannot be replaced in
    // the running app. A new piece is picked up at once by "reload".
    let update = piece.updateVersion
    let coreRunning = loaded
    message = (batchNotes + [update.map { "Downloading \(piece.title) \($0)…" } ?? "Downloading \(piece.title)…"])
      .joined(separator: " ")
    let channel = piece.channel
    let title = piece.title
    Task.detached {
      let result: String
      let ok: Bool
      do {
        try await channel.downloadAndInstall { p in
          // Only while this piece is still the busy one: a late update must
          // not land on the next piece's row.
          Task { @MainActor in if self.busyPiece == id { self.phase = p } }
        }
        result = "\(title) installed."
        ok = true
      } catch is AddonChannel.NotPublished {
        result = "\(title) is not published for download yet."
        ok = false
      } catch let e as AddonChannel.AddonError {
        result = e.text
        ok = false
      } catch {
        result = "\(title) could not be downloaded. Check the connection and try again."
        ok = false
      }
      await MainActor.run {
        var text = result
        self.busyPiece = nil
        self.phase = nil
        if ok {
          // Core loads now (verified again, on a worker); a new piece is
          // picked up by the loaded pack at once ("reload").
          if id == "ai" && coreRunning, let v = update {
            text = "Local search \(v) is installed. Restart MediaViewer to use it."
            AddonRestart.shared.needed("Local search \(v)")
          } else if id == "ai" {
            if !mv_addon2_load("ai") { text = "Local search is installed but could not be started." }
          } else if self.loaded || self.loading {
            // Also a Core installed earlier in this batch and still starting:
            // its load reads the pieces as they are then.
            _ = mv_addon2_reload("ai")
          }
          if let v = update, text == result { text = "\(title) updated to \(v)." }
        } else if id == "ai" && !self.coreInstalled {
          // Pieces clicked with Core cannot install without it.
          let dropped = self.queued.filter { $0 != "ai" }
            .compactMap { q in self.pieces.first(where: { $0.id == q })?.title }
          self.queued.removeAll()
          if !dropped.isEmpty {
            text += " \(dropped.joined(separator: " and ")) \(dropped.count == 1 ? "was" : "were") not installed: "
              + "\(dropped.count == 1 ? "it needs" : "they need") Core."
          }
        }
        self.note(text)
        // The next piece starts once the installed sizes are read again, so
        // its 3 GB check counts the piece that just landed.
        self.advancing = true
        self.refresh {
          self.advancing = false
          self.startNext()
        }
      }
    }
  }

  func remove(_ id: String, keepData: Bool) {
    confirmingRemove = nil
    let title = pieces.first(where: { $0.id == id })?.title ?? id
    if id == "ai" { openWhenLoaded = false }
    // Final Cut Pro search hosts Core: without it, the agent and panel go too.
    if id == "ai", finalCut == 2 || finalCut == 3 { setFinalCut(false) }
    if id == "ai" && loaded {
      // The embedded management view stops using the table now, not at the
      // next poll: the pack is unloaded by the call below.
      loaded = false
      chromeGeneration += 1
      if pillVisible { pillVisible = false }
    }
    // On the main thread: unloading Core is [main-thread] in the host (the
    // chrome shuts down first); the store work itself is queued there, and
    // checkRemovals() sees it land. Removing Core removes its pieces too.
    guard mv_addon2_remove(id, keepData) else {
      message = "\(title) will finish uninstalling the next time MediaViewer starts."
      refresh()
      return
    }
    let ids = id == "ai" ? pieces.filter(\.installed).map(\.id) : [id]
    removing.formUnion(ids)
    for other in ids { removedText[other] = nil }
    removedText[id] = id == "ai"
      ? "Local search removed." + (keepData ? " The search index was kept, so a reinstall can search right away." : "")
      : "\(title) removed. Install it again here whenever you like."
    message = id == "ai" ? "Removing Local search…" : "Removing \(title)…"
    checkRemovals()
  }

  func openSearch() { _ = mv_addon2_run_command("search_open") }

  /// The path bar's search icon shows whenever Local search is on its way to
  /// being usable: loaded, or verifying and starting at launch (seconds for
  /// the 2.3 GB pack). It replaced the gallery search bar, so it must not be
  /// missing just because the pack is still starting (owner, 2026-09-28).
  var searchAvailable: Bool { loaded || loading }

  /// The icon's click: the panel now, or the moment the pack attaches.
  func openSearchWhenReady() {
    if loaded {
      openSearch()
    } else if loading {
      openWhenLoaded = true
    }
  }
  /// Ignore the battery pause until the Mac is next on power (not saved).
  func indexAnyway() { _ = mv_addon2_run_command("index_anyway") }

  /// Settings in a later window, for both add-on sections (the Windows wording).
  nonisolated static let elsewhereText =
    "Add-ons are installed, updated and removed in the first MediaViewer window you opened. "
    + "To manage them here, close every MediaViewer window, then open this one again."
}

/// Settings → Local search.
struct LocalSearchSection: View {
  @ObservedObject var store = LocalSearchStore.shared
  @ObservedObject var settings = SettingsStore.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    if store.supported {
      content
        .onAppear { if settings.visible { store.settingsShown() } }
        .onChange(of: settings.visible) { _, visible in
          if visible { store.settingsShown() }
        }
    }
  }

  private var content: some View {
    VStack(alignment: .leading, spacing: 10) {
      // An item of Settings → Add-ons, titled like Import.
      Text("Local search").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      if !store.stateKnown {
        // Quiet, and no button: nothing is offered for download while what
        // is installed is still being read.
        note("Checking installed add-ons…")
      } else if store.elsewhere {
        elsewhereView
      } else {
        installed
      }
    }
    .animation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.86), value: store.confirmingRemove)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.loaded)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.stateKnown)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.removing)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.pieces)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.queued)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.busyPiece)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.finalCut)
  }

  @ViewBuilder
  private var installed: some View {
    if !store.coreInstalled {
      introCard
    }
    if store.coreInstalled, !store.anyPending, store.updateAllIDs.count > 1 {
      updateAllRow
        .transition(.opacity)
    }
    VStack(spacing: 0) {
      ForEach(store.pieces) { piece in
        pieceRow(piece)
        if piece.id != store.pieces.last?.id {
          Rectangle().fill(MVTheme.hairline).frame(height: 1)
        }
      }
    }
    .background(RoundedRectangle(cornerRadius: 8).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 8).stroke(MVTheme.hairline, lineWidth: 1))
    // docs/design/23: offered once Core is installed, on a Mac that carries the pieces.
    if store.coreInstalled, !store.removing.contains("ai"), store.finalCut != 0 {
      finalCutRow
        .background(RoundedRectangle(cornerRadius: 8).fill(MVTheme.surface))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(MVTheme.hairline, lineWidth: 1))
        .transition(.opacity)
    }
    if let id = store.confirmingRemove {
      removeConfirm(id)
        .transition(.opacity.combined(with: .move(edge: .top)))
    }
    if !store.message.isEmpty {
      Text(store.message).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
        .transition(.opacity)
    }
    AddonRestartButton()
    if store.loading {
      HStack(spacing: 8) {
        ProgressView().controlSize(.small)
        Text("Starting Local search…").font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
    } else if store.coreInstalled && !store.loaded && !store.loadError.isEmpty {
      // The reason, as the command bar's alert says it (addons_mac.mm load_reason).
      Text("Local search couldn't start. "
           + (store.loadReason.isEmpty ? "Something went wrong (\(store.loadError))." : store.loadReason))
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
        .textSelection(.enabled)
    }
    // The pack's own management view: compute, precision, folders, index,
    // People. Owned by AI.bundle; rebuilt when the chrome is re-attached.
    if store.loaded {
      AddonSettingsEmbed(addonID: "ai")
        .id(store.chromeGeneration)
        .frame(maxWidth: .infinity, alignment: .leading)
        .transition(.opacity)
    }
  }

  /// A later window (docs/design/18 "One host process"): no Install, Update or
  /// Remove; it searches through the reader, whose own view (the pack's, in its
  /// read-only form) follows once it is loaded.
  @ViewBuilder
  private var elsewhereView: some View {
    note(store.coreInstalled ? LocalSearchStore.elsewhereText
         : "Local search is not installed. " + LocalSearchStore.elsewhereText)
    if store.coreInstalled {
      if store.loading {
        HStack(spacing: 8) {
          ProgressView().controlSize(.small)
          Text("Starting Local search…").font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        }
      } else if !store.loaded && !store.loadReason.isEmpty {
        note(store.loadError == "NOT_FOUND" ? store.loadReason : "Local search couldn't start. " + store.loadReason)
      }
      if store.loaded {
        AddonSettingsEmbed(addonID: "ai")
          .id(store.chromeGeneration)
          .frame(maxWidth: .infinity, alignment: .leading)
          .transition(.opacity)
      }
    }
  }

  /// Final Cut Pro search on or off (docs/design/23). Off, nothing of it runs and
  /// Final Cut Pro lists no MediaViewer extension.
  private var finalCutRow: some View {
    HStack(alignment: .firstTextBaseline, spacing: 16) {
      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 6) {
          Text("Final Cut Pro").font(MVTheme.font()).foregroundStyle(MVTheme.title)
          Text(store.finalCut == 2 ? "On" : store.finalCut == 3 ? "Waiting for approval" : "Off")
            .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        }
        Text(finalCutDetail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      if store.finalCutBusy {
        ProgressView().controlSize(.small)
      } else if store.finalCut == 1 {
        Button("Turn on") { store.setFinalCut(true) }
      } else {
        if store.finalCut == 3 {
          Button("Open Login Items…") { mv_fcp_open_login_items() }
        }
        Button("Turn off") { store.setFinalCut(false) }
      }
    }
    .padding(12)
  }

  private var finalCutDetail: String {
    switch store.finalCut {
    case 2:
      return "In Final Cut Pro, click the Extensions button in the browser and choose MediaViewer Search. "
        + "Drag results onto the timeline."
    case 3:
      return "Allow MediaViewer in System Settings → General → Login Items so Final Cut Pro can reach Local search."
    default:
      return "Search your footage from a panel inside Final Cut Pro, using this index. Nothing is downloaded."
    }
  }

  /// Every piece with a newer version, in one click, Core first.
  private var updateAllRow: some View {
    let all = store.pieces.filter { store.updateAllIDs.contains($0.id) }
    let archive = all.compactMap { $0.offeredBytes?.archive }.reduce(0, +)
    let v = store.updateAllVersion
    return HStack(alignment: .firstTextBaseline, spacing: 12) {
      Button {
        store.updateAll()
      } label: {
        Text((v.isEmpty ? "Update all" : "Update all to \(v)")
             + " (\(all.map(\.title).joined(separator: ", "))) — downloads ~\(LocalSearchStore.sizeText(archive))")
      }
      .help("Core first, then the others, one after another.")
      Spacer()
    }
  }

  private var introCard: some View {
    let core = store.core
    return VStack(alignment: .leading, spacing: 8) {
      Text("Find photos and moments in your videos by describing them: “dog on a beach”, “guy on a skateboard”.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      Text("Runs entirely on this Mac. Your photos, videos and searches never leave it.")
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      if let bytes = core.offeredBytes {
        Button {
          store.install("ai")
        } label: {
          Text("Install local search — downloads ~\(LocalSearchStore.sizeText(bytes.archive)), uses ~\(LocalSearchStore.sizeText(bytes.installed))")
        }
        .disabled(store.isPending("ai"))
        // Core, People and Sound in one click, one after another (owner
        // request, 2026-09-28). Offered while none is installed.
        if !store.anyPending, store.pieces.allSatisfy({ !$0.installed }), store.installAllIDs.count > 1,
           !store.pieces.contains(where: { store.removing.contains($0.id) }) {
          let all = store.pieces.filter { store.installAllIDs.contains($0.id) }
          let archive = all.compactMap { $0.offeredBytes?.archive }.reduce(0, +)
          let installed = all.compactMap { $0.offeredBytes?.installed }.reduce(0, +)
          Button {
            store.installAll()
          } label: {
            Text("Install all (\(all.map(\.title).joined(separator: ", "))) — downloads ~\(LocalSearchStore.sizeText(archive)), uses ~\(LocalSearchStore.sizeText(installed))")
          }
        }
        Text("Nothing is downloaded until you click Install. The download is a plain request that carries nothing about you or your files.")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        probeNote(core)
      }
    }
    .padding(14)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 12).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 12).stroke(MVTheme.hairline, lineWidth: 1))
  }

  @ViewBuilder
  private func probeNote(_ piece: LocalSearchStore.Piece) -> some View {
    switch piece.probe {
    case .notPublished?:
      note("Not published for download yet. It will be offered here once a release carries it.")
    case .needsNewerApp?:
      note("The published version needs a newer MediaViewer. Update MediaViewer first.")
    case .unreachable?:
      HStack {
        note("Could not reach the download server.")
        Button("Try again") { store.probe(force: true) }
      }
    default:
      note(piece.checking ? "Checking…" : "")
    }
  }

  private func pieceRow(_ piece: LocalSearchStore.Piece) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 16) {
      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 6) {
          Text(piece.title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
          if store.removing.contains(piece.id) {
            EmptyView()
          } else if piece.installed {
            Text(piece.state == "ok" ? "Installed · \(piece.version) · \(LocalSearchStore.sizeText(piece.size))"
                 : piece.state == "needs_update" ? "Needs an update" : "Did not verify")
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          } else if let bytes = piece.offeredBytes {
            Text("~\(LocalSearchStore.sizeText(bytes.installed))")
              .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          }
        }
        Text(piece.detail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      if store.busyPiece == piece.id {
        AddonProgressView(phase: store.phase ?? .downloading(done: 0, total: 0), width: 200)
      } else if store.queued.contains(piece.id) {
        HStack(spacing: 8) {
          Text(!piece.required && !store.coreInstalled ? "Queued, after Core" : "Queued")
            .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          Button("Cancel") { store.cancelQueued(piece.id) }
        }
        .transition(.opacity)
      } else if store.removing.contains(piece.id) {
        HStack(spacing: 6) {
          ProgressView().controlSize(.small)
          Text("Removing…").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        }
        .transition(.opacity)
      } else if piece.installed {
        // Clicks queue behind an install in progress (owner request,
        // 2026-09-28); only Remove waits for the queue to finish.
        if let v = piece.updateVersion {
          Button("Update to \(v)") { store.install(piece.id) }
            .disabled(store.refusal(for: piece) != nil)
            .help(store.refusal(for: piece) ?? (!piece.required && store.coreBehind ? "Core updates first." : ""))
        }
        if piece.state != "ok" {
          Button("Reinstall") { store.install(piece.id) }
        }
        Button("Remove…") { store.confirmingRemove = piece.id }
          .disabled(store.anyPending)
      } else if piece.offeredBytes != nil {
        Button("Install") { store.install(piece.id) }
          .disabled((!piece.required && !store.coreComing) || store.refusal(for: piece) != nil)
          .help(store.refusal(for: piece) ?? (piece.required || store.coreComing ? "" : "Install Core first."))
      }
    }
    .padding(12)
  }

  @ViewBuilder
  private func removeConfirm(_ id: String) -> some View {
    VStack(alignment: .leading, spacing: 8) {
      if id == "ai" {
        Text("Remove Local search? Also delete the search index? Keeping it means a reinstall can search right away.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
          .fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("Remove, keep the index") { store.remove("ai", keepData: true) }
            .keyboardShortcut(.defaultAction)
          Button("Remove and delete the index") { store.remove("ai", keepData: false) }
          Button("Cancel") { store.confirmingRemove = nil }.keyboardShortcut(.cancelAction)
        }
      } else {
        Text(id == "ai-audio"
             ? "Remove Sound? Searching videos by sound and speech stops until it is installed again."
             : "Remove People? Face search stops until it is installed again.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
          .fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("Remove") { store.remove(id, keepData: true) }.keyboardShortcut(.defaultAction)
          Button("Cancel") { store.confirmingRemove = nil }.keyboardShortcut(.cancelAction)
        }
      }
    }
    .padding(12)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 8).fill(MVTheme.surface))
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }
}

/// The add-on chrome's Settings view in the list, sized for the width it is
/// given. The chrome's view answers -fittingHeightForWidth: and posts
/// MVAddonSettingsViewHeightChanged when its content grows or shrinks (a
/// status line wraps, a confirmation opens); an intrinsic size alone is the
/// unwrapped one-line height, so wrapped text drew over the rows below it
/// (owner report, 2026-09-27).
struct AddonSettingsEmbed: View {
  let addonID: String
  @State private var heightGeneration = 0

  var body: some View {
    AddonSettingsHost(addonID: addonID, heightGeneration: heightGeneration)
      .onReceive(NotificationCenter.default.publisher(for: AddonSettingsHost.heightChanged)) { _ in
        heightGeneration &+= 1
      }
  }
}

/// The add-on chrome's own Settings view (an NSView from AI.bundle, kept alive
/// by the chrome), placed in the SwiftUI Settings list.
struct AddonSettingsHost: NSViewRepresentable {
  static let heightChanged = Notification.Name("MVAddonSettingsViewHeightChanged")
  private static let fittingHeight = NSSelectorFromString("fittingHeightForWidth:")

  let addonID: String
  /// Moves when the chrome's content changed height: SwiftUI asks sizeThatFits again.
  var heightGeneration = 0

  func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSView, context: Context) -> CGSize? {
    // A chrome from before the selector: its intrinsic size, as before.
    guard let width = proposal.width, width.isFinite, width > 0,
          nsView.responds(to: Self.fittingHeight) else { return nil }
    typealias Fn = @convention(c) (AnyObject, Selector, CGFloat) -> CGFloat
    let fn = unsafeBitCast(nsView.method(for: Self.fittingHeight), to: Fn.self)
    return CGSize(width: width, height: fn(nsView, Self.fittingHeight, width))
  }

  func makeNSView(context: Context) -> NSView {
    guard let raw = addonID.withCString({ mv_addon2_settings_view($0) }) else { return NSView() }
    let view = Unmanaged<NSView>.fromOpaque(raw).takeUnretainedValue()
    // Rebuilt after a reload or when Settings is rebuilt: move it here.
    view.removeFromSuperview()
    view.setContentHuggingPriority(.defaultLow, for: .horizontal)
    view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return view
  }

  func updateNSView(_ nsView: NSView, context: Context) {}
}

/// The command bar's pill while the pack is indexing: a progress ring, the
/// count, and a click opens the search panel. Absent when idle.
struct LocalSearchBarItem: View {
  @ObservedObject private var store = LocalSearchStore.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var hover = false

  var body: some View {
    if store.pillVisible {
      Button { store.openSearch() } label: {
        HStack(spacing: 7) {
          ProgressRing(progress: store.pillProgress, spinning: store.pillSpinning && !reduceMotion)
            .frame(width: 14, height: 14)
          Text(store.pillText)
            .font(MVTheme.font(13))
            .foregroundStyle(MVTheme.title)
            .lineLimit(1)
            .contentTransition(.numericText())
        }
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(Capsule().fill(Color.primary.opacity(hover ? 0.10 : 0.06)))
        .contentShape(Capsule())
      }
      .buttonStyle(.plain)
      .onHover { hover = $0 }
      .contextMenu {
        Button("Open search") { store.openSearch() }
        if store.pillOnBattery && !store.elsewhere {
          Button("Index anyway, on battery") { store.indexAnyway() }
        }
      }
      .help(store.pillOnBattery
            ? "Indexing waits on battery. Click to search; Control-click to index anyway until the Mac is next on power."
            : "Local search is indexing in the background. Click to search.")
      .accessibilityLabel(store.pillText)
      .accessibilityAction(named: "Index anyway") { if store.pillOnBattery { store.indexAnyway() } }
      .transition(.opacity.combined(with: .scale(scale: 0.96)))
      .padding(.leading, 6)
    }
  }
}

/// A small determinate ring; while `spinning`, an accent arc also turns so
/// work in progress reads at a glance. Still when paused.
struct ProgressRing: View {
  let progress: Double
  let spinning: Bool
  @State private var angle: Double = 0

  var body: some View {
    ZStack {
      Circle().stroke(Color.primary.opacity(0.15), lineWidth: 2)
      Circle()
        .trim(from: 0, to: max(0.04, progress))
        .stroke(MVTheme.accent, style: StrokeStyle(lineWidth: 2, lineCap: .round))
        .rotationEffect(.degrees(-90))
        .animation(.easeOut(duration: 0.3), value: progress)
      if spinning {
        Circle()
          .trim(from: 0, to: 0.18)
          .stroke(MVTheme.accent.opacity(0.55), style: StrokeStyle(lineWidth: 2, lineCap: .round))
          .rotationEffect(.degrees(angle))
          .onAppear {
            withAnimation(.linear(duration: 1.1).repeatForever(autoreverses: false)) { angle = 360 }
          }
          .onDisappear { angle = 0 }
      }
    }
  }
}
