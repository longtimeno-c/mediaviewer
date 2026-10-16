// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 (docs/design/06, docs/design/16 `I`): the metadata pane. Summary card, the searchable full
// tag tree, and -- for a clip -- the per-stream inspector. A field the file does
// not have shows as a dash; a file with no metadata at all is an empty pane, never
// an error.
//
// PR 29 (owner, 2026-09-26; docs/design/12): every tag is editable here, not just a
// rating and a comment. The date taken has its own editor (it moves every
// capture-time tag together), so does the location (#352, owner 2026-10-12:
// coordinates typed in, blank removes after a confirmation), and each tag
// in the tree can be edited or removed where the file allows it. A lock marks
// the rows that describe the file itself. Edits queue on the host and land on
// its I/O pool; Revert puts every tag back as it was before this session.
import AppKit
import SwiftUI

struct MetadataView: View {
  @ObservedObject private var store = MetadataStore.shared
  @State private var tab: Tab = .summary
  @State private var query = ""
  // The row being edited in the tree (its raw_tag) and the text in its field.
  @State private var editing: String?
  @State private var draft = ""
  @State private var editingDate = false
  @State private var dateDraft = ""
  // #352: the Location editor, and the confirmation before a blank save removes it.
  @State private var editingLocation = false
  @State private var locationDraft = ""
  @State private var confirmRemoveLocation = false
  @State private var addingTag = false
  @State private var newKey = ""
  @State private var newValue = ""
  @FocusState private var fieldFocused: Bool
  // docs/design/16 `I` (issue #185): the pane holds the keyboard; ← → change tab,
  // ↓ reaches tag search, Esc hands the keyboard back to the canvas.
  @FocusState private var paneFocused: Bool
  @FocusState private var searchFocused: Bool

  private enum Tab: String, CaseIterable, Identifiable {
    case summary = "Summary"
    case tags = "All tags"
    case streams = "Streams"
    var id: String { rawValue }
  }

  private var tabs: [Tab] { store.isClip ? Tab.allCases : [.summary, .tags] }

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Text("Metadata").font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Spacer()
        if store.loading { ProgressView().controlSize(.small) }
      }
      .padding(.horizontal, MVTheme.space(14))
      .padding(.top, MVTheme.space(12))
      HStack(spacing: MVTheme.space(4)) {
        ForEach(tabs) { t in
          Button { tab = t } label: { Text(t.rawValue).frame(maxWidth: .infinity) }
            .buttonStyle(FlatButtonStyle(selected: tab == t, compact: true))
            .accessibilityAddTraits(tab == t ? [.isSelected] : [])
        }
      }
      .padding(MVTheme.space(4))
      // Where the keyboard is: the tab row is what ← → move.
      .overlay(
        RoundedRectangle(cornerRadius: MVTheme.radius(6))
          .strokeBorder(paneFocused ? MVTheme.accent : .clear, lineWidth: 2)
      )
      .padding(.horizontal, MVTheme.space(6))
      .padding(.vertical, MVTheme.space(4))
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      Group {
        switch tab {
        case .summary: summaryCard
        case .tags: tagTree
        case .streams: streamInspector
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      // docs/design/25 PR 59 (owner, 2026-10-12): the add-ons' sections (Places' label,
      // coordinates and map) are the last items of whichever tab is open, inside its
      // scrolling content: they scroll with the list and never squeeze it.
    }
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    // The pane's width, for the add-ons' surfaces (docs/design/25 PR 59).
    .background(GeometryReader { geo in
      Color.clear
        .onAppear { store.setPaneWidth(geo.size.width) }
        .onChange(of: geo.size.width) { _, w in store.setPaneWidth(w) }
    })
    .focusable()
    .focusEffectDisabled()
    .focused($paneFocused)
    // Only while the pane itself has the keyboard: a field keeps its own keys.
    .onKeyPress(.leftArrow) { paneFocused ? stepTab(-1) : .ignored }
    .onKeyPress(.rightArrow) { paneFocused ? stepTab(1) : .ignored }
    .onKeyPress(.downArrow) {
      guard paneFocused else { return .ignored }
      openTagSearch()
      return .handled
    }
    .onKeyPress(.escape) {
      guard paneFocused else { return .ignored }
      store.blur()
      return .handled
    }
    // `I` focuses the pane; Ctrl+I opens All tags with the keyboard in its search.
    .onChange(of: store.focusSeq) { _, _ in
      if store.focusTags { openTagSearch() } else { paneFocused = true }
    }
    .onChange(of: store.isClip) { _, isClip in
      if !isClip && tab == .streams { tab = .summary }
    }
    // Another item: drop any half-typed edit. Keyed on the item, not on a
    // value two files can share (issue #190).
    .onChange(of: store.itemPath) { _, _ in cancelEdits() }
  }

  private func stepTab(_ by: Int) -> KeyPress.Result {
    let all = tabs
    guard let at = all.firstIndex(of: tab) else { return .ignored }
    tab = all[min(max(at + by, 0), all.count - 1)]
    return .handled
  }

  private func openTagSearch() {
    cancelEdits()
    tab = .tags
    // The field exists only once the tab has switched: focus it on the next pass.
    DispatchQueue.main.async { searchFocused = true }
  }

  private func cancelEdits() {
    editing = nil
    editingDate = false
    editingLocation = false
    confirmRemoveLocation = false
    addingTag = false
    fieldFocused = false
  }

  // MARK: Summary

  private var summaryCard: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 0) {
      if store.summary.isEmpty {
        Text(store.loading ? "Reading…" : "Nothing selected")
          .font(MVTheme.font(14)).foregroundStyle(MVTheme.body).padding(MVTheme.space(20))
      } else {
        VStack(alignment: .leading, spacing: MVTheme.space(8)) {
          ForEach(store.summary) { row in
            if row.label == "Date taken" {
              dateRow(row)
            } else if row.label == "Location" {
              locationRow(row)
            } else if row.label == "Label" {
              labelRow(row)
            } else {
              HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(10)) {
                Text(row.label).foregroundStyle(MVTheme.body).frame(width: 100, alignment: .leading)
                Text(row.value.isEmpty ? "—" : row.value)
                  .foregroundStyle(row.value.isEmpty ? MVTheme.disabled : MVTheme.title)
                  .textSelection(.enabled)
                  .frame(maxWidth: .infinity, alignment: .leading)
              }
              .font(MVTheme.font(14))
            }
          }
          Rectangle().fill(MVTheme.hairline).frame(height: 1).padding(.vertical, MVTheme.space(4))
          HStack {
            Text("Edit any tag under All tags.").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            Spacer(minLength: 0)
            Button("Revert all") { store.revert() }
              .buttonStyle(FlatButtonStyle(compact: true))
              .disabled(!store.canRevert)
              .help("Put every tag back to how this file was before this session's first change")
          }
        }
        .padding(MVTheme.space(14))
      }
      AddonInfoSections()  // the last items, after the summary
      }
    }
  }

  /// Issue #207: the colour label, XMP xmp:Label in Lightroom's words. A swatch
  /// sets it (the same one again clears it, as the 6-9 keys do), through the same
  /// checked tag write as any other tag.
  private static let labelSwatches: [(name: String, colour: Color)] = [
    ("Red", Color(red: 0.90, green: 0.28, blue: 0.30)),
    ("Yellow", Color(red: 0.95, green: 0.76, blue: 0.18)),
    ("Green", Color(red: 0.24, green: 0.70, blue: 0.44)),
    ("Blue", Color(red: 0.23, green: 0.51, blue: 0.96)),
    ("Purple", Color(red: 0.61, green: 0.36, blue: 0.90)),
  ]

  private func labelRow(_ row: MetaRow) -> some View {
    HStack(alignment: .center, spacing: MVTheme.space(10)) {
      Text(row.label).foregroundStyle(MVTheme.body).frame(width: 100, alignment: .leading)
      Text(row.value.isEmpty ? "—" : row.value)
        .foregroundStyle(row.value.isEmpty ? MVTheme.disabled : MVTheme.title)
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
      HStack(spacing: MVTheme.space(4)) {
        ForEach(Self.labelSwatches, id: \.name) { swatch in
          let on = swatch.name.caseInsensitiveCompare(row.value) == .orderedSame
          Button {
            if on { store.removeTag("Xmp.xmp.Label") } else { store.setTag("Xmp.xmp.Label", swatch.name) }
          } label: {
            Circle()
              .fill(swatch.colour)
              .frame(width: 14, height: 14)
              .overlay(Circle().stroke(on ? MVTheme.title : MVTheme.hairline, lineWidth: on ? 2 : 1))
          }
          .buttonStyle(.plain)
          .disabled(!store.canEdit)
          .help(on ? "Clear the \(swatch.name.lowercased()) label" : swatch.name)
          .accessibilityLabel(on ? "Clear label" : "Label \(swatch.name.lowercased())")
        }
      }
    }
    .font(MVTheme.font(14))
  }

  private func dateRow(_ row: MetaRow) -> some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(10)) {
        Text(row.label).foregroundStyle(MVTheme.body).frame(width: 100, alignment: .leading)
        if editingDate {
          TextField("YYYY-MM-DD HH:MM:SS", text: $dateDraft)
            .textFieldStyle(.roundedBorder)
            .font(MVTheme.font(14))
            .focused($fieldFocused)
            .onSubmit { saveDate() }
            .onExitCommand { cancelEdits(); store.blur() }
            .accessibilityLabel("Date taken, year month day hours minutes seconds")
        } else {
          Text(row.value.isEmpty ? "—" : row.value)
            .foregroundStyle(row.value.isEmpty ? MVTheme.disabled : MVTheme.title)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
          Button("Edit") {
            dateDraft = row.value.isEmpty ? "" : String(row.value.prefix(19))
            editingDate = true
            fieldFocused = true
          }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(!store.canEdit)
          .help("Change when this was taken; every date tag in the file moves together")
          .accessibilityLabel("Edit date taken")
        }
      }
      if editingDate {
        HStack(spacing: MVTheme.space(6)) {
          Spacer().frame(width: 100)
          Button("Save") { saveDate() }.buttonStyle(FlatButtonStyle(selected: true, compact: true))
          Button("Remove") {
            store.removeDate()
            cancelEdits()
          }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(row.value.isEmpty)
          .help("Remove every date-taken tag")
          Button("Cancel") { cancelEdits(); store.blur() }.buttonStyle(FlatButtonStyle(compact: true))
        }
      }
    }
    .font(MVTheme.font(14))
  }

  // MARK: Location (#352)

  /// One line under the field: the point Save will write, or why it will not.
  private func locationNote(_ parsed: LocationParse) -> (text: String, error: Bool) {
    switch parsed {
    case .point(_, _, let preview): return ("\u{2192} " + preview, false)
    case .blank:
      if store.locationRights.canClear { return ("Saving an empty field removes the location", false) }
      if store.locationRights.inOriginal {
        return ("This file's own location is in the original, which is never rewritten: change it, not remove it", true)
      }
      return ("Enter latitude and longitude, e.g. 38.7979, -9.3907", false)
    case .invalid(let why): return (why, true)
    }
  }

  private var locationEditHelp: String {
    if !store.canEdit || !store.locationRights.canSet { return "This item cannot be written" }
    if store.locationRights.inOriginal {
      return "Change where this was taken. The original keeps its own; the new one goes in the XMP sidecar, which wins"
    }
    return "Change or remove where this was taken (decimal or degrees, minutes, seconds)"
  }

  private func locationRow(_ row: MetaRow) -> some View {
    let note = locationNote(store.parseLocation(locationDraft))
    return VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(10)) {
        Text(row.label).foregroundStyle(MVTheme.body).frame(width: 100, alignment: .leading)
        if editingLocation {
          TextField("38.7979, -9.3907", text: $locationDraft)
            .textFieldStyle(.roundedBorder)
            .font(MVTheme.font(14))
            .focused($fieldFocused)
            .onSubmit { saveLocation() }
            .onExitCommand { cancelEdits(); store.blur() }
            .accessibilityLabel("Location, latitude and longitude")
            .accessibilityHint(note.text)
        } else {
          Text(row.value.isEmpty ? "—" : row.value)
            .foregroundStyle(row.value.isEmpty ? MVTheme.disabled : MVTheme.title)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
          Button("Edit") {
            cancelEdits()
            locationDraft = store.locationText
            editingLocation = true
            fieldFocused = true
          }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(!store.canEdit || !store.locationRights.canSet)
          .help(locationEditHelp)
          .accessibilityLabel("Edit location")
        }
      }
      if editingLocation {
        HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(4)) {
          Spacer().frame(width: 106)
          if note.error {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Color(nsColor: .systemRed))
              .accessibilityHidden(true)
          }
          Text(note.text)
            .font(MVTheme.font(12))
            .foregroundStyle(note.error ? Color(nsColor: .systemRed) : MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityLabel(note.error ? "Error: " + note.text : note.text)
        }
        HStack(spacing: MVTheme.space(6)) {
          Spacer().frame(width: 100)
          Button("Save") { saveLocation() }.buttonStyle(FlatButtonStyle(selected: true, compact: true))
            .help("Return")
          Button("Cancel") { cancelEdits(); store.blur() }.buttonStyle(FlatButtonStyle(compact: true))
            .help("Esc")
        }
      }
    }
    .font(MVTheme.font(14))
    // A native alert: Esc and Return both land on Cancel; Remove must be chosen.
    .alert("Remove location from this file?", isPresented: $confirmRemoveLocation) {
      Button("Remove", role: .destructive) {
        store.setLocation("")
        cancelEdits()
        store.blur()
      }
      .accessibilityLabel("Remove location")
      Button("Cancel", role: .cancel) { fieldFocused = true }
        .keyboardShortcut(.defaultAction)
    } message: {
      // Revert all restores every tag as it was before this session's first
      // write to the file, until the app quits (meta::revert, docs/design/06).
      Text(MetadataView.removeLocationMessage)
    }
  }

  /// The confirmation's second line; Windows says the same (IslandHost.Panels.cs).
  static let removeLocationMessage =
    "Revert all can bring it back until you quit MediaViewer. It also undoes your other changes to this file."

  private func saveLocation() {
    switch store.parseLocation(locationDraft) {
    case .point(let lat, let lon, _):
      // Unchanged: no write, as an unchanged comment makes none. The starting
      // text is rounded to 6 decimals, so saving it untouched must not count as
      // an edit of a position the file holds more precisely.
      let untouched = locationDraft.trimmingCharacters(in: .whitespaces) == store.locationText
      if untouched && store.location != nil { cancelEdits(); store.blur(); return }
      if let at = store.location, abs(at.lat - lat) < 1e-9, abs(at.lon - lon) < 1e-9 {
        cancelEdits()
        store.blur()
        return
      }
      store.setLocation(locationDraft)
      cancelEdits()
      store.blur()
    case .blank:
      if store.locationRights.canClear { confirmRemoveLocation = true } else { NSSound.beep() }
    case .invalid:
      NSSound.beep()
    }
  }

  private func saveDate() {
    store.setDate(dateDraft.trimmingCharacters(in: .whitespaces))
    cancelEdits()
    store.blur()
  }

  // MARK: Full tree

  private var groups: [(name: String, rows: [MetaProperty])] {
    let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
    let matching = needle.isEmpty
      ? store.properties
      : store.properties.filter {
        $0.label.lowercased().contains(needle) || $0.value.lowercased().contains(needle)
          || $0.rawTag.lowercased().contains(needle)
      }
    var order: [String] = []
    var by: [String: [MetaProperty]] = [:]
    for p in matching {
      if by[p.group] == nil { order.append(p.group) }
      by[p.group, default: []].append(p)
    }
    return order.map { ($0, by[$0] ?? []) }
  }

  private var tagTree: some View {
    VStack(spacing: 0) {
      HStack(spacing: MVTheme.space(6)) {
        TextField("Search tags and values", text: $query)
          .textFieldStyle(.roundedBorder)
          .font(MVTheme.font(14))
          .focused($searchFocused)
          // Esc returns to the canvas (docs/design/16 `I`).
          .onExitCommand { store.blur() }
        Button("Add tag") {
          cancelEdits()
          newKey = ""
          newValue = ""
          addingTag = true
          fieldFocused = true
        }
        .buttonStyle(FlatButtonStyle(compact: true))
        .disabled(!store.canEdit)
        .help("Add a tag by its key, e.g. Xmp.dc.subject or Exif.Image.Artist")
      }
      .padding(MVTheme.space(10))
      if addingTag { addTagForm }
      if store.properties.isEmpty && !addingTag {
        ScrollView {
          VStack(alignment: .leading, spacing: 0) {
            Text(store.loading ? "Reading…" : "No metadata in this file")
              .font(MVTheme.font(14)).foregroundStyle(MVTheme.body).padding(MVTheme.space(20))
            AddonInfoSections()
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      } else {
        List {
          ForEach(groups, id: \.name) { group in
            Section(group.name) {
              ForEach(group.rows) { p in tagRow(p) }
            }
          }
          // The add-ons' sections: the last rows, after every tag.
          AddonInfoSections()
            .listRowInsets(EdgeInsets())
            .listRowSeparator(.hidden)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
      }
    }
  }

  private var addTagForm: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      TextField("Key (Xmp.dc.subject)", text: $newKey)
        .textFieldStyle(.roundedBorder)
        .focused($fieldFocused)
        .accessibilityLabel("New tag key")
      TextField("Value", text: $newValue)
        .textFieldStyle(.roundedBorder)
        .onSubmit(addTag)
        .accessibilityLabel("New tag value")
      HStack(spacing: MVTheme.space(6)) {
        Button("Add") { addTag() }
          .buttonStyle(FlatButtonStyle(selected: true, compact: true))
          .disabled(newKey.isEmpty || newValue.isEmpty)
        Button("Cancel") { cancelEdits() }.buttonStyle(FlatButtonStyle(compact: true))
      }
    }
    .font(MVTheme.font(14))
    .padding(.horizontal, MVTheme.space(10))
    .padding(.bottom, MVTheme.space(10))
  }

  private func addTag() {
    let key = newKey.trimmingCharacters(in: .whitespaces)
    guard !key.isEmpty, !newValue.isEmpty else { return }
    store.setTag(key, newValue)
    cancelEdits()
  }

  private func tagRow(_ p: MetaProperty) -> some View {
    VStack(alignment: .leading, spacing: MVTheme.space(3)) {
      HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(6)) {
        Text(p.label).font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        Spacer(minLength: MVTheme.space(4))
        if p.editable && editing != p.rawTag {
          Button("Edit") {
            cancelEdits()
            draft = p.raw
            editing = p.rawTag
            fieldFocused = true
          }
          .buttonStyle(FlatButtonStyle(compact: true))
          .help(p.access == "s" ? "Saved to the XMP sidecar; the original is never rewritten" : "Edit this tag")
          .accessibilityLabel("Edit \(p.label)")
          if p.removable {
            Button("Remove") { store.removeTag(p.rawTag) }
              .buttonStyle(FlatButtonStyle(compact: true))
              .accessibilityLabel("Remove \(p.label)")
          }
        } else if !p.editable {
          Image(systemName: "lock")
            .foregroundStyle(MVTheme.disabled)
            .help(p.lockReason)
            .accessibilityLabel("Read-only: \(p.lockReason)")
        }
      }
      if editing == p.rawTag {
        TextField(p.label, text: $draft)
          .textFieldStyle(.roundedBorder)
          .font(MVTheme.font(13))
          .focused($fieldFocused)
          .onSubmit {
            if draft != p.raw { store.setTag(p.rawTag, draft) }
            cancelEdits()
            store.blur()
          }
          .onExitCommand { cancelEdits(); store.blur() }
          .accessibilityLabel("New value for \(p.label)")
        Text("Return saves · Esc cancels").font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
      } else {
        Text(p.value.isEmpty ? "—" : p.value)
          .font(.system(.caption, design: .monospaced))
          .foregroundStyle(MVTheme.body)
          .lineLimit(3)
          .textSelection(.enabled)
      }
    }
    .help(p.rawTag)  // the untranslated origin, always one hover away
  }

  // MARK: Streams

  private var streamInspector: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: MVTheme.space(14)) {
        ForEach(store.streams) { stream in
          VStack(alignment: .leading, spacing: MVTheme.space(6)) {
            Text("#\(stream.id)  \(stream.kind.capitalized)  —  \(stream.codec)")
              .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
            ForEach(stream.fields) { f in
              HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(10)) {
                Text(f.label).foregroundStyle(MVTheme.body).frame(width: 130, alignment: .leading)
                Text(f.value).foregroundStyle(MVTheme.title).textSelection(.enabled)
                  .frame(maxWidth: .infinity, alignment: .leading)
              }
              .font(MVTheme.font(13))
            }
          }
        }
        if !store.chapters.isEmpty {
          Text("Chapters").font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
          ForEach(store.chapters) { c in
            HStack {
              Text(timecode(c.startMs)).font(.system(.callout, design: .monospaced)).foregroundStyle(MVTheme.body)
              Text(c.title.isEmpty ? "Chapter \(c.id + 1)" : c.title).foregroundStyle(MVTheme.title)
            }
          }
        }
        if store.streams.isEmpty {
          Text(store.loading ? "Reading…" : "No streams").foregroundStyle(MVTheme.body)
        }
      }
      .padding(MVTheme.space(14))
      AddonInfoSections()  // the last items, after the streams
    }
  }

  private func timecode(_ ms: Int64) -> String {
    let s = ms / 1000
    return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
  }
}
