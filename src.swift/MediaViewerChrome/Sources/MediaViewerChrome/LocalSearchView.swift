// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Local search and the command bar's indexing pill (plan/17 "The AI
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

  /// One installable piece of the AI family (plan/17 per-piece Install/Remove).
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
      if case .available(let a, let i, _) = probe { return (a, i) }
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
  @Published private(set) var used: UInt64 = 0
  @Published private(set) var ceiling: UInt64 = 3_000_000_000
  @Published private(set) var loaded = false
  @Published private(set) var loading = false
  @Published private(set) var loadError = ""
  @Published private(set) var busyPiece: String?
  @Published var message = ""
  @Published var confirmingRemove: String?
  /// Moves whenever the chrome is (re)attached, so the embedded view is rebuilt.
  @Published private(set) var chromeGeneration = 0

  // The command bar pill (mv.ai.1 status, no-block).
  @Published private(set) var pillVisible = false
  @Published private(set) var pillText = ""
  @Published private(set) var pillProgress: Double = 0
  @Published private(set) var pillSpinning = false

  private var timer: Timer?
  private var ticks = 0
  private var probed = false

  private init() {
    guard supported else { return }
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    refresh()
  }

  var core: Piece { pieces[0] }
  var coreInstalled: Bool { core.installed }

  private func poll() {
    ticks += 1
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
    let err = AddonStore.readString { mv_addon2_load_error("ai", $0, $1) }
    if err != loadError { loadError = err }
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

  func refresh() {
    guard supported else { return }
    Task.detached {
      var read: [String: [String: Any]] = [:]
      for id in ["ai", "ai-faces", "ai-audio"] {
        let json = AddonStore.readString { mv_addon2_state_json(id, $0, $1) }
        read[id] = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      }
      var u: UInt64 = 0, c: UInt64 = 0
      let room = mv_addon2_family_usage("ai", &u, &c)
      let states = read, used = u, ceiling = c
      await MainActor.run {
        for i in self.pieces.indices {
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
      }
    }
  }

  /// Settings opening asks the channel for each piece (two small GETs each).
  /// Nothing is downloaded until a button is clicked.
  func probe(force: Bool = false) {
    guard supported, force || !probed else { return }
    probed = true
    for i in pieces.indices where !pieces[i].checking {
      pieces[i].checking = true
      let channel = pieces[i].channel
      let id = pieces[i].id
      Task.detached {
        let result = await channel.probe()
        await MainActor.run {
          guard let j = self.pieces.firstIndex(where: { $0.id == id }) else { return }
          self.pieces[j].checking = false
          self.pieces[j].probe = result
        }
      }
    }
  }

  /// The 3 GB rule (plan/17), before anything downloads: the piece's installed
  /// size, plus what the family already uses minus the copy it replaces.
  func refusal(for piece: Piece) -> String? {
    guard let bytes = piece.offeredBytes?.installed, ceiling > 0 else { return nil }
    let after = Int64(used) - Int64(piece.installed ? piece.size : 0) + Int64(bytes)
    guard after > Int64(ceiling) else { return nil }
    return "\(piece.title) needs about \(Self.sizeText(bytes)), and Local search may use at most "
      + "\(Self.sizeText(Int(ceiling))) in all. Remove another piece first."
  }

  func install(_ id: String) {
    guard busyPiece == nil, let piece = pieces.first(where: { $0.id == id }) else { return }
    if !piece.required && !coreInstalled {
      message = "Install Core first."
      return
    }
    if let refusal = refusal(for: piece) {
      message = refusal
      return
    }
    busyPiece = id
    // An update installs beside the running copy (the store keeps it until the
    // next start). A new Core takes over then: its chrome cannot be replaced in
    // the running app. A new piece is picked up at once by "reload".
    let update = piece.updateVersion
    let coreRunning = loaded
    message = update.map { "Downloading \(piece.title) \($0)…" } ?? "Downloading \(piece.title)…"
    let channel = piece.channel
    let title = piece.title
    Task.detached {
      let result: String
      let ok: Bool
      do {
        try await channel.downloadAndInstall()
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
        if ok {
          // Core loads now (verified again, on a worker); a new piece is
          // picked up by the loaded pack at once ("reload").
          if id == "ai" && coreRunning, let v = update {
            text = "Local search \(v) is installed. It takes over the next time MediaViewer starts."
          } else if id == "ai" {
            if !mv_addon2_load("ai") { text = "Local search is installed but could not be started." }
          } else if self.loaded {
            _ = mv_addon2_reload("ai")
          }
          if let v = update, text == result { text = "\(title) updated to \(v)." }
        }
        self.message = text
        self.refresh()
      }
    }
  }

  func remove(_ id: String, keepData: Bool) {
    confirmingRemove = nil
    let title = pieces.first(where: { $0.id == id })?.title ?? id
    if id == "ai" && loaded {
      // The embedded management view stops using the table now, not at the
      // next poll: the pack is unloaded by the call below.
      loaded = false
      chromeGeneration += 1
      if pillVisible { pillVisible = false }
    }
    // On the main thread: unloading Core is [main-thread] in the host (the
    // chrome shuts down first); the store work itself is queued there.
    message = mv_addon2_remove(id, keepData)
      ? (id == "ai" ? "Local search removed." + (keepData ? " The search index was kept." : "")
                    : "\(title) removed.")
      : "\(title) will finish uninstalling the next time MediaViewer starts."
    refresh()
  }

  func openSearch() { _ = mv_addon2_run_command("search_open") }
}

/// Settings → Local search.
struct LocalSearchSection: View {
  @ObservedObject var store = LocalSearchStore.shared
  @ObservedObject var settings = SettingsStore.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    if store.supported {
      content
        .onAppear { if settings.visible { store.refresh(); store.probe() } }
        .onChange(of: settings.visible) { _, visible in
          if visible { store.refresh(); store.probe() }
        }
    }
  }

  private var content: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("Local search").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
      if !store.coreInstalled {
        introCard
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
      budgetBar
      if let id = store.confirmingRemove {
        removeConfirm(id)
          .transition(.opacity.combined(with: .move(edge: .top)))
      }
      if !store.message.isEmpty {
        Text(store.message).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
          .transition(.opacity)
      }
      if store.loading {
        HStack(spacing: 8) {
          ProgressView().controlSize(.small)
          Text("Starting Local search…").font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
        }
      } else if store.coreInstalled && !store.loaded && !store.loadError.isEmpty {
        Text(store.loadError == "UNSUPPORTED_FORMAT"
             ? "This Local search needs a newer MediaViewer. Update MediaViewer, then reinstall it."
             : "The installed Local search did not pass verification, so it was not started. Reinstall Core.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      // The pack's own management view: compute, quality, folders, index,
      // People. Owned by AI.bundle; rebuilt when the chrome is re-attached.
      if store.loaded {
        AddonSettingsHost(addonID: "ai")
          .id(store.chromeGeneration)
          .frame(maxWidth: .infinity, alignment: .leading)
          .transition(.opacity)
      }
    }
    .animation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.86), value: store.confirmingRemove)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: store.loaded)
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
        .disabled(store.busyPiece != nil)
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
          if piece.installed {
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
        ProgressView().controlSize(.small)
      } else if piece.installed {
        if let v = piece.updateVersion {
          Button("Update to \(v)") { store.install(piece.id) }
            .disabled(store.busyPiece != nil || store.refusal(for: piece) != nil)
            .help(store.refusal(for: piece) ?? "")
        }
        if piece.state != "ok" {
          Button("Reinstall") { store.install(piece.id) }.disabled(store.busyPiece != nil)
        }
        Button("Remove…") { store.confirmingRemove = piece.id }
          .disabled(store.busyPiece != nil)
      } else if piece.offeredBytes != nil {
        Button("Install") { store.install(piece.id) }
          .disabled(store.busyPiece != nil || (!piece.required && !store.coreInstalled)
                    || store.refusal(for: piece) != nil)
          .help(store.refusal(for: piece) ?? (piece.required || store.coreInstalled ? "" : "Install Core first."))
      }
    }
    .padding(12)
  }

  /// used / 3 GB, animated as pieces come and go.
  private var budgetBar: some View {
    let fraction = store.ceiling == 0 ? 0 : min(1, Double(store.used) / Double(store.ceiling))
    return VStack(alignment: .leading, spacing: 4) {
      GeometryReader { geo in
        ZStack(alignment: .leading) {
          Capsule().fill(Color.primary.opacity(0.08))
          Capsule().fill(Color.accentColor)
            .frame(width: max(fraction > 0 ? 6 : 0, geo.size.width * fraction))
        }
      }
      .frame(height: 6)
      .animation(reduceMotion ? nil : .spring(response: 0.45, dampingFraction: 0.85), value: store.used)
      Text("\(LocalSearchStore.sizeText(Int(store.used))) of \(LocalSearchStore.sizeText(Int(store.ceiling))) used by Local search. The search index is yours and does not count.")
        .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
    .accessibilityElement(children: .combine)
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

/// The add-on chrome's own Settings view (an NSView from AI.bundle, kept alive
/// by the chrome), placed in the SwiftUI Settings list. It sizes itself: the
/// chrome's NSHostingView publishes its intrinsic content size.
struct AddonSettingsHost: NSViewRepresentable {
  let addonID: String

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
      .help("Local search is indexing. Click to search.")
      .accessibilityLabel(store.pillText)
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
        .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, lineCap: .round))
        .rotationEffect(.degrees(-90))
        .animation(.easeOut(duration: 0.3), value: progress)
      if spinning {
        Circle()
          .trim(from: 0, to: 0.18)
          .stroke(Color.accentColor.opacity(0.55), style: StrokeStyle(lineWidth: 2, lineCap: .round))
          .rotationEffect(.degrees(angle))
          .onAppear {
            withAnimation(.linear(duration: 1.1).repeatForever(autoreverses: false)) { angle = 360 }
          }
          .onDisappear { angle = 0 }
      }
    }
  }
}
