// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "Choose your Local search models" (#482, docs/plans/model-tiers.md T4): the
// model tier picker. Opened by the chrome as a sheet on the window it is asked
// for (the viewer once per start while no tier is chosen; Settings' Change…),
// never by the pack. Download sets the pack's "tier" and asks the host for the
// pieces still missing (-installLocalSearchPieces:, LocalSearchStore's queue);
// Not now chooses nothing, and the picker comes back next start.
import AppKit
import CAiApi
import SwiftUI

extension AITable {
  /// The picker's input: the tiers, the check and the choice from
  /// settings_json, and which pieces the pack has loaded now.
  func tierCatalogue() -> (catalogue: TierCatalogue, installed: Set<TierPiece>, documentFiles: Bool)? {
    guard let raw = json({ a.settings_json?(ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG }),
          let obj = parseJSON(raw) as? [String: Any] else { return nil }
    let catalogue = TierCatalogue(settings: obj, raw: raw)
    let flags = status()?.flags ?? 0
    var installed = Set<TierPiece>()
    if flags & MV_AI_STATUS_FACES_READY != 0 { installed.insert(.people) }
    if flags & MV_AI_STATUS_AUDIO_READY != 0 || obj["audio_ready"] as? Bool == true { installed.insert(.sounds) }
    if flags & MV_AI_STATUS_DOCS_READY != 0 || obj["docs_ready"] as? Bool == true { installed.insert(.documents) }
    return (catalogue, installed, obj["document_files"] as? Bool ?? false)
  }

  /// settings "tier": a JSON string.
  func setTier(_ id: String) {
    guard let data = try? JSONSerialization.data(withJSONObject: id, options: .fragmentsAllowed),
          let value = String(data: data, encoding: .utf8) else { return }
    setSetting("tier", value)
  }
}

struct TierPickerView: View {
  let catalogue: TierCatalogue
  /// Download: the tier and the pieces to install.
  let choose: (String, [String]) -> Void
  let notNow: () -> Void
  @State private var choice: TierChoice

  static let footnote = "Models download from Hugging Face. It sees your IP address and which public model "
    + "files are fetched — nothing about your photos or videos."

  init(catalogue: TierCatalogue, installed: Set<TierPiece>, documentFiles: Bool,
       choose: @escaping (String, [String]) -> Void, notNow: @escaping () -> Void) {
    self.catalogue = catalogue
    self.choose = choose
    self.notNow = notNow
    _choice = State(initialValue: TierChoice(catalogue: catalogue, installed: installed,
                                             documentFiles: documentFiles))
  }

  private var accelerated: Bool { catalogue.machine.accelerated }
  private var selected: TierOption? { catalogue.tier(choice.selected) }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      VStack(alignment: .leading, spacing: 4) {
        Text("Choose your Local search models").font(AITheme.font(18)).foregroundStyle(AITheme.title)
        Text(catalogue.machine.line).font(AITheme.font(12)).foregroundStyle(AITheme.body)
      }
      ScrollView {
        VStack(alignment: .leading, spacing: 8) {
          if let rec = catalogue.recommended {
            Text("★ RECOMMENDED").font(AITheme.font(11)).fontWeight(.semibold).foregroundStyle(AITheme.accent)
            card(rec)
          }
          if !catalogue.others.isEmpty {
            Text("Other options").font(AITheme.font(11)).fontWeight(.semibold).foregroundStyle(AITheme.body)
              .padding(.top, 6)
            ForEach(catalogue.others) { card($0) }
          }
        }
      }
      .frame(minHeight: 220)
      if let tier = selected { pieces(tier) }
      if choice.switching {
        Text(TierChoice.switchNote).font(AITheme.font(12)).foregroundStyle(AITheme.title)
          .fixedSize(horizontal: false, vertical: true)
      }
      Text(Self.footnote).font(AITheme.font(11)).foregroundStyle(AITheme.body)
        .fixedSize(horizontal: false, vertical: true)
      HStack {
        Spacer()
        Button("Not now", action: notNow).keyboardShortcut(.cancelAction)
        Button(choice.buttonTitle) {
          guard let tier = selected else { return }
          choose(tier.id, choice.installIDs(tier))
        }
        .keyboardShortcut(.defaultAction)
        .disabled(!choice.canConfirm(selected))
      }
    }
    .padding(20)
    .frame(width: 560)
    .frame(minHeight: 520, idealHeight: 640)
    .onExitCommand(perform: notNow)
  }

  /// One tier: a click selects it.
  private func card(_ tier: TierOption) -> some View {
    let on = tier.id == choice.selected
    let time = tier.timeText(accelerated: accelerated)
    var sizes = "Downloads \(TierText.size(tier.downloadTotal(choice.checked)))"
      + " · \(TierText.size(tier.diskTotal(choice.checked))) on disk"
    if !time.isEmpty { sizes += " · \(time)" }
    return Button {
      choice.selected = tier.id
    } label: {
      HStack(alignment: .top, spacing: 10) {
        Image(systemName: on ? "largecircle.fill.circle" : "circle")
          .foregroundStyle(on ? AITheme.accent : AITheme.body)
        VStack(alignment: .leading, spacing: 3) {
          HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(tier.title).font(AITheme.font(14)).foregroundStyle(AITheme.title)
            if tier.id == catalogue.chosen {
              Text("in use").font(AITheme.font(11)).foregroundStyle(AITheme.body)
            }
          }
          if !tier.forLine.isEmpty {
            Text(tier.forLine).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          }
          if !tier.modelsLine.isEmpty {
            Text(tier.modelsLine).font(AITheme.font(12)).foregroundStyle(AITheme.title)
              .fixedSize(horizontal: false, vertical: true)
          }
          Text(sizes).font(AITheme.font(12)).foregroundStyle(AITheme.body)
          if !tier.licences.isEmpty {
            Text("Licences: " + tier.licences.joined(separator: ", "))
              .font(AITheme.font(10)).foregroundStyle(AITheme.body)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      .padding(12)
      .contentShape(Rectangle())
      .background(RoundedRectangle(cornerRadius: AITheme.radius(8))
                    .fill(on ? AITheme.accent.opacity(0.10) : AITheme.surface))
      .overlay(RoundedRectangle(cornerRadius: AITheme.radius(8))
                 .stroke(on ? AITheme.accent : AITheme.hairline, lineWidth: on ? 2 : 1))
    }
    .buttonStyle(.plain)
    .accessibilityAddTraits(on ? .isSelected : [])
  }

  /// People, Sounds, Documents: which pieces come with the tier, sized for it.
  private func pieces(_ tier: TierOption) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("Also install").font(AITheme.font(12)).fontWeight(.semibold).foregroundStyle(AITheme.title)
      ForEach(TierPiece.allCases) { piece in
        Toggle(isOn: Binding(get: { choice.isChecked(piece) && tier.offers(piece) },
                             set: { _ in choice.toggle(piece) })) {
          HStack(spacing: 6) {
            Text(label(piece, tier)).font(AITheme.font(13)).foregroundStyle(AITheme.title)
            Text(pieceDetail(piece, tier)).font(AITheme.font(11)).foregroundStyle(AITheme.body)
          }
        }
        .toggleStyle(.checkbox)
        .disabled(choice.installed.contains(piece) || !tier.offers(piece))
      }
    }
  }

  private func label(_ piece: TierPiece, _ tier: TierOption) -> String {
    switch piece {
    case .people: return "People"
    case .sounds: return tier.soundsLabel
    case .documents: return "Documents"
    }
  }

  private func pieceDetail(_ piece: TierPiece, _ tier: TierOption) -> String {
    if choice.installed.contains(piece) { return "installed" }
    if !tier.offers(piece) { return "not in this tier" }
    let bytes = tier.downloadBytes[piece.rawValue] ?? 0
    return bytes > 0 ? TierText.size(bytes) : ""
  }
}

/// The picker as a sheet on a window (or a window of its own with none).
@MainActor
final class TierPickerController {
  private var window: NSWindow?

  var isOpen: Bool { window != nil }

  func show(over parent: NSWindow?, catalogue: TierCatalogue, installed: Set<TierPiece>, documentFiles: Bool,
            choose: @escaping (String, [String]) -> Void) {
    guard window == nil, !catalogue.isEmpty else { return }
    let sheet = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 640),
                         styleMask: [.titled], backing: .buffered, defer: true)
    sheet.isReleasedWhenClosed = false
    let view = TierPickerView(catalogue: catalogue, installed: installed, documentFiles: documentFiles, choose: { [weak self] tier, ids in
      self?.close()
      choose(tier, ids)
    }, notNow: { [weak self] in self?.close() })
    sheet.contentViewController = NSHostingController(rootView: AIThemeRoot { view })
    window = sheet
    if let parent, parent.isVisible {
      parent.beginSheet(sheet)
    } else {
      sheet.center()
      sheet.makeKeyAndOrderFront(nil)
    }
  }

  func close() {
    guard let sheet = window else { return }
    window = nil
    if let parent = sheet.sheetParent {
      parent.endSheet(sheet)
    } else {
      sheet.close()
    }
  }
}
