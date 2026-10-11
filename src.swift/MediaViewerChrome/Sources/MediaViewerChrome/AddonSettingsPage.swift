// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An add-on's settings page, drawn from the schema its manifest wrote
// (docs/design/25 "Settings page", contribution API 2), and the key layouts add-ons
// offer (docs/design/16 "Keymap packs"). The Windows twin is the page in
// IslandHost.OpenAddons.cs: the same controls in the same order.
//
//   toggle -> a switch         choice -> a menu of the option labels
//   number -> a field, a stepper and the unit    text -> a field
//
// The add-on writes no UI code and this file writes no add-on's values
// itself: the core reads the page and the values (mv_addons_settings_page)
// and checks and stores each change (mv_addons_setting_set), both on a
// worker; this file shows and asks.
import Foundation
import SwiftUI
import MVChromeBridge

/// One item of a page, as the core hands it over (settings_values::page_json).
struct AddonSettingItem: Identifiable {
  enum Kind: String { case toggle, choice, number, text }
  struct Option: Hashable {
    let value: String
    let label: String
  }
  let kind: Kind
  let key: String
  let label: String
  let help: String
  let enabledBy: String
  var flag = false
  var number = 0.0
  var text = ""
  var min = 0.0, max = 0.0, step = 1.0
  var unit = ""
  var options: [Option] = []
  var maxLength = 256
  var placeholder = ""

  var id: String { key }

  init?(_ obj: [String: Any]) {
    guard let kind = Kind(rawValue: obj["type"] as? String ?? ""),
          let key = obj["key"] as? String else { return nil }
    self.kind = kind
    self.key = key
    label = obj["label"] as? String ?? key
    help = obj["help"] as? String ?? ""
    enabledBy = obj["enabled_by"] as? String ?? ""
    let value = obj["value"]
    switch kind {
    case .toggle:
      flag = value as? Bool ?? false
    case .number:
      number = (value as? NSNumber)?.doubleValue ?? 0
      min = (obj["min"] as? NSNumber)?.doubleValue ?? 0
      max = (obj["max"] as? NSNumber)?.doubleValue ?? 0
      step = (obj["step"] as? NSNumber)?.doubleValue ?? 1
      unit = obj["unit"] as? String ?? ""
    case .choice:
      text = value as? String ?? ""
      options = (obj["options"] as? [[String: Any]] ?? []).compactMap {
        guard let v = $0["value"] as? String else { return nil }
        return Option(value: v, label: $0["label"] as? String ?? v)
      }
    case .text:
      text = value as? String ?? ""
      maxLength = obj["max_length"] as? Int ?? 256
      placeholder = obj["placeholder"] as? String ?? ""
    }
  }

  /// The value as JSON text, the form the core stores.
  var json: String {
    switch kind {
    case .toggle: return flag ? "true" : "false"
    case .number:
      return number.rounded() == number && abs(number) < 1e15
        ? String(Int64(number)) : String(format: "%.3f", number)
    case .choice, .text:
      let data = (try? JSONSerialization.data(withJSONObject: [text], options: [])) ?? Data("[\"\"]".utf8)
      let wrapped = String(decoding: data, as: UTF8.self)
      return String(wrapped.dropFirst().dropLast())
    }
  }

  /// "15 km", "2.5 km": what a number shows.
  var numberText: String {
    number.rounded() == number ? String(Int64(number)) : String(format: "%g", number)
  }
}

struct AddonSettingSection: Identifiable {
  let id: Int
  let title: String
  let help: String
  var items: [AddonSettingItem]
}

/// One add-on's page: read when Settings shows it, each change stored at once.
@MainActor
final class AddonSettingsModel: ObservableObject {
  let addonID: String
  @Published private(set) var title = ""
  @Published private(set) var sections: [AddonSettingSection] = []
  @Published private(set) var loaded = false
  /// Why the last change to an item was refused, by key.
  @Published private(set) var refusals: [String: String] = [:]

  init(addonID: String) { self.addonID = addonID }

  func load() {
    let id = addonID
    Task.detached(priority: .utility) {
      let json = OpenAddonBridge.read { mv_addons_settings_page(id, $0, $1) }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      let title = obj["title"] as? String ?? ""
      let raw = obj["sections"] as? [[String: Any]] ?? []
      let sections = raw.enumerated().map { index, s in
        AddonSettingSection(
          id: index, title: s["title"] as? String ?? "", help: s["help"] as? String ?? "",
          items: (s["items"] as? [[String: Any]] ?? []).compactMap(AddonSettingItem.init))
      }
      await MainActor.run {
        self.title = title
        self.sections = sections
        self.loaded = true
      }
    }
  }

  /// Whether an item's `enabled_by` toggle is on (or it has none).
  func enabled(_ item: AddonSettingItem) -> Bool {
    guard !item.enabledBy.isEmpty else { return true }
    for s in sections {
      if let t = s.items.first(where: { $0.key == item.enabledBy }) { return t.flag }
    }
    return true
  }

  /// Shows the change at once and stores it on a worker; a refusal puts the
  /// last good value back and says why under the item.
  func change(_ item: AddonSettingItem) {
    guard let (si, ii) = index(of: item.key) else { return }
    let before = sections[si].items[ii]
    sections[si].items[ii] = item
    refusals[item.key] = nil
    let id = addonID
    let key = item.key
    let value = item.json
    Task.detached(priority: .userInitiated) {
      let json = OpenAddonBridge.read(capacity: 4096) { mv_addons_setting_set(id, key, value, $0, $1) }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      let ok = obj["ok"] as? Bool ?? false
      let message = obj["message"] as? String ?? "The setting could not be saved."
      await MainActor.run {
        guard !ok, let (si, ii) = self.index(of: key) else { return }
        self.sections[si].items[ii] = before
        self.refusals[key] = message.isEmpty ? "The setting could not be saved." : message
      }
    }
  }

  private func index(of key: String) -> (Int, Int)? {
    for (si, s) in sections.enumerated() {
      if let ii = s.items.firstIndex(where: { $0.key == key }) { return (si, ii) }
    }
    return nil
  }
}

/// The page, under its add-on's row in Settings → Add-ons.
struct AddonSettingsPage: View {
  @StateObject private var model: AddonSettingsModel
  /// Shown when the page has no title of its own.
  let fallbackTitle: String

  init(addonID: String, fallbackTitle: String) {
    _model = StateObject(wrappedValue: AddonSettingsModel(addonID: addonID))
    self.fallbackTitle = fallbackTitle
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
      if model.loaded && !model.sections.isEmpty {
        Text("\(model.title.isEmpty ? fallbackTitle : model.title) settings")
          .font(MVTheme.font(14)).fontWeight(.semibold).foregroundStyle(MVTheme.title)
        ForEach(model.sections) { section in
          VStack(alignment: .leading, spacing: MVTheme.space(8)) {
            Text(section.title).font(MVTheme.font(13)).fontWeight(.semibold)
              .foregroundStyle(MVTheme.title)
            if !section.help.isEmpty { note(section.help) }
            ForEach(section.items) { item in
              itemRow(item)
            }
          }
        }
      }
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8)).stroke(MVTheme.hairline, lineWidth: 1))
    .opacity(model.loaded && !model.sections.isEmpty ? 1 : 0)
    .frame(height: model.loaded && !model.sections.isEmpty ? nil : 0)
    .onAppear { model.load() }
  }

  @ViewBuilder private func itemRow(_ item: AddonSettingItem) -> some View {
    let enabled = model.enabled(item)
    VStack(alignment: .leading, spacing: MVTheme.space(4)) {
      HStack(spacing: MVTheme.space(16)) {
        Text(item.label).font(MVTheme.font(13)).foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
          .frame(maxWidth: .infinity, alignment: .leading)
        control(item).disabled(!enabled).accessibilityLabel(item.label)
      }
      if !item.help.isEmpty { note(item.help) }
      if let why = model.refusals[item.key] {
        Text(why).font(MVTheme.font(12)).foregroundStyle(MVTheme.accent)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
  }

  @ViewBuilder private func control(_ item: AddonSettingItem) -> some View {
    switch item.kind {
    case .toggle:
      Toggle(item.label, isOn: Binding(
        get: { item.flag },
        set: { var next = item; next.flag = $0; model.change(next) }))
        .toggleStyle(.switch).labelsHidden().fixedSize()
    case .choice:
      Picker(item.label, selection: Binding(
        get: { item.text },
        set: { var next = item; next.text = $0; model.change(next) })) {
        ForEach(item.options, id: \.self) { Text($0.label).tag($0.value) }
      }
      .pickerStyle(.menu).labelsHidden().fixedSize()
    case .number:
      NumberSettingField(item: item) { model.change($0) }
    case .text:
      TextSettingField(item: item) { model.change($0) }
    }
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }
}

/// A number: typed (committed on Return or when the field is left; text that
/// is not a number in range goes back), or stepped by `step`, then the unit.
private struct NumberSettingField: View {
  let item: AddonSettingItem
  let commit: (AddonSettingItem) -> Void
  @State private var typed = ""
  @FocusState private var focused: Bool

  var body: some View {
    HStack(spacing: MVTheme.space(6)) {
      TextField(item.label, text: $typed)
        .textFieldStyle(.roundedBorder).font(MVTheme.font(13))
        .frame(width: 72).multilineTextAlignment(.trailing)
        .focused($focused)
        .onSubmit { take() }
        .onChange(of: focused) { _, now in if !now { take() } }
      Stepper(item.label, value: Binding(
        get: { item.number },
        set: { set(Swift.min(item.max, Swift.max(item.min, $0))) }),
        in: item.min...item.max, step: item.step)
        .labelsHidden()
      if !item.unit.isEmpty {
        Text(item.unit).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      }
    }
    .onAppear { typed = item.numberText }
    .onChange(of: item.number) { _, _ in typed = item.numberText }
  }

  private func take() {
    let text = typed.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".")
    guard let v = Double(text), v >= item.min, v <= item.max else {
      typed = item.numberText
      return
    }
    set(v)
  }

  private func set(_ v: Double) {
    guard v != item.number else { return }
    var next = item
    next.number = (v * 1000).rounded() / 1000  // three decimals, what the core keeps
    commit(next)
  }
}

/// Text, committed on Return or when the field is left; at most max_length.
private struct TextSettingField: View {
  let item: AddonSettingItem
  let commit: (AddonSettingItem) -> Void
  @State private var typed = ""
  @FocusState private var focused: Bool

  var body: some View {
    TextField(item.placeholder.isEmpty ? item.label : item.placeholder, text: $typed)
      .textFieldStyle(.roundedBorder).font(MVTheme.font(13))
      .frame(maxWidth: 240)
      .focused($focused)
      .onSubmit { take() }
      .onChange(of: focused) { _, now in if !now { take() } }
      .onChange(of: typed) { _, now in
        if now.utf8.count > item.maxLength { typed = String(now.prefix(item.maxLength)) }
      }
      .onAppear { typed = item.text }
  }

  private func take() {
    guard typed != item.text else { return }
    var next = item
    next.text = typed
    commit(next)
  }
}

// MARK: - Key layouts

/// Settings → Keyboard shortcuts: the key layouts installed add-ons offer
/// (docs/design/16 "Keymap packs"). Applied once, on a click: the defaults with
/// the layout's keys written in. Reset to default puts the defaults back.
struct KeyLayoutRow: View {
  @ObservedObject private var addons = OpenAddonStore.shared
  @ObservedObject private var settings = SettingsStore.shared
  @State private var chosen = ""
  @State private var note = ""
  @State private var busy = false

  private var layouts: [(key: String, addon: String, pack: String, name: String)] {
    addons.installed.filter { $0.state == "ok" }.flatMap { a in
      a.keymaps.map { (key: a.addonID + "/" + $0.id, addon: a.addonID, pack: $0.id,
                       name: "\($0.name) (\(a.name))") }
    }
  }

  var body: some View {
    if !layouts.isEmpty {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        HStack(spacing: MVTheme.space(12)) {
          Text("Key layout").font(MVTheme.font()).foregroundStyle(MVTheme.title)
          Picker("Key layout", selection: $chosen) {
            ForEach(layouts, id: \.key) { Text($0.name).tag($0.key) }
          }
          .pickerStyle(.menu).labelsHidden().fixedSize()
          Button("Apply") { apply() }.disabled(busy || chosen.isEmpty)
          Spacer()
        }
        if !note.isEmpty {
          Text(note).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
      .onAppear { if chosen.isEmpty { chosen = layouts.first?.key ?? "" } }
    }
  }

  private func apply() {
    guard let layout = layouts.first(where: { $0.key == chosen }) else { return }
    busy = true
    Task.detached(priority: .userInitiated) {
      let pack = OpenAddonBridge.read { mv_open_addons_keymap(layout.addon, layout.pack, $0, $1) }
      await MainActor.run {
        busy = false
        guard !pack.isEmpty else {
          note = "\(layout.name) is no longer installed."
          return
        }
        let report = OpenAddonBridge.read(capacity: 8192) { mv_chrome_apply_keymap(pack, $0, $1) }
        let obj = (try? JSONSerialization.jsonObject(with: Data(report.utf8))) as? [String: Any] ?? [:]
        let skipped = obj["skipped"] as? [String] ?? []
        let displaced = obj["displaced"] as? Int ?? 0
        var text = "Applied \(layout.name). Reset to default puts back the default keys."
        if displaced > 0 {
          text += " \(displaced) shortcut\(displaced == 1 ? " is" : "s are") now unbound: the layout took \(displaced == 1 ? "its key" : "their keys")."
        }
        if !skipped.isEmpty { text += " Not applied: " + skipped.joined(separator: "; ") + "." }
        note = text
        settings.keysChanged()
      }
    }
  }
}
