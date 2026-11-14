// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "This computer" → Drives (docs/plans/self-host-drives.md §6, owner 2026-11-14): the
// drives on this Mac, and the five ways to use two or more of them offered equally,
// side by side: Separate, Mirror (recommended), Span, Stripe (warned), Parity. "RAID is
// not a backup" always sits above them. A combined choice erases: the card shows what
// is on each drive and the person types each drive's name before Combine is enabled;
// the program checks the same names again and macOS asks for an administrator in its
// own prompt. Everything runs in the driftwire program on the link's thread
// (phone/link.h "host_drives…"); this view only sends requests and draws state_json.
// The Windows twin is IslandHost.PhoneDrives.cs.
import SwiftUI

struct PhoneDrivesSection: View {
  @ObservedObject private var model = PhoneStore.shared
  @State private var open = false
  @State private var picked: [String] = []          // drive ids, in the list's order
  @State private var choice: String?
  @State private var label = ""
  @State private var typed: [String: String] = [:]  // drive id → the name typed
  @State private var replacing: String?             // a set's id
  @State private var replaceWith: String?
  @State private var replaceTyped = ""
  @State private var asked = ""                     // "combine" | "replace" until its outcome shows
  private var s: PhoneStore.State { model.state }
  private var idle: Bool { s.busy.isEmpty }
  private var listing: PhoneDrives.Listing? { s.drives }

  var body: some View {
    Group {
      if let l = listing { ForEach(l.sets) { set in setRow(set, l) } }
      if !open {
        SettingsRow(title: "Drives",
                    detail: "Share each drive as it is, or erase two or more and combine them into one.",
                    labelsContent: false) {
          SettingsButton(title: "Use two or more drives…") {
            open = true
            model.send(["do": "host_drives"])
          }
          .disabled(!idle)
          .accessibilityLabel("Choose drives on this Mac for the server")
        }
      } else {
        chooser
      }
      if let line = outcome {
        Text(line).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
          .padding(.horizontal, MVTheme.space(12))
      }
    }
    .onChange(of: picked) { _, ids in
      choice = nil
      if !ids.isEmpty { model.send(["do": "host_drives_choices", "ids": ids]) }
    }
    .onChange(of: s.busy) { _, busy in
      // The program's last line once a combine or replace ends (an error says its own).
      if busy.isEmpty && !asked.isEmpty && !s.error.isEmpty { asked = "" }
    }
  }

  /// "Made "Photos" (mirror, 2 TB); shared as "Photos"." once a run this card asked for ends.
  private var outcome: String? {
    guard !asked.isEmpty, idle, s.error.isEmpty, !s.hostSaid.isEmpty else { return nil }
    return s.hostSaid
  }

  // MARK: the drives and the five choices

  @ViewBuilder private var chooser: some View {
    if let l = listing {
      if !l.error.isEmpty {
        SettingsRow(title: "Drives", detail: l.error) { EmptyView() }
      }
      ForEach(l.disks) { d in diskRow(d) }
      if l.disks.isEmpty {
        SettingsRow(title: "Drives", detail: "No drives besides this Mac's own. Connect the drives and choose Check again.",
                    labelsContent: false) {
          SettingsButton(title: "Check again") { model.send(["do": "host_drives"]) }.disabled(!idle)
        }
      }
      if !picked.isEmpty, let c = s.drivesChoices { options(c) }
      if let o = chosenOption { confirm(o, l) }
      HStack {
        Spacer()
        SettingsButton(title: "Close") { close() }.disabled(!idle)
          .accessibilityLabel("Close the drives step")
      }
    } else if idle {
      SettingsRow(title: "Drives", detail: "The drives on this Mac couldn't be listed.", labelsContent: false) {
        SettingsButton(title: "Check again") { model.send(["do": "host_drives"]) }
      }
    }
  }

  private func diskRow(_ d: PhoneDrives.Disk) -> some View {
    let on = Binding(
      get: { picked.contains(d.id) },
      set: { want in
        guard let l = listing else { return }
        var ids = picked.filter { $0 != d.id }
        if want { ids.append(d.id) }
        picked = l.disks.map(\.id).filter { ids.contains($0) }  // keep the list's order
      })
    let detail = d.refused.isEmpty ? d.facts : d.facts + "\nCan't be erased: " + d.refused
    return SettingsRow(title: d.name, detail: detail) {
      Toggle(d.name, isOn: on).toggleStyle(.checkbox).disabled(!idle)
    }
  }

  private func options(_ c: PhoneDrives.Choices) -> some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      Text(c.notABackup).font(MVTheme.font(12)).fontWeight(.semibold).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      HStack(alignment: .top, spacing: MVTheme.space(8)) {
        ForEach(c.options) { o in optionCard(o) }
      }
      .fixedSize(horizontal: false, vertical: true)  // the cards share one height
    }
    .padding(.horizontal, MVTheme.space(12))
    .opacity(s.busy == "host_drives_choices" ? 0.5 : 1)
  }

  private func optionCard(_ o: PhoneDrives.Option) -> some View {
    let chosen = choice == o.choice
    return Button {
      choice = o.choice
      typed = [:]
      if o.erases && label.isEmpty { label = "Photos" }
    } label: {
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
        Text(o.title).font(MVTheme.font(13)).fontWeight(.semibold)
          .foregroundStyle(o.offered ? MVTheme.title : MVTheme.disabled)
          .fixedSize(horizontal: false, vertical: true)
        if o.recommended {
          Label("For photos", systemImage: "checkmark.circle.fill")
            .font(MVTheme.font(11)).foregroundStyle(MVTheme.accent)
        }
        if o.warning {
          Label("Risky", systemImage: "exclamationmark.triangle.fill")
            .font(MVTheme.font(11)).foregroundStyle(MVTheme.warm)
        }
        if o.offered {
          words(o.spaceWords)
          words("If one dies: " + o.deadWords)
          words(o.speedWords)
        } else {
          words(o.whyNot)
        }
        Spacer(minLength: 0)
      }
      .padding(MVTheme.space(12))
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(8))
        .fill(chosen ? MVTheme.raised : MVTheme.surface))
      .settingsCardOutline(MVTheme.radius(8))
      .contentShape(Rectangle())
      .animation(.easeOut(duration: 0.15), value: chosen)
    }
    .buttonStyle(.plain)
    .disabled(!o.offered || !idle)
    .accessibilityLabel(o.offered ? o.title : "\(o.title), not available: \(o.whyNot)")
    .accessibilityAddTraits(chosen ? .isSelected : [])
  }

  private func words(_ text: String) -> some View {
    Text(text).font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }

  // MARK: confirming

  private var chosenOption: PhoneDrives.Option? {
    guard let c = choice, !picked.isEmpty else { return nil }
    return s.drivesChoices?.options.first { $0.choice == c && $0.offered }
  }

  private func pickedDisks(_ l: PhoneDrives.Listing) -> [PhoneDrives.Disk] {
    picked.compactMap { id in l.disks.first { $0.id == id } }
  }

  @ViewBuilder private func confirm(_ o: PhoneDrives.Option, _ l: PhoneDrives.Listing) -> some View {
    let disks = pickedDisks(l)
    if !o.erases {
      SettingsRow(title: "Share each drive as it is",
                  detail: "Nothing is erased. Each drive becomes its own folder for your phone.",
                  labelsContent: false) {
        SettingsButton(title: "Share") { combine(o, disks) }
          .disabled(!idle)
          .accessibilityLabel("Share each picked drive as it is")
      }
    } else {
      let refused = disks.filter { !$0.refused.isEmpty }
      SettingsRow(title: "Name of the combined drive",
                  detail: "1 to 27 letters, digits, spaces, - or _. It is shared under this name.") {
        TextField("Photos", text: $label)
          .textFieldStyle(.roundedBorder).frame(width: 200).font(MVTheme.font(12))
          .accessibilityLabel("Name of the combined drive")
      }
      ForEach(disks) { d in
        SettingsRow(title: d.name, detail: d.facts) {
          TextField("Type “\(d.name)” to erase it", text: Binding(
            get: { typed[d.id] ?? "" }, set: { typed[d.id] = $0 }))
            .textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
            .accessibilityLabel("Type \(d.name) to erase it")
        }
      }
      SettingsRow(title: "\(o.title): erase and combine",
                  detail: refused.isEmpty
                    ? "Everything on these drives is erased. macOS asks for an administrator once."
                    : refused.map { "\($0.name) can't be erased: \($0.refused)" }.joined(separator: "\n"),
                  labelsContent: false) {
        SettingsButton(title: "Erase and combine") { combine(o, disks) }
          .disabled(!canCombine(disks) || !refused.isEmpty)
          .accessibilityLabel("Erase the picked drives and combine them")
      }
    }
  }

  private func canCombine(_ disks: [PhoneDrives.Disk]) -> Bool {
    idle && PhoneDrives.goodLabel(label.trimmingCharacters(in: .whitespaces))
      && PhoneDrives.namesMatch(typed: disks.map { typed[$0.id] ?? "" }, disks: disks)
  }

  private func combine(_ o: PhoneDrives.Option, _ disks: [PhoneDrives.Disk]) {
    guard idle, !disks.isEmpty else { return }
    if o.erases && !canCombine(disks) { return }
    asked = "combine"
    model.send(PhoneDrives.combineRequest(choice: o.choice, disks: disks, label: label,
                                          typed: disks.map { typed[$0.id] ?? "" }))
    typed = [:]
    choice = nil
    picked = []
  }

  private func close() {
    open = false
    picked = []
    choice = nil
    typed = [:]
  }

  // MARK: a set's health, and replacing a drive

  @ViewBuilder private func setRow(_ set: PhoneDrives.DriveSet, _ l: PhoneDrives.Listing) -> some View {
    SettingsRow(title: set.name, detail: set.line, labelsContent: false) {
      if set.canTakeADrive && replacing != set.id {
        SettingsButton(title: "Replace a drive…") {
          replacing = set.id
          replaceWith = nil
          replaceTyped = ""
        }
        .disabled(!idle)
        .accessibilityLabel("Replace the lost drive in \(set.name)")
      }
    }
    if replacing == set.id { replaceRows(set, l) }
  }

  @ViewBuilder private func replaceRows(_ set: PhoneDrives.DriveSet, _ l: PhoneDrives.Listing) -> some View {
    let candidates = PhoneDrives.replacements(for: set, in: l)
    if candidates.isEmpty {
      SettingsRow(title: "A drive to take its place",
                  detail: "Connect a drive at least as big as the others, then choose Check again.",
                  labelsContent: false) {
        SettingsButton(title: "Check again") { model.send(["do": "host_drives"]) }.disabled(!idle)
      }
    } else {
      SettingsRow(title: "A drive to take its place", detail: "Everything on it is erased, and the mirror rebuilds onto it.") {
        Picker("New drive", selection: Binding(get: { replaceWith ?? "" }, set: { replaceWith = $0; replaceTyped = "" })) {
          Text("Choose…").tag("")
          ForEach(candidates) { d in Text(d.name).tag(d.id) }
        }
        .frame(width: 200)
      }
      if let id = replaceWith, let d = candidates.first(where: { $0.id == id }) {
        SettingsRow(title: d.name, detail: d.facts) {
          TextField("Type “\(d.name)” to erase it", text: $replaceTyped)
            .textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
            .accessibilityLabel("Type \(d.name) to erase it")
        }
      }
    }
    HStack(spacing: MVTheme.space(8)) {
      Spacer()
      SettingsButton(title: "Cancel") { replacing = nil }.disabled(!idle)
      SettingsButton(title: "Erase and replace") { replace(set, candidates) }
        .disabled(!canReplace(candidates))
        .accessibilityLabel("Erase the new drive and rebuild \(set.name) onto it")
    }
  }

  private func canReplace(_ candidates: [PhoneDrives.Disk]) -> Bool {
    guard idle, let id = replaceWith, let d = candidates.first(where: { $0.id == id }) else { return false }
    return PhoneDrives.namesMatch(typed: [replaceTyped], disks: [d])
  }

  private func replace(_ set: PhoneDrives.DriveSet, _ candidates: [PhoneDrives.Disk]) {
    guard canReplace(candidates), let id = replaceWith else { return }
    asked = "replace"
    model.send(["do": "host_drives_replace", "set": set.name, "ids": [id],
                "name": replaceTyped.trimmingCharacters(in: .whitespacesAndNewlines)])
    replacing = nil
    replaceWith = nil
    replaceTyped = ""
  }
}
