// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Import window (docs/design/18 "The Import window"), SwiftUI, the Mac twin of
// ImportWindow.cs: sources, the day-grouped grid, and three plain steps on the
// right (where to, what to import, how to organise) with the rest of the preset
// under More Options; one primary button; progress while copying, summary after.
// Keyboard-complete: Return imports (⌘Return from anywhere; Return on a tile
// opens it in the viewer), Space toggles a tile (pauses / resumes while
// copying), ⇧Space a day, ⌃Tab / ⌃⇧Tab the next / previous source, ⌘J ejects,
// Esc closes (the import carries on).
import AppKit
import SwiftUI

private let canvas = Color(nsColor: .windowBackgroundColor)
private let titleColor = Color(nsColor: .labelColor)
private let bodyColor = Color(nsColor: .secondaryLabelColor)

struct ImportView: View {
  @ObservedObject var model: ImportModel
  @FocusState private var focusedTile: Int?
  @State private var presetName = ""
  @State private var useForCard = false
  @State private var autoImport = false
  @State private var showHistory = false
  @State private var showMore = false

  var body: some View {
    VStack(spacing: 0) {
      if !model.banner.isEmpty {
        HStack {
          Text(model.banner).foregroundStyle(titleColor)
          Button("Resume") { model.resumeUnfinished() }
          Button("Dismiss") { model.banner = "" }
        }
        .padding(8)
      }
      HStack(alignment: .top, spacing: 0) {
        sourcesColumn.frame(width: 240)
        Divider()
        gridColumn
        Divider()
        presetColumn.frame(width: 320)
      }
      Divider()
      bottomBar
    }
    .background(canvas)
    .onKeyPress(keys: [.space, .return, .escape, .tab]) { press in handle(press) }
    .onKeyPress(characters: ["j"]) { press in
      guard press.modifiers.contains(.command) else { return .ignored }
      model.eject()
      return .handled
    }
    .sheet(isPresented: $showHistory) {
      VStack(alignment: .leading) {
        Text("Imports").font(.title2)
        List(model.historyRows, id: \.self) { Text($0) }
        Button("Close") { showHistory = false }.keyboardShortcut(.cancelAction)
      }
      .padding()
      .frame(width: 720, height: 480)
      .onAppear { model.loadHistory() }
    }
    .sheet(isPresented: Binding(
      get: { model.explainerSection != nil },
      set: { if !$0 { model.explainerSection = nil } })) {
      explainerSheet
    }
    .sheet(isPresented: $model.confirmingImport) { importConfirmationSheet }
    .background(DuplicatesSheetHost(model: model.duplicates))
  }

  // MARK: first-use explainer (issue #41)

  private var explainerSheet: some View {
    ScrollViewReader { proxy in
      VStack(alignment: .leading, spacing: 0) {
        Text("About Import").font(.title2).padding([.top, .horizontal])
        ScrollView {
          VStack(alignment: .leading, spacing: 18) {
            ForEach(ExplainerSection.allCases) { section in
              VStack(alignment: .leading, spacing: 4) {
                Text(section.title).font(.headline)
                Text(section.body).font(.callout).foregroundStyle(bodyColor)
              }
              .id(section.id)
            }
          }
          .padding()
        }
        Button("Close") { model.explainerSection = nil }
          .keyboardShortcut(.cancelAction)
          .padding([.bottom, .horizontal])
      }
      .frame(width: 460, height: 520)
      .onAppear {
        if let target = model.explainerSection { proxy.scrollTo(target, anchor: .top) }
      }
    }
  }

  // MARK: pre-copy confirmation (issue #41)

  private var confirmationCounts: String {
    let items: String = model.importCount == 1 ? "item" : "items"
    let files: String = model.confirmFiles == 1 ? "file" : "files"
    let bytes: String = ByteCountFormatter.string(fromByteCount: model.confirmBytes, countStyle: .file)
    return "\(model.importCount) \(items) (\(model.confirmFiles) \(files), \(bytes))"
  }

  private var importConfirmationSheet: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Import these files?").font(.title3)
      Text(confirmationCounts)
      Text("From: \(model.selectedSource)").font(.caption).foregroundStyle(bodyColor)
      Text("To: \(model.destinationPreview.isEmpty ? "(not set)" : model.destinationPreview)")
        .font(.caption).foregroundStyle(bodyColor)
      Text("Originals are never modified or deleted; every copy is verified.")
        .font(.caption).foregroundStyle(bodyColor)
      HStack {
        Spacer()
        Button("Cancel") { model.confirmingImport = false }.keyboardShortcut(.cancelAction)
        Button("Import") { model.startConfirmed() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding()
    .frame(width: 380)
  }

  private func handle(_ press: KeyPress) -> KeyPress.Result {
    switch press.key {
    case .return:
      if press.modifiers.contains(.command) || focusedTile == nil {
        model.start()
      } else if let i = focusedTile, let tile = tile(i) {
        model.open(tile)  // cull before copying
      }
      return .handled
    case .space:
      if model.copying { model.pauseResume(); return .handled }
      guard let i = focusedTile, let tile = tile(i) else { return .ignored }
      if press.modifiers.contains(.shift) {
        if let day = model.days.first(where: { $0.tiles.contains(tile) }) { model.toggleDay(day.day) }
      } else {
        model.toggle(tile)
      }
      return .handled
    case .tab where press.modifiers.contains(.control):
      model.nextSource(press.modifiers.contains(.shift) ? -1 : 1)
      return .handled
    case .escape:
      NSApp.keyWindow?.close()  // the job keeps running
      return .handled
    default:
      return .ignored
    }
  }

  private func tile(_ index: Int) -> ImportTile? {
    for d in model.days { if let t = d.tiles.first(where: { $0.index == index }) { return t } }
    return nil
  }

  // MARK: columns

  /// Left: where the files come from, then the occasional tools in one menu.
  private var sourcesColumn: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Import from").font(.headline).foregroundStyle(titleColor)
      if model.sources.isEmpty {
        Text("Insert a memory card or a drive, or add a folder.")
          .font(.callout).foregroundStyle(bodyColor)
          .fixedSize(horizontal: false, vertical: true)
        Spacer()
      } else {
        List(model.sources, selection: Binding(
          get: { model.selectedSource },
          set: { root in if let s = model.sources.first(where: { $0.root == root }) { model.load(s) } })) { s in
          HStack(spacing: 8) {
            Image(systemName: s.removable ? "sdcard" : "folder").foregroundStyle(bodyColor)
            VStack(alignment: .leading) {
              Text(s.label).foregroundStyle(titleColor).lineLimit(1).truncationMode(.middle)
              if !s.detail.isEmpty { Text(s.detail).font(.caption).foregroundStyle(bodyColor) }
            }
          }
          .tag(s.root)
        }
        .scrollContentBackground(.hidden)
      }
      Button { model.addFolder() } label: { Label("Add a folder…", systemImage: "plus") }
      Divider()
      Menu {
        Button("Past imports…") { showHistory = true }
        Button("Check a folder for damaged files…") { model.verifyFolder() }
        Button("Find duplicates…") { model.duplicates.choose() }
        Divider()
        Button("About Import") { model.showExplainer() }
      } label: {
        Label("Tools", systemImage: "wrench.and.screwdriver")
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
    }
    .padding(12)
  }

  /// Centre: the files, by day, or a sentence saying why there are none.
  private var gridColumn: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack {
        Text(model.title).font(.title3).foregroundStyle(titleColor).lineLimit(1)
        if model.planUnits > 0 {
          Text("\(model.planNew) new of \(model.planUnits)").foregroundStyle(bodyColor)
        }
        Spacer()
        if !model.days.isEmpty && !model.copying {
          Button("Select All") { model.selectAll(true) }.controlSize(.small)
          Button("Select None") { model.selectAll(false) }.controlSize(.small)
        }
      }
      .padding(8)
      if model.days.isEmpty {
        emptyGrid.frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        if model.planNew == 0 && model.string("selection", "new") == "new" && !model.copying {
          HStack(spacing: 8) {
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            Text("Everything here has already been imported. Click a file to import it again.")
              .foregroundStyle(titleColor)
            Spacer()
          }
          .padding(8)
          .background(RoundedRectangle(cornerRadius: 6).fill(Color.green.opacity(0.1)))
          .padding(.horizontal, 8)
        }
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 8, pinnedViews: [.sectionHeaders]) {
            ForEach(model.days) { day in
              Section {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 128, maximum: 128), spacing: 6)], spacing: 6) {
                  ForEach(day.tiles) { tile in tileView(tile) }
                }
              } header: {
                Button(day.header) { model.toggleDay(day.day) }
                  .buttonStyle(.plain)
                  .foregroundStyle(titleColor)
                  .padding(.vertical, 4)
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .background(canvas)
                  .help("Click to select or clear the whole day")
              }
            }
          }
          .padding(8)
        }
        .disabled(model.copying)
      }
    }
  }

  @ViewBuilder private var emptyGrid: some View {
    VStack(spacing: 10) {
      if model.selectedSource.isEmpty {
        Image(systemName: "sdcard").font(.system(size: 40)).foregroundStyle(bodyColor)
        Text("Insert a memory card, or choose a folder to import from.").foregroundStyle(titleColor)
        Button("Choose a Folder…") { model.addFolder() }
      } else if model.scanState == "reading" {
        ProgressView()
        Text("Looking for photos and videos…").foregroundStyle(bodyColor)
      } else if model.scanState == "failed" {
        Image(systemName: "exclamationmark.triangle").font(.system(size: 32)).foregroundStyle(.orange)
        Text("This source could not be read. Check that it is still connected.").foregroundStyle(titleColor)
      } else if model.planUnits > 0 && model.string("selection", "new") == "new" {
        Image(systemName: "checkmark.circle").font(.system(size: 40)).foregroundStyle(.green)
        Text("Everything here has already been imported.").foregroundStyle(titleColor)
        Button("Show All \(model.planUnits) Files") { model.set("selection", "all") }
      } else {
        Image(systemName: "photo.on.rectangle").font(.system(size: 40)).foregroundStyle(bodyColor)
        Text("No photos or videos match these settings.").foregroundStyle(titleColor)
      }
    }
    .multilineTextAlignment(.center)
    .padding()
  }

  private func tileView(_ tile: ImportTile) -> some View {
    ZStack(alignment: .topLeading) {
      if let img = model.thumbs[tile.index] {
        Image(nsImage: img).resizable().scaledToFill()
      } else {
        Rectangle().fill(Color.primary.opacity(0.05))
      }
      Image(systemName: tile.selected ? "checkmark.circle.fill" : "circle")
        .font(.title3)
        .foregroundStyle(tile.selected ? Color.accentColor : Color.white)
        .shadow(radius: 1)
        .padding(5)
      VStack {
        HStack { Spacer(); Text(tile.badge).font(.caption2).padding(4) }
        Spacer()
        Text(tile.state == "duplicate" || tile.state == "imported" ? "Already imported" : tile.name)
          .font(.caption2).lineLimit(1).padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
          .background(Color.black.opacity(0.35))
          .foregroundStyle(.white)
      }
    }
    .frame(width: 128, height: 128)
    .clipShape(RoundedRectangle(cornerRadius: 4))
    .opacity(tile.dimmed ? 0.45 : 1)
    .overlay(RoundedRectangle(cornerRadius: 4)
      .stroke(focusedTile == tile.index ? Color.accentColor : tile.selected ? Color.accentColor.opacity(0.6) : .clear,
              lineWidth: 2))
    .focusable()
    .focused($focusedTile, equals: tile.index)
    .onTapGesture { model.toggle(tile) }
    .help(tile.tip.isEmpty ? tile.name : tile.tip)
    .onAppear { model.thumbnail(for: tile) }
  }

  /// Right: three plain questions (where, what, how organised), the backup
  /// and eject switches, and everything else under More Options.
  private var presetColumn: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
        if model.presetNames.count > 1 {
          Picker("Saved settings", selection: Binding(get: { model.string("name", "Default") },
                                                      set: { model.choosePreset($0) })) {
            ForEach(model.presetNames, id: \.self) { Text($0).tag($0) }
          }
        }
        step("Where to") { destinationRow }
        step("What to import") {
          Picker("What to import", selection: Binding(get: { model.string("selection", "new") },
                                                      set: { model.set("selection", $0) })) {
            Text("New").tag("new")
            Text("All").tag("all")
            Text("Marked").tag("marked")
            Text("Dates").tag("date_range")
          }
          .pickerStyle(.segmented)
          .labelsHidden()
          Text(selectionHint).font(.caption).foregroundStyle(bodyColor)
            .fixedSize(horizontal: false, vertical: true)
          if model.string("selection") == "date_range" {
            HStack {
              textField("From YYYY-MM-DD", "range_from")
              textField("To YYYY-MM-DD", "range_to")
            }
          }
        }
        step("How to organise") {
          Picker("How to organise", selection: Binding(get: { model.string("layout", "YYYY/YYYY-MM-DD") },
                                                       set: { model.set("layout", $0) })) {
            Text("Year, then day").tag("YYYY/YYYY-MM-DD")
            Text("Year, month, then day").tag("YYYY/MM/DD")
            Text("A folder per day").tag("YYYY-MM-DD")
            Text("Same folders as the card").tag("card")
            Text("All in one folder").tag("flat")
          }
          .labelsHidden()
          if !model.folders.isEmpty {
            VStack(alignment: .leading, spacing: 2) {
              ForEach(model.folders.prefix(4)) { f in
                HStack {
                  Image(systemName: "folder").font(.caption)
                  Text(f.folder).lineLimit(1).truncationMode(.head)
                  Spacer()
                  Text("\(f.units)")
                }
                .font(.caption).foregroundStyle(bodyColor)
              }
              if model.folders.count > 4 {
                Text("and \(model.folders.count - 4) more folders").font(.caption).foregroundStyle(bodyColor)
              }
            }
            .help("Where the selected files will go. Nothing is copied until you click Import.")
          }
        }
        VStack(alignment: .leading, spacing: 6) {
          Toggle("Also copy to a backup drive", isOn: Binding(
            get: { !model.string("backup").isEmpty },
            set: { on in if on { model.chooseFolder("backup") } else { model.set("backup", "") } }))
          if !model.string("backup").isEmpty {
            Text(model.string("backup")).font(.caption).foregroundStyle(bodyColor)
              .lineLimit(1).truncationMode(.middle)
          }
          // Eject only ever means something for a card, USB or network drive
          // (issue #41/#42): an ordinary folder has nothing to eject.
          if model.removable {
            toggle("Eject the card when done", "eject_after", true)
          }
        }
        DisclosureGroup("More options", isExpanded: $showMore) { moreOptions.padding(.top, 6) }
        Text("Import only copies. Nothing on the card is deleted, changed or overwritten, and every copy is checked.")
          .font(.caption).foregroundStyle(bodyColor)
          .fixedSize(horizontal: false, vertical: true)
      }
      .padding(12)
    }
  }

  private var selectionHint: String {
    switch model.string("selection", "new") {
    case "all": return "Everything on the source, even what you imported before."
    case "marked": return "Only what you marked in the viewer."
    case "date_range": return "Only what was taken between two dates."
    default: return "Only what has not been imported from here before."
    }
  }

  private func step<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title).font(.headline).foregroundStyle(titleColor)
      content()
    }
  }

  @ViewBuilder private var destinationRow: some View {
    let dest = model.string("destination")
    if dest.isEmpty {
      Button { model.chooseFolder("destination") } label: {
        Label("Choose a Folder…", systemImage: "folder.badge.plus").frame(maxWidth: .infinity)
      }
      .controlSize(.large)
    } else {
      HStack(spacing: 8) {
        Image(systemName: "folder.fill").foregroundStyle(Color.accentColor)
        VStack(alignment: .leading, spacing: 0) {
          Text((dest as NSString).lastPathComponent).foregroundStyle(titleColor).lineLimit(1)
          Text((dest as NSString).abbreviatingWithTildeInPath).font(.caption).foregroundStyle(bodyColor)
            .lineLimit(1).truncationMode(.middle)
        }
        Spacer()
        Button("Change…") { model.chooseFolder("destination") }.controlSize(.small)
      }
      .help(dest)
    }
  }

  /// Everything a first import does not need to touch.
  private var moreOptions: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("File types").font(.caption).foregroundStyle(bodyColor)
      typeFilter
      toggle("Add a folder per camera", "layout_camera", false)
      toggle("Add RAW / JPEG / Video folders", "layout_type", false)
      picker("Date from", "dates", [("taken", "When it was taken"), ("file_time", "The file's date")])
      textField("Rename files, e.g. {date}_{seq} (empty keeps names)", "rename")
      toggle("Skip files already in the library", "skip_duplicates", true)
      if model.bool("skip_duplicates", true) {
        picker("Look for them in", "scope", [("destination", "This folder"), ("library", "The whole library")])
      }
      toggle("Read every copy back to check it", "full_verify", true)
      toggle("Notify me when done", "notify", true)
      toggle("Copy at full speed (the viewer may lag)", "fast", false)
      helpLink("How duplicates, verifying and the folder preview work", .filters)
      Divider()
      Text("Saved settings").font(.caption).foregroundStyle(bodyColor)
      HStack {
        TextField("Name", text: $presetName)
        Button("Save") { model.savePreset(named: presetName) }
      }
      if model.hasCard {
        Toggle("Always use these settings for this card", isOn: $useForCard)
          .onChange(of: useForCard) { model.bindCard(use: useForCard, auto: autoImport) }
        Toggle("Import this card as soon as it is inserted", isOn: $autoImport)
          .onChange(of: autoImport) { model.bindCard(use: useForCard, auto: autoImport) }
          .disabled(!useForCard)
      }
    }
  }

  private func picker(_ label: String, _ key: String, _ options: [(String, String)]) -> some View {
    Picker(label, selection: Binding(get: { model.string(key, options[0].0) }, set: { model.set(key, $0) })) {
      ForEach(Array(options.enumerated()), id: \.offset) { Text($0.element.1).tag($0.element.0) }
    }
  }

  private func toggle(_ label: String, _ key: String, _ fallback: Bool) -> some View {
    Toggle(label, isOn: Binding(get: { model.bool(key, fallback) }, set: { model.set(key, $0) }))
  }

  /// A small, contextual "?" a person can invoke any time, not just first-run
  /// (issue #41), jumping straight to the relevant explainer section.
  private func helpLink(_ label: String, _ section: ExplainerSection) -> some View {
    Button(label) { model.showExplainer(section) }
      .buttonStyle(.link)
      .font(.caption)
  }

  private func textField(_ label: String, _ key: String) -> some View {
    TextField(label, text: Binding(get: { model.string(key) }, set: { model.preset[key] = $0 }))
      .onSubmit { model.replan() }
  }

  private var typeFilter: some View {
    HStack {
      ForEach([("raw", "RAW"), ("jpeg", "JPEG"), ("heic", "HEIC"), ("video", "Video"), ("other", "Other")], id: \.0) { t, label in
        let types = model.preset["types"] as? [String] ?? ["raw", "jpeg", "heic", "video", "other"]
        Toggle(label, isOn: Binding(
          get: { types.contains(t) },
          set: { on in
            var next = types.filter { $0 != t }
            if on { next.append(t) }
            model.set("types", next)
          }))
        .toggleStyle(.checkbox)
      }
    }
  }

  // MARK: bottom

  private var bottomBar: some View {
    HStack(alignment: .top) {
      VStack(alignment: .leading, spacing: 6) {
        HStack {
          Text(model.bottom).foregroundStyle(titleColor)
          if let section = model.ejectFailureSection, let s = ExplainerSection(rawValue: section) {
            Button("Why?") { model.showExplainer(s) }.buttonStyle(.link).font(.caption)
          }
        }
        if !model.copying && model.summaryTitle.isEmpty && !model.bottomDetail.isEmpty {
          Text(model.bottomDetail).font(.caption).foregroundStyle(bodyColor)
        }
        if model.copying {
          ForEach(Array(model.progress.enumerated()), id: \.offset) { ProgressView(value: $0.element).frame(width: 520) }
          Text(model.progressLine).font(.caption).foregroundStyle(bodyColor)
          HStack {
            Button(model.paused ? "Resume (Space)" : "Pause (Space)") { model.pauseResume() }
            Button("Cancel") { model.cancel() }
          }
        } else if !model.summaryTitle.isEmpty {
          Text(model.summaryTitle).foregroundStyle(titleColor)
          List(model.summary, id: \.self) { Text($0).font(.caption) }.frame(height: 120)
          HStack {
            if model.canRetry { Button("Retry failed") { model.retryFailed() } }
            if model.summaryOffersEject { Button("Eject (⌘J)") { model.eject() } }
            Button("Open in viewer") { model.openDestination() }
            Button("Show report") { model.showReport() }.disabled(model.reportPath.isEmpty)
          }
        }
      }
      Spacer()
      if model.string("destination").isEmpty {
        Button("Choose Where to Import…") { model.chooseFolder("destination") }
          .keyboardShortcut(.defaultAction)
          .buttonStyle(.borderedProminent)
          .controlSize(.large)
          .disabled(model.copying)
      } else {
        Button("Import \(model.importCount)") { model.start() }
          .keyboardShortcut(.defaultAction)
          .buttonStyle(.borderedProminent)
          .controlSize(.large)
          .disabled(model.importCount == 0 || model.copying)
      }
    }
    .padding(12)
  }
}

/// Observes the duplicates model itself, so its sheet opens and updates
/// without re-rendering the whole Import window on every progress tick.
private struct DuplicatesSheetHost: View {
  @ObservedObject var model: DuplicatesModel

  var body: some View {
    Color.clear.sheet(isPresented: $model.showing) { DuplicatesView(model: model) }
  }
}
