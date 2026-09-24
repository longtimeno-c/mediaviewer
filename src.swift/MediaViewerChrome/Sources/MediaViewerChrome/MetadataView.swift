// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9 (plan/06, plan/16 `I`): the metadata pane. Summary card, the searchable full
// tag tree, and -- for a clip -- the per-stream inspector. A field the file does
// not have shows as a dash; a file with no metadata at all is an empty pane, never
// an error.
import SwiftUI

struct MetadataView: View {
  @ObservedObject private var store = MetadataStore.shared
  @State private var tab: Tab = .summary
  @State private var query = ""

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
        Text("Metadata").font(.headline)
        Spacer()
        if store.loading { ProgressView().controlSize(.small) }
      }
      .padding(.horizontal, 14)
      .padding(.top, 12)
      Picker("", selection: $tab) {
        ForEach(tabs) { Text($0.rawValue).tag($0) }
      }
      .pickerStyle(.segmented)
      .labelsHidden()
      .padding(.horizontal, 14)
      .padding(.vertical, 10)
      Divider()
      Group {
        switch tab {
        case .summary: summaryCard
        case .tags: tagTree
        case .streams: streamInspector
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    .background(.regularMaterial)
    .overlay(alignment: .leading) { Divider() }
    .onChange(of: store.isClip) { _, isClip in
      if !isClip && tab == .streams { tab = .summary }
    }
  }

  // MARK: Summary

  private var summaryCard: some View {
    ScrollView {
      if store.summary.isEmpty {
        Text(store.loading ? "Reading…" : "Nothing selected").foregroundStyle(.secondary).padding(20)
      } else {
        VStack(alignment: .leading, spacing: 8) {
          ForEach(store.summary) { row in
            HStack(alignment: .firstTextBaseline, spacing: 10) {
              Text(row.label).foregroundStyle(.secondary).frame(width: 100, alignment: .leading)
              Text(row.value.isEmpty ? "—" : row.value)
                .foregroundStyle(row.value.isEmpty ? .tertiary : .primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
          }
        }
        .padding(14)
      }
    }
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
      TextField("Search tags and values", text: $query)
        .textFieldStyle(.roundedBorder)
        .padding(10)
      if store.properties.isEmpty {
        Text(store.loading ? "Reading…" : "No metadata in this file")
          .foregroundStyle(.secondary).padding(20)
        Spacer()
      } else {
        List {
          ForEach(groups, id: \.name) { group in
            Section(group.name) {
              ForEach(group.rows) { p in
                VStack(alignment: .leading, spacing: 1) {
                  Text(p.label).font(.callout)
                  Text(p.value.isEmpty ? "—" : p.value)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(3)
                    .textSelection(.enabled)
                }
                .help(p.rawTag)  // the untranslated origin, always one hover away
              }
            }
          }
        }
        .listStyle(.plain)
      }
    }
  }

  // MARK: Streams

  private var streamInspector: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
        ForEach(store.streams) { stream in
          VStack(alignment: .leading, spacing: 6) {
            Text("#\(stream.id)  \(stream.kind.capitalized)  —  \(stream.codec)").font(.subheadline.bold())
            ForEach(stream.fields) { f in
              HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text(f.label).foregroundStyle(.secondary).frame(width: 130, alignment: .leading)
                Text(f.value).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
              }
              .font(.callout)
            }
          }
        }
        if !store.chapters.isEmpty {
          Text("Chapters").font(.subheadline.bold())
          ForEach(store.chapters) { c in
            HStack {
              Text(timecode(c.startMs)).font(.system(.callout, design: .monospaced)).foregroundStyle(.secondary)
              Text(c.title.isEmpty ? "Chapter \(c.id + 1)" : c.title)
            }
          }
        }
        if store.streams.isEmpty {
          Text(store.loading ? "Reading…" : "No streams").foregroundStyle(.secondary)
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
