// SPDX-License-Identifier: GPL-2.0-or-later
// The Import window (plan/18 "The Import window"), SwiftUI, the Mac twin of
// ImportWindow.cs: sources, the day-grouped grid, the preset with "Where
// files go", one primary button; progress while copying, summary after.
// Keyboard-complete: Return imports (⌘Return from anywhere; Return on a tile
// opens it in the viewer), Space toggles a tile (pauses / resumes while
// copying), ⇧Space a day, ⌃Tab / ⌃⇧Tab the next / previous source, ⌘J ejects,
// Esc closes (the import carries on).
import AppKit
import SwiftUI

private let canvas = Color(red: 33 / 255, green: 35 / 255, blue: 42 / 255)
private let titleColor = Color(red: 220 / 255, green: 222 / 255, blue: 228 / 255)
private let bodyColor = Color(red: 150 / 255, green: 154 / 255, blue: 164 / 255)

struct ImportView: View {
  @ObservedObject var model: ImportModel
  @FocusState private var focusedTile: Int?
  @State private var presetName = ""
  @State private var useForCard = false
  @State private var autoImport = false
  @State private var showHistory = false

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
        List(model.history(), id: \.self) { Text($0) }
        Button("Close") { showHistory = false }.keyboardShortcut(.cancelAction)
      }
      .padding()
      .frame(width: 720, height: 480)
    }
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

  private var sourcesColumn: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("SOURCES").font(.caption).foregroundStyle(bodyColor)
      List(model.sources, selection: Binding(
        get: { model.selectedSource },
        set: { root in if let s = model.sources.first(where: { $0.root == root }) { model.load(s) } })) { s in
        VStack(alignment: .leading) {
          Text(s.label).foregroundStyle(titleColor)
          Text(s.detail).font(.caption).foregroundStyle(bodyColor)
        }
        .tag(s.root)
      }
      .scrollContentBackground(.hidden)
      Button("＋ Folder…") { model.addFolder() }
      Button("Imports…") { showHistory = true }
      Button("Verify a folder…") { model.verifyFolder() }
        .help("Re-hash imported files against the library index to find silent corruption.")
    }
    .padding(12)
  }

  private var gridColumn: some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(model.title).font(.title3).foregroundStyle(titleColor).padding(8)
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
            }
          }
        }
        .padding(8)
      }
      .disabled(model.copying)
    }
  }

  private func tileView(_ tile: ImportTile) -> some View {
    ZStack(alignment: .topLeading) {
      if let img = model.thumbs[tile.index] {
        Image(nsImage: img).resizable().scaledToFill()
      } else {
        Rectangle().fill(Color.white.opacity(0.05))
      }
      Text(tile.selected ? "☑" : "☐").font(.title3).padding(4)
      VStack {
        HStack { Spacer(); Text(tile.badge).font(.caption2).padding(4) }
        Spacer()
        Text(tile.name).font(.caption2).lineLimit(1).padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
    }
    .frame(width: 128, height: 128)
    .clipped()
    .opacity(tile.dimmed ? 0.4 : 1)
    .overlay(RoundedRectangle(cornerRadius: 2).stroke(focusedTile == tile.index ? Color.accentColor : .clear, lineWidth: 2))
    .focusable()
    .focused($focusedTile, equals: tile.index)
    .onTapGesture { model.toggle(tile) }
    .help(tile.tip)
    .onAppear { model.thumbnail(for: tile) }
  }

  private var presetColumn: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 8) {
        Text("PRESET").font(.caption).foregroundStyle(bodyColor)
        Picker("Preset", selection: Binding(get: { model.string("name", "Default") },
                                            set: { model.choosePreset($0) })) {
          ForEach(model.presetNames, id: \.self) { Text($0).tag($0) }
        }
        folderRow("To", key: "destination", allowOff: false)
        folderRow("Backup", key: "backup", allowOff: true)
        picker("Selection", "selection", [("new", "New since last import"), ("all", "All"),
                                          ("marked", "Marked in viewer"), ("date_range", "Date range")])
        if model.string("selection") == "date_range" {
          textField("From (YYYY-MM-DD)", "range_from")
          textField("To (YYYY-MM-DD)", "range_to")
        }
        typeFilter
        picker("Layout", "layout", [("YYYY/YYYY-MM-DD", "YYYY/YYYY-MM-DD"), ("YYYY/MM/DD", "YYYY/MM/DD"),
                                    ("YYYY-MM-DD", "YYYY-MM-DD"), ("card", "Keep card structure"), ("flat", "Flat")])
        toggle("+ camera model", "layout_camera", false)
        toggle("+ type folders (RAW / JPEG / Video)", "layout_type", false)
        picker("Date", "dates", [("taken", "Date taken, else file time"), ("file_time", "File time only")])
        textField("Rename ({date} {time} {camera} {seq} {original}), empty = off", "rename")
        toggle("Skip duplicates (by content)", "skip_duplicates", true)
        picker("Duplicate scope", "scope", [("destination", "This destination"), ("library", "The whole library index")])
        toggle("Full verify (read back from the drive)", "full_verify", true)
        toggle("Eject the card when done", "eject_after", true)
        toggle("Notify when done", "notify", true)
        toggle("Fast (does not wait for the viewer)", "fast", false)
        Text("Never offered: deleting from or formatting the card, overwriting a file, or any upload.")
          .font(.caption).foregroundStyle(bodyColor)
        Text("WHERE FILES GO").font(.caption).foregroundStyle(bodyColor)
        ForEach(model.folders) { f in
          HStack { Text(f.folder); Spacer(); Text("\(f.units)") }.font(.caption).foregroundStyle(titleColor)
        }
        TextField("Preset name", text: $presetName)
        Button("Save preset") { model.savePreset(named: presetName) }
        if model.hasCard {
          Toggle("Use this preset for this card", isOn: $useForCard)
            .onChange(of: useForCard) { model.bindCard(use: useForCard, auto: autoImport) }
          Toggle("Auto-import this card on insert (never deletes)", isOn: $autoImport)
            .onChange(of: autoImport) { model.bindCard(use: useForCard, auto: autoImport) }
        }
      }
      .padding(12)
    }
  }

  private func folderRow(_ label: String, key: String, allowOff: Bool) -> some View {
    VStack(alignment: .leading, spacing: 2) {
      let value = model.string(key)
      Text("\(label): \(value.isEmpty ? "Off" : value)").font(.caption).foregroundStyle(titleColor)
      HStack {
        Button("Choose…") { model.chooseFolder(key) }
        if allowOff && !value.isEmpty { Button("Off") { model.set(key, "") } }
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

  private func textField(_ label: String, _ key: String) -> some View {
    TextField(label, text: Binding(get: { model.string(key) }, set: { model.preset[key] = $0 }))
      .onSubmit { model.replan() }
  }

  private var typeFilter: some View {
    HStack {
      ForEach(["raw", "jpeg", "heic", "video", "other"], id: \.self) { t in
        let types = model.preset["types"] as? [String] ?? ["raw", "jpeg", "heic", "video", "other"]
        Toggle(t.uppercased(), isOn: Binding(
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
        Text(model.bottom).foregroundStyle(titleColor)
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
            Button("Eject (⌘J)") { model.eject() }
            Button("Open in viewer") { model.openDestination() }
            Button("Show report") { model.showReport() }.disabled(model.reportPath.isEmpty)
          }
        }
      }
      Spacer()
      Button("Import \(model.importCount)") { model.start() }
        .keyboardShortcut(.defaultAction)
        .disabled(model.importCount == 0 || model.copying)
    }
    .padding(12)
  }
}
