// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #510 (docs/design/18 "Updates"): after an add-on update that takes over only
// at the next start (Import, Local search: a loaded bundle is not replaced
// under itself), a card in the bottom-right corner says "Import was updated to
// 1.3.0. Restart MediaViewer to use it." with Later and Restart now. Several
// updates make one card. Later hides it; Settings → Add-ons keeps "Restart to
// finish updating" until the next start (AddonsRestartMarker). Restart now
// never cuts work off: while an import, an add-on's transfer (busy_post), Local
// search indexing or an add-on download runs, the card says "Waiting for … to
// finish" and restarts once it has, or at Restart anyway.
//
// It shares the resume notice's hosting view (ResumeNoticeView.swift), above
// that card, so the corner stacks resume, restart, then the hint card
// (main_mac.mm -layoutHintNotice). A slot add-on (Places, Driftwire) needs no
// card: its update reloads it at once (reloadUpdatedSlotAddon), after a
// transfer it reports has ended.
//
// The twin of the Windows card (IslandHost.RestartNotice.cs); the words are
// RestartWords.cs's, and Chrome.Tests checks the two copies agree.
import AppKit
import MVChromeBridge
import SwiftUI

/// An add-on update that takes over at the next start.
struct AddonUpdated: Equatable {
  let name: String
  let version: String
}

/// The card's words and Settings' marker: src.managed/Shared/RestartWords.cs.
enum RestartWords {
  static let importing = "an import"
  static let indexing = "Local search indexing"
  static let download = "an add-on download"

  static let later = "Later"
  static let restartNow = "Restart now"
  static let restartAnyway = "Restart anyway"
  static let cancel = "Cancel"
  static let laterTip = "Keep using this version. Settings → Add-ons offers the restart until MediaViewer next starts."
  static let waitingBody = "MediaViewer restarts when it is done, on this folder and file."
  static let cancelTip = "Don't restart now. The update waits for the next start."

  /// "Import", "Import and Places", "Import, Places and Driftwire".
  static func join(_ names: [String]) -> String {
    switch names.count {
    case 0: return ""
    case 1: return names[0]
    default: return names.dropLast().joined(separator: ", ") + " and " + names[names.count - 1]
    }
  }

  static func title(_ u: [AddonUpdated]) -> String {
    switch u.count {
    case 0: return ""
    case 1: return "\(u[0].name) was updated to \(u[0].version)."
    default: return "\(join(u.map(\.name))) were updated."
    }
  }

  static func body(_ count: Int) -> String {
    "Restart MediaViewer to use \(count == 1 ? "it" : "them"). It opens again on this folder and file."
  }

  /// The command bar button's help.
  static func tooltip(_ u: [AddonUpdated]) -> String {
    u.isEmpty ? "" : "\(title(u)) Restart MediaViewer to use \(u.count == 1 ? "it" : "them")."
  }

  /// Settings → Add-ons until the next start: "Restart to finish updating Import 1.3.0."
  static func marker(_ u: [AddonUpdated]) -> String {
    u.isEmpty ? "" : "Restart to finish updating \(join(u.map { "\($0.name) \($0.version)" }))."
  }

  static func waitingTitle(_ what: [String]) -> String { "Waiting for \(join(what)) to finish" }

  static func anywayTip(_ what: [String]) -> String { "Restart now. This stops \(join(what))." }
}

@MainActor
final class RestartNoticeStore: ObservableObject {
  static let shared = RestartNoticeStore()

  /// The card is up.
  @Published private(set) var shown = false
  /// Restart now is waiting for these to finish; empty when it is not.
  @Published private(set) var waitingFor: [String] = []
  /// busy_post: add-on id -> what it is doing (AddonNotices.read).
  private(set) var slotBusy: [String: String] = [:]
  /// #532: "Update ready — restart" was clicked while an add-on had work under
  /// way (a Driftwire transfer); the restart goes once it has ended.
  @Published private(set) var updateWaiting = false
  /// A slot add-on updated while it was busy: reloaded once it is not.
  private var pendingReloads: [String: AddonUpdated] = [:]
  private var timer: Timer?
  /// MV_ADDON_RESTART_SELFTEST's made-up work ("" none).
  private var selfTestBusy = ""

  private init() {}

  /// AddonRestart.needed: the card, with every update waiting on a restart.
  func show() {
    let pending = AddonRestart.shared.pending
    guard !pending.isEmpty else { return hide() }
    shown = true
    ResumeNoticeStore.shared.syncHost()
    // Said once as it appears, not only when focused.
    NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested,
                         userInfo: [.announcement: RestartWords.title(pending),
                                    .priority: NSAccessibilityPriorityLevel.medium.rawValue])
  }

  func later() { hide() }

  private func hide() {
    stopWaiting()
    guard shown else { return }
    shown = false
    ResumeNoticeStore.shared.syncHost()
  }

  /// What Restart now would cut off. An F7 / F8 copy needs no wait: the file
  /// worker finishes it before the app exits, and the waiter opens the app
  /// again only once this process is gone.
  func blockers() -> [String] {
    var what: [String] = []
    if !AddonStore.shared.status.isEmpty { what.append(RestartWords.importing) }
    for w in slotBusy.keys.sorted().compactMap({ slotBusy[$0] }) where !what.contains(w) { what.append(w) }
    let search = LocalSearchStore.shared
    if search.pillVisible && search.pillSpinning { what.append(RestartWords.indexing) }
    if !selfTestBusy.isEmpty && !what.contains(selfTestBusy) { what.append(selfTestBusy) }
    if AddonStore.shared.phase != nil || search.busyPiece != nil || SlotAddonOffer.anyBusy || DriftwireStore.shared.busy {
      what.append(RestartWords.download)
    }
    return what
  }

  /// Restart now (the card, the bar, Settings): at once when nothing runs;
  /// otherwise the card says what it waits for and restarts when that is done.
  /// The host still refuses under a playing clip.
  func restartWhenIdle() {
    let b = blockers()
    guard !b.isEmpty else {
      stopWaiting()
      mv_chrome_restart_for_addons()
      return
    }
    waitingFor = b
    show()
    timer?.invalidate()
    timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.tick() }
    }
  }

  private func tick() {
    guard !waitingFor.isEmpty else { return stopWaiting() }
    let b = blockers()
    if b.isEmpty {
      stopWaiting()
      mv_chrome_restart_for_addons()
    } else if b != waitingFor {
      waitingFor = b
    }
  }

  private func stopWaiting() {
    timer?.invalidate()
    timer = nil
    if !waitingFor.isEmpty { waitingFor = [] }
  }

  func cancelWait() { stopWaiting() }

  /// The card's verify without publishing an update, as on Windows
  /// (IslandHost.RestartNotice.cs RestartNoticeSelfTest; DEVELOPMENT "Add-on
  /// restart card"). MV_ADDON_RESTART_SELFTEST="Import=1.3.0,Places=1.1.0"
  /// shows the card for those; ";busy=a Driftwire transfer" makes something
  /// run, and ";wait" presses Restart now, which then waits. Read once: the
  /// MediaViewer a Restart now opens again does not inherit it.
  func selfTest() {
    guard let spec = ProcessInfo.processInfo.environment["MV_ADDON_RESTART_SELFTEST"], !spec.isEmpty,
          AddonRestart.shared.pending.isEmpty else { return }
    unsetenv("MV_ADDON_RESTART_SELFTEST")
    let parts = spec.split(separator: ";").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    guard let first = parts.first else { return }
    for pair in first.split(separator: ",") {
      let kv = pair.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
      if kv.count == 2, !kv[0].isEmpty, !kv[1].isEmpty { AddonRestart.shared.needed(kv[0], kv[1]) }
    }
    for p in parts.dropFirst() where p.hasPrefix("busy=") { selfTestBusy = String(p.dropFirst(5)) }
    if parts.contains("wait") { restartWhenIdle() }
  }

  func restartAnyway() {
    stopWaiting()
    mv_chrome_restart_for_addons()
  }

  // ---- the app update (#532) ---------------------------------------------------

  /// "Update ready — restart": at once, or once no add-on has work under way.
  /// A transfer is never cut by an update.
  func restartToUpdateWhenIdle() {
    if slotBusy.isEmpty {
      updateWaiting = false
      mv_chrome_restart_to_update()
    } else {
      updateWaiting = true
    }
  }

  // ---- slot add-ons ------------------------------------------------------------

  /// AddonNotices.read: what each add-on said it is doing (busy_post). An
  /// update that waited on one is reloaded once it is done.
  func noteSlotBusy(_ now: [String: String]) {
    guard now != slotBusy else { return }
    slotBusy = now
    if updateWaiting && now.isEmpty {
      updateWaiting = false
      mv_chrome_restart_to_update()
    }
    for (id, u) in pendingReloads where now[id] == nil {
      pendingReloads[id] = nil
      reloadNow(id)
      SlotAddonOffer.channel(id: id, name: u.name).reloaded("\(u.name) updated to \(u.version).")
    }
  }

  /// A loaded slot add-on's update has installed: it reloads now (unload, then
  /// the newest verified version loads), with no restart, or once the work it
  /// reports has ended. Returns Settings' line for its card.
  func reloadUpdatedSlotAddon(id: String, name: String, version: String) -> String {
    if let what = slotBusy[id] {
      pendingReloads[id] = AddonUpdated(name: name, version: version)
      return "\(name) \(version) is installed. It takes over when \(what) has finished."
    }
    reloadNow(id)
    return "\(name) updated to \(version)."
  }

  private func reloadNow(_ id: String) {
    _ = id.withCString { mv_chrome_slot_addon_reload($0) }
  }
}

/// The card itself, drawn above the resume notice in its hosting view.
struct RestartNoticeCard: View {
  @ObservedObject private var store = RestartNoticeStore.shared
  @ObservedObject private var restart = AddonRestart.shared

  var body: some View {
    let waiting = !store.waitingFor.isEmpty
    let title = waiting ? RestartWords.waitingTitle(store.waitingFor) : RestartWords.title(restart.pending)
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      Text(title)
        .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      Text(waiting ? RestartWords.waitingBody : RestartWords.body(restart.pending.count))
        .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: MVTheme.space(8)) {
        Spacer(minLength: 0)
        if waiting {
          Button(RestartWords.cancel) { store.cancelWait() }
            .help(RestartWords.cancelTip)
          Button(RestartWords.restartAnyway) { store.restartAnyway() }
            .help(RestartWords.anywayTip(store.waitingFor))
        } else {
          Button(RestartWords.later) { store.later() }
            .help(RestartWords.laterTip)
          Button(RestartWords.restartNow) { store.restartWhenIdle() }
        }
      }
    }
    .padding(MVTheme.space(14))
    .frame(width: ResumeNoticeView.width, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(10)).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(10)).stroke(MVTheme.hairline, lineWidth: 1))
    .shadow(color: .black.opacity(0.18), radius: 8, y: 2)
    .padding(MVTheme.space(8))  // room for the shadow inside the hosting view
    .fixedSize()
    .accessibilityElement(children: .contain)
    .accessibilityLabel(title)
  }
}

/// Settings → Add-ons: "Restart to finish updating …" and Restart now, while an
/// update waits on a restart (the card's Later leaves it). The Windows row is
/// IslandHost.Update.cs BuildAddonsRestartRow.
struct AddonsRestartMarker: View {
  @ObservedObject private var restart = AddonRestart.shared

  var body: some View {
    if !restart.pending.isEmpty {
      HStack(spacing: MVTheme.space(12)) {
        Text(RestartWords.marker(restart.pending))
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
          .fixedSize(horizontal: false, vertical: true)
        Button(RestartWords.restartNow) { restart.restart() }
          .help("MediaViewer opens again on this folder and file. Anything still running finishes first.")
      }
    }
  }
}
