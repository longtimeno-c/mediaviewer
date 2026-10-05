// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 (docs/design/06, docs/design/16 `I`): the metadata pane. Summary card, the searchable full
// tag tree, and -- for a clip -- the per-stream inspector. A field the file does
// not have shows as a dash; a file with no metadata at all is an empty pane, never
// an error.
//
// PR 29 (owner, 2026-09-26; docs/design/12): every tag is editable here, not just a
// rating and a comment. The date taken has its own editor (it moves every
// capture-time tag together), location can be removed in one go, and each tag
// in the tree can be edited or removed where the file allows it. A lock marks
// the rows that describe the file itself. Edits queue on the host and land on
// its I/O pool; Revert puts every tag back as it was before this session.
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
  @State private var addingTag = false
  @State private var newKey = ""
  @State private var newValue = ""
  @FocusState private var fieldFocused: Bool

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
      .padding(.horizontal, 14)
      .padding(.top, 12)
      HStack(spacing: 4) {
        ForEach(tabs) { t in
          Button { tab = t } label: { Text(t.rawValue).frame(maxWidth: .infinity) }
            .buttonStyle(FlatButtonStyle(selected: tab == t, compact: true))
            .accessibilityAddTraits(tab == t ? [.isSelected] : [])
        }
      }
      .padding(.horizontal, 10)
      .padding(.vertical, 8)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      Group {
        switch tab {
        case .summary: summaryCard
        case .tags: tagTree
        case .streams: streamInspector
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    .background(MVTheme.canvas)
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .onChange(of: store.isClip) { _, isClip in
      if !isClip && tab == .streams { tab = .summary }
    }
    // Another item: drop any half-typed edit.
    .onChange(of: store.summary.first?.value) { _, _ in cancelEdits() }
  }

  private func cancelEdits() {
    editing = nil
    editingDate = false
    addingTag = false
    fieldFocused = false
  }

  // MARK: Summary

  private var summaryCard: some View {
    ScrollView {
      if store.summary.isEmpty {
        Text(store.loading ? "Reading…" : "Nothing selected")
          .font(MVTheme.font(14)).foregroundStyle(MVTheme.body).padding(20)
      } else {
        VStack(alignment: .leading, spacing: 8) {
          ForEach(store.summary) { row in
            if row.label == "Date taken" {
              dateRow(row)
            } else {
              HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text(row.label).foregroundStyle(MVTheme.body).frame(width: 100, alignment: .leading)
                Text(row.value.isEmpty ? "—" : row.value)
                  .foregroundStyle(row.value.isEmpty ? MVTheme.disabled : MVTheme.title)
                  .textSelection(.enabled)
                  .frame(maxWidth: .infinity, alignment: .leading)
                if row.label == "Location" && !row.value.isEmpty && !store.locationKeys.isEmpty {
                  Button("Remove") { store.locationKeys.forEach { store.removeTag($0) } }
                    .buttonStyle(FlatButtonStyle(compact: true))
                    .help("Remove every GPS tag from this file")
                    .accessibilityLabel("Remove location")
                }
              }
              .font(MVTheme.font(14))
            }
          }
          Rectangle().fill(MVTheme.hairline).frame(height: 1).padding(.vertical, 4)
          HStack {
            Text("Edit any tag under All tags.").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            Spacer(minLength: 0)
            Button("Revert all") { store.revert() }
              .buttonStyle(FlatButtonStyle(compact: true))
              .disabled(!store.canRevert)
              .help("Put every tag back to how this file was before this session's first change")
          }
        }
        .padding(14)
      }
    }
  }

  private func dateRow(_ row: MetaRow) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      HStack(alignment: .firstTextBaseline, spacing: 10) {
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
        HStack(spacing: 6) {
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
      HStack(spacing: 6) {
        TextField("Search tags and values", text: $query)
          .textFieldStyle(.roundedBorder)
          .font(MVTheme.font(14))
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
      .padding(10)
      if addingTag { addTagForm }
      if store.properties.isEmpty && !addingTag {
        Text(store.loading ? "Reading…" : "No metadata in this file")
          .font(MVTheme.font(14)).foregroundStyle(MVTheme.body).padding(20)
        Spacer()
      } else {
        List {
          ForEach(groups, id: \.name) { group in
            Section(group.name) {
              ForEach(group.rows) { p in tagRow(p) }
            }
          }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
      }
    }
  }

  private var addTagForm: some View {
    VStack(alignment: .leading, spacing: 6) {
      TextField("Key (Xmp.dc.subject)", text: $newKey)
        .textFieldStyle(.roundedBorder)
        .focused($fieldFocused)
        .accessibilityLabel("New tag key")
      TextField("Value", text: $newValue)
        .textFieldStyle(.roundedBorder)
        .onSubmit(addTag)
        .accessibilityLabel("New tag value")
      HStack(spacing: 6) {
        Button("Add") { addTag() }
          .buttonStyle(FlatButtonStyle(selected: true, compact: true))
          .disabled(newKey.isEmpty || newValue.isEmpty)
        Button("Cancel") { cancelEdits() }.buttonStyle(FlatButtonStyle(compact: true))
      }
    }
    .font(MVTheme.font(14))
    .padding(.horizontal, 10)
    .padding(.bottom, 10)
  }

  private func addTag() {
    let key = newKey.trimmingCharacters(in: .whitespaces)
    guard !key.isEmpty, !newValue.isEmpty else { return }
    store.setTag(key, newValue)
    cancelEdits()
  }

  private func tagRow(_ p: MetaProperty) -> some View {
    VStack(alignment: .leading, spacing: 3) {
      HStack(alignment: .firstTextBaseline, spacing: 6) {
        Text(p.label).font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        Spacer(minLength: 4)
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
      VStack(alignment: .leading, spacing: 14) {
        ForEach(store.streams) { stream in
          VStack(alignment: .leading, spacing: 6) {
            Text("#\(stream.id)  \(stream.kind.capitalized)  —  \(stream.codec)")
              .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
            ForEach(stream.fields) { f in
              HStack(alignment: .firstTextBaseline, spacing: 10) {
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
      .padding(14)
    }
  }

  private func timecode(_ ms: Int64) -> String {
    let s = ms / 1000
    return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
  }
}
