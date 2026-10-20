// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #510 (docs/design/18 "Settings → Add-ons"): Settings → Add-ons is a short
// list, one row per add-on with its state in a few words (Installed · 1.2.0,
// Update available · 1.3.0, Restart to finish updating, Not installed), and
// each row opens that add-on's own page in Settings: its line, Install /
// Update / Open / Remove, its progress and messages, and its settings. The
// order is Import, Local search, Places, Driftwire, any other installed add-on
// of MediaViewer's, then From other makers.
//
// The pages are the views the add-ons always had (ImportAddonPage,
// LocalSearchSection, SlotAddonsSection showing one add-on, OpenAddonsSection).
// Opening Settings shows the list unless something asked for a page (a
// package to install, an add-on's alert).
//
// The twin of IslandHost.AddonList.cs; the words are
// src.managed/Shared/AddonListWords.cs, and Chrome.Tests checks the copies agree.
import MVChromeBridge
import SwiftUI

/// The list's and pages' words: src.managed/Shared/AddonListWords.cs.
enum AddonListWords {
  static let heading = "Add-ons"
  static let back = "All add-ons"
  static let fromOthers = "From other makers"
  static let checking = "Checking…"
  static let notInstalled = "Not installed"
  static let installing = "Installing…"
  static let needsUpdate = "Needs an update"
  static let couldNotStart = "Couldn't start"
  static let restart = "Restart to finish updating"

  static func installed(_ version: String) -> String { version.isEmpty ? "Installed" : "Installed · \(version)" }

  static func update(_ version: String) -> String { "Update available · \(version)" }

  static func others(_ count: Int) -> String {
    switch count {
    case 0: return "None installed"
    case 1: return "1 installed"
    default: return "\(count) installed"
    }
  }

  /// A row's state (AddonListWords.Line).
  static func line(known: Bool, installed: Bool, version: String, state: String, update: String?,
                   restart: Bool, busy: Bool) -> String {
    if !known { return checking }
    if busy { return installing }
    if restart { return self.restart }
    if !installed { return notInstalled }
    if state == "needs_update" { return needsUpdate }
    if state != "ok" { return couldNotStart }
    if let update { return self.update(update) }
    return self.installed(version)
  }
}

/// Which add-on's page Settings shows (nil: the General page itself).
@MainActor
final class AddonNav: ObservableObject {
  static let shared = AddonNav()
  static let importPage = "import"
  static let localSearchPage = "ai"
  static let othersPage = "others"

  @Published private(set) var page: String?
  /// Asked for while Settings was closed: shown when it opens.
  private var wanted: String?

  func open(_ id: String) {
    if SettingsStore.shared.visible { page = id } else { wanted = id }
  }

  /// Opens Settings on an add-on's page.
  func show(_ id: String) {
    open(id)
    if !SettingsStore.shared.visible { mv_chrome_menu(kMenuSettings) }
  }

  func back() { page = nil }

  /// A row's scroll id: back from a page, the list comes back at that row,
  /// as on Windows.
  static func rowAnchor(_ id: String) -> String { "addon-row-" + id }

  /// Settings opened: the page something asked for, else the list.
  func settingsShown() {
    page = wanted
    wanted = nil
  }
}

/// Settings → Add-ons: the heading, the restart marker and the rows.
struct AddonsSection: View {
  @ObservedObject private var settings = SettingsStore.shared
  @ObservedObject private var importer = AddonStore.shared
  @ObservedObject private var search = LocalSearchStore.shared
  @ObservedObject private var slots = SlotAddonsSection.Installed.shared
  @ObservedObject private var places = SlotAddonOffer.places
  @ObservedObject private var driftwire = DriftwireStore.shared
  @ObservedObject private var others = OpenAddonStore.shared
  @ObservedObject private var restart = AddonRestart.shared
  @ObservedObject private var nav = AddonNav.shared

  private struct Entry: Identifiable {
    let id: String
    let name: String
    let line: String
  }

  private func restarting(_ name: String) -> Bool { restart.pending.contains { $0.name == name } }

  private func slotLine(_ row: SlotAddonsSection.Row?, _ offer: SlotAddonOffer, _ name: String) -> String {
    let update = row.flatMap { r in
      offer.offer?.version.flatMap { v in AddonChannel.isNewer(v, than: r.version) ? v : nil }
    }
    return AddonListWords.line(known: slots.known, installed: row != nil, version: row?.version ?? "",
                               state: row?.state ?? "", update: update, restart: restarting(name),
                               busy: offer.phase != nil)
  }

  private var entries: [Entry] {
    var list: [Entry] = [
      Entry(id: AddonNav.importPage, name: "Import",
            line: AddonListWords.line(known: importer.stateKnown, installed: importer.installed,
                                      version: importer.version, state: importer.state,
                                      update: importer.updateVersion, restart: restarting("Import"),
                                      busy: importer.phase != nil)),
    ]
    if search.supported {
      let core = search.pieces.first { $0.id == "ai" }
      list.append(Entry(id: AddonNav.localSearchPage, name: "Local search",
                        line: AddonListWords.line(known: search.stateKnown, installed: core?.installed ?? false,
                                                  version: core?.version ?? "", state: core?.state ?? "",
                                                  update: core?.updateVersion, restart: restarting("Local search"),
                                                  busy: search.busyPiece != nil)))
    }
    list.append(Entry(id: "places", name: "Places",
                      line: slotLine(slots.rows.first { $0.id == "places" }, places, "Places")))
    let dw = slots.rows.first { $0.id == DriftwireStore.addonID }
    list.append(Entry(id: DriftwireStore.addonID, name: "Driftwire",
                      line: slotLine(dw, SlotAddonOffer.channel(id: DriftwireStore.addonID, name: "Driftwire"),
                                     "Driftwire")))
    let rest = slots.rows.filter { $0.id != "places" && $0.id != DriftwireStore.addonID }
      .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    for row in rest {
      list.append(Entry(id: row.id, name: row.name,
                        line: slotLine(row, SlotAddonOffer.channel(id: row.id, name: row.name), row.name)))
    }
    list.append(Entry(id: AddonNav.othersPage, name: AddonListWords.fromOthers,
                      line: AddonListWords.others(others.installed.count)))
    return list
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      Text(AddonListWords.heading).font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
      AddonsRestartMarker()  // #510: "Restart to finish updating …"
      VStack(spacing: MVTheme.space(6)) {
        ForEach(entries) { e in
          Button { nav.open(e.id) } label: {
            HStack(spacing: MVTheme.space(12)) {
              Text(e.name).font(MVTheme.font()).foregroundStyle(MVTheme.title)
              Spacer(minLength: MVTheme.space(12))
              Text(e.line).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
              Image(systemName: "chevron.right").font(.system(size: 11, weight: .semibold))
                .foregroundStyle(MVTheme.body)
            }
            .padding(.horizontal, MVTheme.space(14)).padding(.vertical, MVTheme.space(12))
            .frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
            .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8)).stroke(MVTheme.hairline, lineWidth: 1))
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .accessibilityLabel("\(e.name), \(e.line)")
          .id(AddonNav.rowAnchor(e.id))
        }
      }
    }
    // Opening Settings asks every channel, whatever the automatic-check
    // switch says, so the rows can say "Update available": the person is
    // looking at what can be installed or updated (docs/design/18).
    .onAppear { if settings.visible { probeAll() } }
    .onChange(of: settings.visible) { _, visible in if visible { probeAll() } }
  }

  private func probeAll() {
    importer.probe()
    search.settingsShown()
    slots.reload()
    places.probe()
    guard !mv_addon2_elsewhere() else { return }
    for row in slots.rows where row.id != "places" {
      SlotAddonOffer.channel(id: row.id, name: row.name).probe()
    }
  }
}

/// An add-on's page: back, its name, its view.
struct AddonPageView: View {
  let id: String
  @ObservedObject private var nav = AddonNav.shared

  init(id: String) { self.id = id }
  @ObservedObject private var slots = SlotAddonsSection.Installed.shared

  private var title: String {
    switch id {
    case AddonNav.importPage: return "Import"
    case AddonNav.localSearchPage: return "Local search"
    case AddonNav.othersPage: return AddonListWords.fromOthers
    case "places": return "Places"
    case DriftwireStore.addonID: return "Driftwire"
    default: return slots.rows.first { $0.id == id }?.name ?? id
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(12)) {
      Button { nav.back() } label: {
        HStack(spacing: MVTheme.space(6)) {
          Image(systemName: "chevron.left").font(.system(size: 11, weight: .semibold))
          Text(AddonListWords.back).font(MVTheme.font())
        }
        .foregroundStyle(MVTheme.title)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .help("Back to the list of add-ons (Esc)")
      Text(title).font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
        .accessibilityAddTraits(.isHeader)
      Group {
        switch id {
        case AddonNav.importPage: ImportAddonPage()
        case AddonNav.localSearchPage: LocalSearchSection()
        case AddonNav.othersPage: OpenAddonsSection().id(OpenAddonStore.sectionAnchor)
        default: SlotAddonsSection(only: id)
        }
      }
      .padding(MVTheme.space(14))
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8)).stroke(MVTheme.hairline, lineWidth: 1))
    }
    .padding(.top, MVTheme.space(12))
    .onExitCommand { nav.back() }
  }
}
