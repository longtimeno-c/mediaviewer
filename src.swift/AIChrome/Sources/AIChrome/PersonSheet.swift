// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A person's faces, and the faces filed under nobody (docs/design/17 "Reviewing
// a person"): "Check these" first with why, a pin on the user's own, Yes / Not /
// Move to… / Unpin on the selection, where rejected faces went with Undo (⌘Z),
// "Might also be" at the bottom, and the whole picture with the face outlined
// (Space). Sheet keys Y, N, M, Space stay in the sheet (docs/design/16 has no
// rows for them). Every table call runs detached through the model (rule 1);
// nothing about a face or a file is logged (rule 6).
import AppKit
import SwiftUI

struct PersonSheet: View {
  @ObservedObject var model: ManagementModel
  let person: Person
  let done: () -> Void
  @State private var faces: [ReviewFace] = []
  /// person_review_json answered: doubt, pins and quality to show.
  @State private var detailed = false
  @State private var selection = Set<UInt64>()
  @State private var loading = true
  @State private var refining = false
  @State private var refineNote = ""
  @State private var sort = ReviewSort.doubtful
  /// The face the arrow keys move from (the last one clicked or reached).
  @State private var cursor: UInt64?
  /// The last action's line ("2 now with Carl, 1 unknown") and its undo.
  @State private var status = ""
  @State private var lastUndo: PeopleUndo?
  @State private var busy = false
  @State private var suggestions = PersonSuggestions()
  @State private var picking = false
  @State private var preview: PreviewFace?
  @FocusState private var focused: Bool

  /// The person as the grid has it now (a rename, a cover that moved).
  private var current: Person { model.people.first { $0.id == person.id } ?? person }
  private var name: String { current.name }
  private var groups: (check: [ReviewFace], rest: [ReviewFace]) { sort.groups(faces) }
  /// The order on screen, for the arrow keys.
  private var ordered: [ReviewFace] { let g = groups; return g.check + g.rest }
  private var selected: [ReviewFace] { faces.filter { selection.contains($0.id) } }
  private var yesLabel: String { name.isEmpty ? "Yes, same person" : "Yes, this is \(name)" }
  private var notLabel: String { name.isEmpty ? "Not this person" : "Not \(name)" }
  private var allPinned: Bool { !selected.isEmpty && selected.allSatisfy(\.pinned) }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      header
      Text(detailed
           ? "Check the faces that may be someone else. ⌘-click selects several, ← → move, ⇧ adds. Y yes, N not, M move, Space shows the photo."
           : "Select faces that are someone else. ⌘-click selects several, ← → move, ⇧ adds; Delete removes them from this person.")
        .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      grid
      if !status.isEmpty { statusLine }
      actions
      if !suggestions.isEmpty { suggestionStrip }
    }
    .padding(20)
    .frame(width: 680, height: suggestions.isEmpty ? 560 : 680)
    .onExitCommand(perform: done)  // Esc closes, as Done does (issue #192)
    .background {
      // ⌘Z: the last action, put back. Disabled with none (Edit ▸ Undo then has it).
      Button("Undo") { undoLast() }
        .keyboardShortcut("z", modifiers: .command)
        .disabled(lastUndo == nil || busy)
        // Not .hidden(): a hidden button may drop its shortcut.
        .opacity(0).frame(width: 0, height: 0)
        .accessibilityHidden(true)
    }
    .sheet(item: $preview) { p in
      PhotoPreview(table: model.table, face: p) { preview = nil }
    }
    .task {
      await reload()
      focused = true
      suggestions = await model.suggestions(for: person.id)
    }
  }

  // ---- parts ----

  private var header: some View {
    HStack {
      Text(name.isEmpty ? "Unnamed person" : name)
        .font(AITheme.font(18)).foregroundStyle(AITheme.title)
      Spacer()
      if detailed && faces.contains(where: { $0.quality >= 0 }) {
        Picker("Order", selection: $sort) {
          ForEach(ReviewSort.allCases) { Text($0.label).tag($0) }
        }
        .pickerStyle(.segmented).labelsHidden().fixedSize()
        .help("Doubtful first, or the clearest faces first")
        .accessibilityLabel("Order of faces")
      }
      Button("Show photos") { done(); model.showPhotos(of: current) }
      Menu("Merge into…") {
        ForEach(model.mergeTargets(for: current)) { other in
          Button(model.displayName(other)) {
            model.merge(into: other.id, from: person.id)
            done()
          }
        }
      }
      .fixedSize()
      .accessibilityLabel("Merge into another person")
    }
  }

  private var grid: some View {
    ScrollViewReader { proxy in
      ScrollView {
        if loading {
          ProgressView().frame(maxWidth: .infinity).padding(40)
        }
        let g = groups
        VStack(alignment: .leading, spacing: 10) {
          if !g.check.isEmpty {
            Text("Check these (\(g.check.count))")
              .font(AITheme.font(13)).foregroundStyle(AITheme.title)
              .accessibilityAddTraits(.isHeader)
            tiles(g.check, captions: true)
            if !g.rest.isEmpty {
              Divider().padding(.vertical, 2)
            }
          }
          tiles(g.rest, captions: false)
        }
        .padding(4)
      }
      .frame(minHeight: 260)
      .focusable()
      .focused($focused)
      .focusEffectDisabled()
      .onKeyPress(keys: [.delete, .deleteForward]) { _ in
        guard !selection.isEmpty else { return .ignored }
        reject()
        return .handled
      }
      // Selection from the keyboard (issue #192).
      .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
        let list = ordered
        guard !list.isEmpty else { return .ignored }
        let id = step(list.map(\.id), from: cursor, by: press.key == .leftArrow ? -1 : 1)
        cursor = id
        if press.modifiers.contains(.shift) { selection.insert(id) } else { selection = [id] }
        proxy.scrollTo(id)
        return .handled
      }
      .onKeyPress(.space) {
        guard let f = focusedFace else { return .ignored }
        preview = PreviewFace(f)
        return .handled
      }
      .onKeyPress(characters: CharacterSet(charactersIn: "ynmYNM")) { press in
        guard !press.modifiers.contains(.command), !selection.isEmpty, !busy else { return .ignored }
        switch press.characters.lowercased() {
        case "y" where model.canPin: yes()
        case "n": reject()
        case "m" where model.canAssign: picking = true
        default: return .ignored
        }
        return .handled
      }
    }
  }

  /// The face Space shows: the cursor's, else the only one selected.
  private var focusedFace: ReviewFace? {
    if let cursor, selection.contains(cursor), let f = faces.first(where: { $0.id == cursor }) { return f }
    return selected.count == 1 ? selected.first : nil
  }

  private func tiles(_ list: [ReviewFace], captions: Bool) -> some View {
    LazyVGrid(columns: [GridItem(.adaptive(minimum: captions ? 96 : 76, maximum: captions ? 120 : 92), spacing: 10)],
              alignment: .leading, spacing: 10) {
      ForEach(list) { face in
        ReviewTile(table: model.table, face: face, caption: captions ? face.caption : nil,
                   selected: selection.contains(face.id), notLabel: notLabel,
                   toggle: { toggle(face.id) },
                   reject: { reject([face.id]) },
                   show: { preview = PreviewFace(face) })
          .id(face.id)
          .onTapGesture {
            if NSEvent.modifierFlags.contains(.command) || NSEvent.modifierFlags.contains(.shift) {
              toggle(face.id)
            } else {
              selection = [face.id]
              cursor = face.id
            }
            focused = true  // Y, N, M and Space act on what was clicked
          }
          .contextMenu {
            Button("Show photo") { preview = PreviewFace(face) }
            if model.canPin && !face.pinned { Button(yesLabel) { yes([face.id]) } }
            Button(notLabel) { reject([face.id]) }
            if model.canAssign {
              Button("Move to…") {
                selection = [face.id]
                picking = true
              }
            }
            if model.canPin && face.pinned { Button("Unpin") { unpin([face.id]) } }
          }
      }
    }
  }

  private var statusLine: some View {
    HStack(spacing: 10) {
      Text(status).font(AITheme.font(12)).foregroundStyle(AITheme.title)
      if lastUndo != nil {
        Button("Undo") { undoLast() }
          .disabled(busy)
          .help("Put it back (⌘Z)")
          .accessibilityLabel("Undo")
      }
    }
    .accessibilityElement(children: .contain)
  }

  private var actions: some View {
    HStack {
      if model.canPin {
        if allPinned {
          Button("Unpin") { unpin() }
            .disabled(busy)
            .help("Let the faces be checked again like any other")
        } else {
          Button(yesLabel) { yes() }
            .disabled(selection.isEmpty || busy)
            .help("Keep these faces with this person (Y)")
            .accessibilityLabel(yesLabel)
        }
      }
      Button(notLabel) { reject() }
        .disabled(selection.isEmpty || busy)
        .help("Remove these faces from this person (N)")
        .accessibilityLabel(notLabel)
      if model.canAssign {
        Button("Move to…") { picking = true }
          .disabled(selection.isEmpty || busy)
          .help("Move these faces to someone else (M)")
          .accessibilityLabel("Move to another person")
          .popover(isPresented: $picking, arrowEdge: .top) {
            PersonPicker(model: model, excluding: person.id) { target in
              picking = false
              move(to: target)
              focused = true
            }
          }
      }
      Button("Split into new person") {
        model.split(Array(selection))
        faces.removeAll { selection.contains($0.id) }
        selection = []
        lastUndo = nil
        status = ""
      }
      .disabled(selection.isEmpty || busy)
      if model.canRefine {
        Button("Refine faces") { refine() }
          .disabled(refining || loading)
          .help("Check every face against this person and move out the ones that don't match")
        if refining {
          ProgressView().controlSize(.small)
        } else if !refineNote.isEmpty {
          Text(refineNote).font(AITheme.font(12)).foregroundStyle(AITheme.body)
        }
      }
      Spacer()
      Button("Done", action: done).keyboardShortcut(.defaultAction)
    }
  }

  private var suggestionStrip: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(name.isEmpty ? "Might also be this person" : "Might also be \(name)")
        .font(AITheme.font(13)).foregroundStyle(AITheme.title)
        .accessibilityAddTraits(.isHeader)
      ScrollView(.horizontal) {
        HStack(alignment: .top, spacing: 12) {
          ForEach(suggestions.people) { p in
            VStack(spacing: 4) {
              FaceImage(table: model.table, faceID: p.coverFace, box: p.coverBox, monogram: "")
                .frame(width: 60, height: 60)
                .accessibilityHidden(true)
              Text(p.faces == 1 ? "1 photo" : "\(p.faces) photos")
                .font(AITheme.font(11)).foregroundStyle(AITheme.body)
              Button("Merge") { mergeSuggested(p) }
                .controlSize(.small).disabled(busy)
                .help("These are the same person: merge them")
                .accessibilityLabel("Merge an unnamed person with \(p.faces) photos")
            }
            .accessibilityElement(children: .contain)
          }
          ForEach(suggestions.faces) { f in
            VStack(spacing: 4) {
              FaceImage(table: model.table, faceID: f.id, box: f.box, monogram: "")
                .frame(width: 60, height: 60)
                .onTapGesture(count: 2) {
                  preview = PreviewFace(id: f.id, path: f.path, ptsMs: f.ptsMs, box: f.box)
                }
                .contextMenu {
                  Button("Show photo") { preview = PreviewFace(id: f.id, path: f.path, ptsMs: f.ptsMs, box: f.box) }
                }
                .accessibilityLabel(faceAccessibilityLabel(path: f.path, ptsMs: f.ptsMs))
              Button("Yes") { acceptSuggested(f) }
                .controlSize(.small).disabled(busy || !model.canAssign)
                .help(name.isEmpty ? "This is the same person" : "This is \(name)")
                .accessibilityLabel(name.isEmpty ? "Yes, same person" : "Yes, this is \(name)")
            }
          }
        }
        .padding(.bottom, 4)
      }
      .frame(height: 104)
    }
  }

  // ---- actions ----

  private func toggle(_ id: UInt64) {
    if !selection.insert(id).inserted { selection.remove(id) }
    cursor = id
  }

  private func reload() async {
    let r = await model.review(of: person.id)
    withAnimation(.easeOut(duration: 0.18)) {
      faces = r.faces
      detailed = r.detailed
    }
    selection.formIntersection(Set(r.faces.map(\.id)))
    loading = false
  }

  private func say(_ text: String, undo: PeopleUndo?) {
    status = text
    lastUndo = undo
  }

  private func yes(_ ids: [UInt64]? = nil) {
    let picked = ids ?? Array(selection)
    guard !picked.isEmpty, model.canPin, !busy else { return }
    // Undo unpins only the faces this pinned.
    let newly = faces.filter { picked.contains($0.id) && !$0.pinned }.map(\.id)
    busy = true
    Task {
      let ok = await model.pin(picked, true)
      busy = false
      if ok {
        say(picked.count == 1 ? "Kept 1 face" : "Kept \(picked.count) faces",
            undo: newly.isEmpty ? nil : .pin(newly, false))
        await reload()
      } else {
        say("Couldn't keep the faces. Try again.", undo: nil)
      }
    }
  }

  private func unpin(_ ids: [UInt64]? = nil) {
    let picked = ids ?? Array(selection)
    guard !picked.isEmpty, model.canPin, !busy else { return }
    busy = true
    Task {
      let ok = await model.pin(picked, false)
      busy = false
      if ok {
        say(picked.count == 1 ? "Unpinned 1 face" : "Unpinned \(picked.count) faces", undo: .pin(picked, true))
        await reload()
      } else {
        say("Couldn't unpin the faces. Try again.", undo: nil)
      }
    }
  }

  private func reject(_ ids: [UInt64]? = nil) {
    let picked = ids ?? Array(selection)
    guard !picked.isEmpty, !busy else { return }
    let removed = faces.filter { picked.contains($0.id) }
    withAnimation(.easeOut(duration: 0.18)) {
      faces.removeAll { picked.contains($0.id) }
      selection.subtract(picked)
    }
    busy = true
    let reporting = model.canRejectJSON
    Task {
      let outcome = await model.rejectReporting(picked)
      busy = false
      if let outcome {
        say(outcome.summary, undo: model.canAssign ? .restoring(removed, to: person.id) : nil)
      } else if reporting {
        say("Couldn't remove the faces. Try again.", undo: nil)
      } else {
        say("", undo: nil)  // an older pack says nothing of where they went
      }
      model.reloadPeople()
      await reload()
    }
  }

  private func move(to target: Person) {
    let picked = Array(selection)
    guard !picked.isEmpty, model.canAssign, !busy else { return }
    let moved = faces.filter { picked.contains($0.id) }
    withAnimation(.easeOut(duration: 0.18)) {
      faces.removeAll { picked.contains($0.id) }
      selection = []
    }
    busy = true
    Task {
      let ok = await model.assign(picked, to: target.id, pin: true)
      busy = false
      if ok {
        say(movedSummary(picked.count, to: target.name), undo: .restoring(moved, to: person.id))
      } else {
        say("Couldn't move the faces. Try again.", undo: nil)
      }
      model.reloadPeople()
      await reload()
    }
  }

  private func mergeSuggested(_ p: PersonSuggestions.SuggestedPerson) {
    guard !busy else { return }
    busy = true
    Task {
      let token = await model.mergeCovers(into: person.id, from: p.id,
                                          coverInto: current.coverFace, coverFrom: p.coverFace)
      busy = false
      switch token {
      case nil: say("Couldn't merge them. Try again.", undo: nil)
      case 0?: say("Merged", undo: nil)
      case let t?: say("Merged", undo: model.table.canMergeUndo ? .merge([t]) : nil)
      }
      model.reloadPeople()
      await reload()
      suggestions = await model.suggestions(for: person.id)
    }
  }

  private func acceptSuggested(_ f: PersonSuggestions.SuggestedFace) {
    guard !busy, model.canAssign else { return }
    busy = true
    Task {
      let ok = await model.assign([f.id], to: person.id, pin: true)
      busy = false
      if ok {
        suggestions.faces.removeAll { $0.id == f.id }
        say("Added 1 face", undo: nil)
      } else {
        say("Couldn't add the face. Try again.", undo: nil)
      }
      model.reloadPeople()
      await reload()
    }
  }

  private func undoLast() {
    guard let action = lastUndo, !busy else { return }
    lastUndo = nil
    busy = true
    Task {
      let ok = await model.undo(action)
      busy = false
      say(ok ? "Undone" : "That can no longer be undone.", undo: nil)
      await reload()
      if case .merge = action { suggestions = await model.suggestions(for: person.id) }
    }
  }

  private func refine() {
    refining = true
    refineNote = ""
    selection = []
    lastUndo = nil
    status = ""
    Task {
      let removed = await model.refine(person.id)
      await reload()
      refining = false
      switch removed {
      case nil: refineNote = "Couldn't refine"
      case 0: refineNote = "All faces match"
      case 1: refineNote = "1 face moved out"
      case let n?: refineNote = "\(n) faces moved out"
      }
    }
  }
}

/// The next id ← or → from `cursor` (the first or last with none), clamped.
func step(_ ids: [UInt64], from cursor: UInt64?, by delta: Int) -> UInt64 {
  guard let cursor, let i = ids.firstIndex(of: cursor) else { return delta < 0 ? ids[ids.count - 1] : ids[0] }
  return ids[min(max(i + delta, 0), ids.count - 1)]
}

/// One face in a sheet: the round crop, a pin on the user's own, a caption
/// under a doubtful one, ✕ on hover.
private struct ReviewTile: View {
  let table: AITable
  let face: ReviewFace
  let caption: String?
  let selected: Bool
  let notLabel: String
  let toggle: () -> Void
  let reject: () -> Void
  let show: () -> Void
  @State private var hover = false

  private var label: String {
    var parts = [faceAccessibilityLabel(path: face.path, ptsMs: face.ptsMs)]
    if face.pinned { parts.append("pinned") }
    if let caption { parts.append(caption) }
    return parts.joined(separator: ", ")
  }

  var body: some View {
    VStack(spacing: 4) {
      FaceImage(table: table, faceID: face.id, box: face.box, monogram: "")
        .frame(width: 72, height: 72)
        .overlay(Circle().stroke(selected ? AITheme.accent : .clear, lineWidth: 2.5).padding(-2))
        .overlay(alignment: .bottomLeading) {
          if face.pinned {
            Image(systemName: "pin.circle.fill")
              .symbolRenderingMode(.palette)
              .foregroundStyle(.white, AITheme.accent)
              .font(.system(size: 16))
              .help("You said this is them")
          }
        }
        .overlay(alignment: .topTrailing) {
          if hover {
            Button(action: reject) {
              Image(systemName: "xmark.circle.fill")
                .symbolRenderingMode(.palette)
                .foregroundStyle(.white, .black.opacity(0.65))
                .font(.system(size: 16))
            }
            .buttonStyle(.plain)
            .help(notLabel)
            .accessibilityLabel(notLabel)
            .transition(.opacity)
          }
        }
      if let caption {
        Text(caption)
          .font(AITheme.font(11)).foregroundStyle(AITheme.body)
          .lineLimit(2).multilineTextAlignment(.center)
          .frame(maxWidth: 110)
      }
    }
    .onHover { h in withAnimation(.easeOut(duration: 0.12)) { hover = h } }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(label)
    .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
    .accessibilityHint("Selects or deselects this face")
    .accessibilityAction { toggle() }
    .accessibilityAction(named: notLabel) { reject() }
    .accessibilityAction(named: "Show photo") { show() }
  }
}

/// "Move to…": a searchable list of people, named first. Return takes the first.
struct PersonPicker: View {
  @ObservedObject var model: ManagementModel
  let excluding: UInt64?
  let pick: (Person) -> Void
  @State private var query = ""
  @FocusState private var searching: Bool

  private var candidates: [Person] { moveCandidates(model.people, excluding: excluding, query: query) }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      TextField("Find a person", text: $query)
        .textFieldStyle(.roundedBorder)
        .focused($searching)
        .onSubmit { if let first = candidates.first { pick(first) } }
        .accessibilityLabel("Find a person to move the faces to")
      let list = candidates
      if list.isEmpty {
        Text("No one by that name").font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .padding(.vertical, 8)
      }
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 2) {
          ForEach(list) { p in
            Button { pick(p) } label: {
              HStack(spacing: 8) {
                FaceImage(table: model.table, faceID: p.coverFace, box: p.coverBox,
                          monogram: String(p.name.prefix(1)).uppercased())
                  .frame(width: 28, height: 28)
                Text(model.displayName(p)).font(AITheme.font(13)).foregroundStyle(AITheme.title)
                  .lineLimit(1)
                Spacer()
                Text(p.faces == 1 ? "1 photo" : "\(p.faces) photos")
                  .font(AITheme.font(11)).foregroundStyle(AITheme.body)
              }
              .contentShape(Rectangle())
              .padding(.vertical, 3).padding(.horizontal, 4)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Move to \(model.displayName(p))")
          }
        }
      }
      .frame(height: 260)
    }
    .padding(12)
    .frame(width: 280)
    .onAppear { searching = true }
  }
}

/// A face to show in its whole picture.
struct PreviewFace: Identifiable, Equatable {
  let id: UInt64
  let path: String
  let ptsMs: Int64
  let box: [Double]

  init(id: UInt64, path: String, ptsMs: Int64, box: [Double]) {
    self.id = id
    self.path = path
    self.ptsMs = ptsMs
    self.box = box
  }
  init(_ f: ReviewFace) { self.init(id: f.id, path: f.path, ptsMs: f.ptsMs, box: f.box) }
  init(_ f: UnknownFace) { self.init(id: f.id, path: f.path, ptsMs: f.ptsMs, box: f.box) }
}

/// Where `box` (x, y, w, h in 0..1 of the picture) falls in a picture of
/// `image` size shown aspect-fit in `frame`.
func boxRect(_ box: [Double], image: CGSize, in frame: CGSize) -> CGRect? {
  guard box.count == 4, box[2] > 0, box[3] > 0, image.width > 0, image.height > 0 else { return nil }
  let scale = min(frame.width / image.width, frame.height / image.height)
  let w = image.width * scale, h = image.height * scale
  let ox = (frame.width - w) / 2, oy = (frame.height - h) / 2
  return CGRect(x: ox + box[0] * w, y: oy + box[1] * h, width: box[2] * w, height: box[3] * h)
}

/// Space / "Show photo": the picture the face was found in, the face outlined,
/// so the user judges by more than a small round crop. In memory only.
struct PhotoPreview: View {
  let table: AITable
  let face: PreviewFace
  let done: () -> Void
  @State private var image: CGImage?
  @State private var failed = false
  /// Space closes it again: the preview takes the keys while it is up.
  @FocusState private var keys: Bool

  var body: some View {
    VStack(spacing: 10) {
      GeometryReader { geo in
        ZStack(alignment: .topLeading) {
          if let image {
            Image(decorative: image, scale: 1).resizable().aspectRatio(contentMode: .fit)
              .frame(width: geo.size.width, height: geo.size.height)
            if let r = boxRect(face.box, image: CGSize(width: image.width, height: image.height), in: geo.size) {
              Rectangle().stroke(AITheme.accent, lineWidth: 2.5)
                .frame(width: r.width, height: r.height)
                .offset(x: r.minX, y: r.minY)
                .accessibilityHidden(true)
            }
          } else if failed {
            Text("The photo could not be shown.").font(AITheme.font(12)).foregroundStyle(AITheme.body)
              .frame(width: geo.size.width, height: geo.size.height)
          } else {
            ProgressView().frame(width: geo.size.width, height: geo.size.height)
          }
        }
      }
      .accessibilityElement(children: .ignore)
      .accessibilityLabel("Photo, the face outlined. " + faceAccessibilityLabel(path: face.path, ptsMs: face.ptsMs))
      HStack {
        Text(faceAccessibilityLabel(path: face.path, ptsMs: face.ptsMs))
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .lineLimit(1).truncationMode(.middle)
        Spacer()
        Button("Done", action: done).keyboardShortcut(.defaultAction)
      }
    }
    .padding(16)
    .frame(width: 760, height: 600)
    .focusable()
    .focused($keys)
    .focusEffectDisabled()
    .onAppear { keys = true }
    .onKeyPress(.space) {
      done()
      return .handled
    }
    .onExitCommand(perform: done)
    .task(id: face.id) {
      let t = table, id = face.id
      let pic = await Task.detached(priority: .userInitiated) { ImageLoad.facePicture(t, faceID: id) }.value
      if let pic { image = pic } else { failed = true }
    }
  }
}

/// "Unknown faces (N)": faces filed under nobody that are clear enough to name,
/// 60 at a time, each with whom it resembles. Yes, Move to…, Space.
struct UnknownFacesSheet: View {
  @ObservedObject var model: ManagementModel
  let done: () -> Void
  @State private var faces: [UnknownFace] = []
  @State private var total = 0
  @State private var loading = true
  @State private var selection = Set<UInt64>()
  @State private var cursor: UInt64?
  @State private var status = ""
  @State private var busy = false
  @State private var picking = false
  @State private var preview: PreviewFace?
  @FocusState private var focused: Bool

  private static let page = 60

  private var selected: [UnknownFace] { faces.filter { selection.contains($0.id) } }
  private var canYes: Bool { !selected.isEmpty && selected.allSatisfy { $0.suggest != 0 } }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .firstTextBaseline) {
        Text("Unknown faces").font(AITheme.font(18)).foregroundStyle(AITheme.title)
        Text(total == 1 ? "1 face" : "\(total) faces").font(AITheme.font(12)).foregroundStyle(AITheme.body)
        Spacer()
      }
      Text("Faces not filed under anyone yet. Say who they are. ⌘-click selects several, ← → move. Y yes, M move, Space shows the photo.")
        .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      ScrollViewReader { proxy in
        ScrollView {
          if loading && faces.isEmpty {
            ProgressView().frame(maxWidth: .infinity).padding(40)
          } else if faces.isEmpty {
            Text("No unknown faces left.").font(AITheme.font(12)).foregroundStyle(AITheme.body)
              .frame(maxWidth: .infinity).padding(40)
          }
          LazyVGrid(columns: [GridItem(.adaptive(minimum: 96, maximum: 120), spacing: 10)], spacing: 10) {
            ForEach(faces) { face in tile(face).id(face.id) }
          }
          .padding(4)
          if faces.count < total {
            Button(loading ? "Loading…" : "Show more") { Task { await load(more: true) } }
              .disabled(loading)
              .frame(maxWidth: .infinity).padding(.vertical, 8)
              .accessibilityLabel("Show more unknown faces")
          }
        }
        .focusable()
        .focused($focused)
        .focusEffectDisabled()
        .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
          guard !faces.isEmpty else { return .ignored }
          let id = step(faces.map(\.id), from: cursor, by: press.key == .leftArrow ? -1 : 1)
          cursor = id
          if press.modifiers.contains(.shift) { selection.insert(id) } else { selection = [id] }
          proxy.scrollTo(id)
          return .handled
        }
        .onKeyPress(.space) {
          let f = cursor.flatMap { c in faces.first { $0.id == c && selection.contains(c) } }
            ?? (selected.count == 1 ? selected.first : nil)
          guard let f else { return .ignored }
          preview = PreviewFace(f)
          return .handled
        }
        .onKeyPress(characters: CharacterSet(charactersIn: "ymYM")) { press in
          guard !press.modifiers.contains(.command), !selection.isEmpty, !busy else { return .ignored }
          if press.characters.lowercased() == "y" {
            guard canYes else { return .ignored }
            yes(selected)
          } else {
            picking = true
          }
          return .handled
        }
      }
      if !status.isEmpty {
        Text(status).font(AITheme.font(12)).foregroundStyle(AITheme.title)
      }
      HStack {
        Button("Yes") { yes(selected) }
          .disabled(!canYes || busy)
          .help("Each selected face is the person it looks like (Y)")
          .accessibilityLabel("Yes, each is the person suggested")
        Button("Move to…") { picking = true }
          .disabled(selection.isEmpty || busy)
          .help("These faces are someone (M)")
          .accessibilityLabel("Move to a person")
          .popover(isPresented: $picking, arrowEdge: .top) {
            PersonPicker(model: model, excluding: nil) { target in
              picking = false
              assign(selected, to: target)
              focused = true
            }
          }
        Spacer()
        Button("Done", action: done).keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 680, height: 560)
    .onExitCommand(perform: done)
    .sheet(item: $preview) { p in
      PhotoPreview(table: model.table, face: p) { preview = nil }
    }
    .task {
      await load(more: false)
      focused = true
    }
  }

  private func tile(_ face: UnknownFace) -> some View {
    let isSelected = selection.contains(face.id)
    var label = faceAccessibilityLabel(path: face.path, ptsMs: face.ptsMs)
    if !face.caption.isEmpty { label += ", looks like " + face.caption }
    return VStack(spacing: 4) {
      FaceImage(table: model.table, faceID: face.id, box: face.box, monogram: "")
        .frame(width: 72, height: 72)
        .overlay(Circle().stroke(isSelected ? AITheme.accent : .clear, lineWidth: 2.5).padding(-2))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label)
        .accessibilityAddTraits(isSelected ? [.isButton, .isSelected] : .isButton)
        .accessibilityAction { toggle(face.id) }
        .accessibilityAction(named: "Show photo") { preview = PreviewFace(face) }
      if face.suggest != 0 {
        Text(face.caption).font(AITheme.font(11)).foregroundStyle(AITheme.body).lineLimit(1)
        Button("Yes") { yes([face]) }
          .controlSize(.small).disabled(busy)
          .accessibilityLabel(face.suggestName.isEmpty ? "Yes, the person suggested" : "Yes, this is \(face.suggestName)")
      }
    }
    .onTapGesture {
      if NSEvent.modifierFlags.contains(.command) || NSEvent.modifierFlags.contains(.shift) {
        toggle(face.id)
      } else {
        selection = [face.id]
        cursor = face.id
      }
      focused = true  // Y, M and Space act on what was clicked
    }
    .contextMenu {
      Button("Show photo") { preview = PreviewFace(face) }
      if face.suggest != 0 { Button(face.suggestName.isEmpty ? "Yes" : "Yes, this is \(face.suggestName)") { yes([face]) } }
      Button("Move to…") {
        selection = [face.id]
        picking = true
      }
    }
  }

  private func toggle(_ id: UInt64) {
    if !selection.insert(id).inserted { selection.remove(id) }
    cursor = id
  }

  /// The first page, or the next. Faces named meanwhile have left the list,
  /// so the next page starts at the count still shown.
  private func load(more: Bool) async {
    loading = true
    let page = await model.unknownFaces(offset: more ? faces.count : 0, limit: Self.page)
    loading = false
    guard let page else { return }
    total = page.total
    if more {
      let have = Set(faces.map(\.id))
      faces += page.faces.filter { !have.contains($0.id) }
    } else {
      faces = page.faces
    }
  }

  /// Each face to the person it resembles: one call per person.
  private func yes(_ list: [UnknownFace]) {
    let named = list.filter { $0.suggest != 0 }
    guard !named.isEmpty, !busy else { return }
    busy = true
    Task {
      var moved: [UInt64] = []
      var to: [String] = []
      for (person, group) in Dictionary(grouping: named, by: \.suggest) {
        let ids = group.map(\.id)
        if await model.assign(ids, to: person, pin: true) {
          moved += ids
          to.append(group[0].suggestName)
        }
      }
      finish(moved, failed: moved.count < named.count,
             text: to.count == 1 ? movedSummary(moved.count, to: to[0]) : "Moved \(moved.count) faces")
    }
  }

  private func assign(_ list: [UnknownFace], to target: Person) {
    guard !list.isEmpty, !busy else { return }
    busy = true
    let ids = list.map(\.id)
    Task {
      let ok = await model.assign(ids, to: target.id, pin: true)
      finish(ok ? ids : [], failed: !ok, text: movedSummary(ids.count, to: target.name))
    }
  }

  private func finish(_ moved: [UInt64], failed: Bool, text: String) {
    busy = false
    withAnimation(.easeOut(duration: 0.18)) {
      faces.removeAll { moved.contains($0.id) }
      selection.subtract(moved)
    }
    total = max(0, total - moved.count)
    status = failed ? (moved.isEmpty ? "Couldn't move the faces. Try again." : text + ". Some could not be moved.") : text
    model.reloadPeople()
  }
}
