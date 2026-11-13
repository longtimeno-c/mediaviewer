// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Search: field, recent searches, then results. States: typing, results, nothing found. Filters
// narrow by person, place, date, camera and kind, and work with or without words.
import MobileKit
import SwiftUI

struct SearchScreen: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    let search = model.search
    NavigationStack {
      content
        .background(Theme.ground)
        .safeAreaInset(edge: .top, spacing: 0) { FilterRow() }
        .navigationTitle("Search")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: Binding(get: { search.text }, set: { search.setText($0) }),
                    placement: .navigationBarDrawer(displayMode: .always), prompt: "Type what you remember")
        .onSubmit(of: .search) { search.submit() }
    }
    .task { await search.prepare() }
  }

  /// One answer glides into the next: tiles that stay move to their new place, new ones fade in,
  /// gone ones fade out. A spring, so a quicker answer takes over from one still moving. Reduce
  /// Motion cuts.
  private var answerMotion: Animation? {
    reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.9)
  }

  /// The last answer stays on screen until the next is ready (`SearchModel.shown`).
  private var content: some View {
    shownContent
      .animation(answerMotion, value: model.search.answer)
  }

  @ViewBuilder
  private var shownContent: some View {
    let search = model.search
    switch search.shown {
    case .idle:
      RecentsList()
    case .failed:
      VStack(spacing: 0) {
        topNotices
        Spacer(minLength: 0)
      }
    case .searching where search.entries.isEmpty:
      VStack(spacing: 0) {
        topNotices
        GridSkeleton()
      }
    case .searching, .results:
      // Not dimmed while the next answer is found: a dim on every key was the flicker.
      MediaGrid(sections: [GridSection(id: "results", title: nil, range: search.entries.indices)], entries: search.entries,
                context: "search", answering: "\(search.entries.count)|\(search.answered)") {
        topNotices
      }
    case .nothing:
      // Centred while it fits; at the largest text sizes it is taller than a small phone, and
      // then it scrolls rather than run under the search field and the tab bar.
      GeometryReader { space in
        ScrollView {
          VStack(spacing: 0) {
            topNotices
            NothingFound().frame(maxWidth: .infinity, maxHeight: .infinity)
          }
          .frame(minHeight: space.size.height)
        }
        .scrollBounceBehavior(.basedOnSize)
      }
    }
  }

  @ViewBuilder
  private var topNotices: some View {
    if model.search.isWarmingUp {
      ProgressLine(verb: "Getting search ready", fraction: nil)
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
    }
    if model.search.didFail {
      NoticeCard(message: FailureCopy.searchFailed, actionTitle: "Try again") { model.search.rerun() }
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
    }
    if model.search.capability == .unsupportedTower {
      NoticeCard(message: FailureCopy.unsupportedTower)
        .padding(.horizontal, Theme.inset)
        .padding(.vertical, 8)
    }
  }
}

/// Says what went wrong with this search, what to change, and what the catalogue cannot know yet.
private struct NothingFound: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    let search = model.search
    VStack(spacing: 12) {
      VStack(spacing: 4) {
        Text(search.answered.isEmpty ? "Nothing matches these filters." : "Nothing found for “\(search.answered)”.")
          .font(.subheadline.weight(.semibold))
          .foregroundStyle(Theme.title)
        Text(hint)
          .font(.footnote)
          .foregroundStyle(Theme.body)
      }
      .multilineTextAlignment(.center)
      .fixedSize(horizontal: false, vertical: true)
      if !search.filters.isEmpty {
        Button("Clear filters") { search.clearFilters() }
          .buttonStyle(SecondaryButtonStyle())
          .frame(maxWidth: 240)
      }
    }
    .frame(maxWidth: Theme.readableWidth)
    .padding(24)
  }

  private var hint: String {
    var parts: [String] = []
    if model.search.filters.isEmpty { parts.append("Try fewer words, or a person, place or year.") }
    if let catalogue = model.connection.catalogue {
      parts.append("This catalogue was updated \(catalogue.updatedAt.formatted(.relative(presentation: .named))), so anything newer won't show.")
    }
    return parts.joined(separator: " ")
  }
}

private struct FilterRow: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    let search = model.search
    let options = search.options
    let filters = search.filters
    ScrollView(.horizontal, showsIndicators: false) {
      HStack(spacing: 8) {
        if !options.people.isEmpty {
          menu("Person", symbol: "person", value: filters.person?.name, options: options.people.map(\.name)) { name in
            search.setFilters(changing(\.person, to: options.people.first { $0.name == name }))
          }
        }
        if !options.places.isEmpty {
          menu("Place", symbol: "mappin.and.ellipse", value: filters.place, options: options.places) { place in
            search.setFilters(changing(\.place, to: place))
          }
        }
        if !options.dates.isEmpty {
          menu("Date", symbol: "calendar", value: filters.date?.label, options: options.dates.map(\.label)) { label in
            search.setFilters(changing(\.date, to: options.dates.first { $0.label == label }))
          }
        }
        if !options.cameras.isEmpty {
          menu("Camera", symbol: "camera", value: filters.camera, options: options.cameras) { camera in
            search.setFilters(changing(\.camera, to: camera))
          }
        }
        menu("Kind", symbol: "photo.on.rectangle", value: kindName(filters.kind), options: ["Photos", "Clips"]) { name in
          search.setFilters(changing(\.kind, to: name.map { $0 == "Clips" ? MediaKind.clip : .photo }))
        }
      }
      .padding(.horizontal, Theme.inset)
    }
    .accessibilityIdentifier("scrolls-sideways")  // a test may let its last item sit past the edge
    .background(Theme.ground.ignoresSafeArea(edges: .top))
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: filters)
  }

  private func kindName(_ kind: MediaKind?) -> String? {
    kind.map { $0 == .clip ? "Clips" : "Photos" }
  }

  private func changing<Value>(_ path: WritableKeyPath<SearchFilters, Value>, to value: Value) -> SearchFilters {
    var new = model.search.filters
    new[keyPath: path] = value
    return new
  }

  private func menu(_ title: String, symbol: String, value: String?, options: [String],
                    choose: @escaping (String?) -> Void) -> some View {
    Menu {
      Button("Any \(title.lowercased())") { choose(nil) }
      Divider()
      ForEach(options, id: \.self) { option in
        Button { choose(option) } label: {
          if option == value { Label(option, systemImage: "checkmark") } else { Text(option) }
        }
      }
    } label: {
      ChipLabel(isOn: value != nil) {
        Image(systemName: symbol).font(.footnote).foregroundStyle(value == nil ? Theme.body : Theme.accent)
        Text(value ?? title).font(.subheadline).foregroundStyle(value == nil ? Theme.body : Theme.title).fixedSize()
        Image(systemName: "chevron.down").font(.caption2.weight(.semibold)).foregroundStyle(Theme.body)
      }
    }
    .accessibilityLabel(value.map { "\(title): \($0)" } ?? "\(title) filter")
  }
}

private struct RecentsList: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    let search = model.search
    List {
      if search.capability == .unsupportedTower {
        NoticeCard(message: FailureCopy.unsupportedTower)
          .listRowBackground(Color.clear)
          .listRowInsets(EdgeInsets(top: 8, leading: Theme.inset, bottom: 8, trailing: Theme.inset))
      }
      if !search.recents.isEmpty {
        Section {
          ForEach(search.recents, id: \.self) { query in
            Button {
              search.setText(query)
            } label: {
              Label {
                Text(query).foregroundStyle(Theme.title)
              } icon: {
                Image(systemName: "clock.arrow.circlepath").foregroundStyle(Theme.body)
              }
            }
            .listRowBackground(Theme.surface)
            .swipeActions {
              Button("Remove", role: .destructive) { search.forget(query) }
            }
          }
        } header: {
          HStack {
            Text("Recent").font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title)
            Spacer()
            Button("Clear") { search.forgetAll() }.buttonStyle(QuietButtonStyle())
          }
          .textCase(nil)
        }
      }
    }
    .scrollContentBackground(.hidden)
  }
}
