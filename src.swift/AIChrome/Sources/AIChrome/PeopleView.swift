// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People (docs/design/17 PR 24; the chrome brief's management panel): circular covers
// cut from the cover picture with cover_box in memory (never written to disk);
// a click opens their photos in the gallery (Settings steps aside); editable
// names, merging (drag a person onto another, ⌘-click
// several then "Merge into…", or the context menu), and a person's faces with
// "Not this person" (hover ✕ or Delete) and multi-select "Split into new
// person". A grouping that cannot be corrected is worse than none.
import AppKit
import SwiftUI

/// A face or cover, cropped in memory. Monogram until (or unless) it decodes.
struct FaceImage: View {
  let table: AITable
  let faceID: UInt64
  let box: [Double]
  let monogram: String
  @State private var image: CGImage?

  var body: some View {
    ZStack {
      Circle().fill(Color.primary.opacity(0.08))
      if let image {
        Image(decorative: image, scale: 1).resizable().aspectRatio(contentMode: .fill)
          .transition(.opacity)
      } else {
        Text(monogram).font(AITheme.font(18)).foregroundStyle(AITheme.body)
      }
    }
    .clipShape(Circle())
    .task(id: faceID) {
      guard faceID != 0 else { return }
      // A cover seen before (Settings reopened, the grid re-sorted): no task.
      if let hit = ImageCache.shared.get(ImageLoad.faceKey(faceID, box: box)) {
        image = hit
        return
      }
      // A few decodes at once (ThumbGate), not one per card: 200 people are
      // 200 face_thumb calls into the pack, each possibly a decode, and
      // unbounded they fill the cooperative pool and stall every other task.
      let t = table, f = faceID, b = box
      let work = Task.detached(priority: .utility) { () -> CGImage? in
        await ThumbGate.shared.acquire()
        let cut = Task.isCancelled ? nil : ImageLoad.face(t, faceID: f, box: b)
        await ThumbGate.shared.release()
        return cut
      }
      let cut = await withTaskCancellationHandler { await work.value } onCancel: { work.cancel() }
      guard let cut, !Task.isCancelled else { return }
      withAnimation(.easeOut(duration: 0.2)) { image = cut }
    }
  }
}

struct PeopleGrid: View {
  @ObservedObject var model: ManagementModel
  let open: (Person) -> Void
  /// Settings lists the first `limit` people (named, then most photos) and a
  /// "Show all" that calls `showAll`: the base Settings list is not lazy, so
  /// every card there is live at once, and 200 of them made it scroll slowly
  /// (owner, 2026-10-06). The all-people sheet passes nil: its grid is in a
  /// ScrollView of its own, where the LazyVGrid is lazy.
  var limit: Int? = nil
  var showAll: (() -> Void)? = nil
  /// ⌘-click (or ⇧-click) picks several people to merge.
  @State private var selection = Set<UInt64>()
  /// "Merge duplicates" is running in the pack.
  @State private var deduping = false
  /// "Merge duplicates" in detail: the look, shown until Apply or Cancel.
  @State private var dedupeLook: DedupePlan?
  /// The unknown faces sheet is open.
  @State private var showUnknown = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      if !model.folder.isEmpty {
        scopeBar
      }
      if model.people.isEmpty {
        Text(model.peopleScopeDir == nil
             ? "No people yet. Faces are grouped as your folders are indexed."
             : model.peopleScope == .folder
               ? "Nobody in \(model.folderName) yet. Faces are grouped as the folder is indexed; everyone found shows when no folder is open."
               : "Nobody in \(model.folderName) or its subfolders yet. Faces are grouped as the folder is indexed; everyone found shows when no folder is open.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        if selected.count >= 2 {
          mergeBar
            .transition(.opacity.combined(with: .move(edge: .top)))
        } else {
          HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(model.readOnly
                 ? "Click a person to see their photos. People are named and merged in the first MediaViewer window you opened."
                 : "Click a person to see their photos. The same person twice? Drag one onto the other, or ⌘-click several and merge them.")
              .font(AITheme.font(12)).foregroundStyle(AITheme.body)
              .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            if model.canDedupe && !model.readOnly {
              if deduping {
                ProgressView().controlSize(.small)
              }
              Button("Merge duplicates") { mergeDuplicates() }
                .disabled(deduping)
                .help(model.canDedupeDetail
                      ? "Re-check every face and merge people who are the same person. Two people you named differently are never merged. You see the merges before they happen, and can undo them."
                      : "Re-check every face and merge people who are the same person. Two people you named differently are never merged; undo with Split.")
            }
          }
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 104, maximum: 132), spacing: 14)], spacing: 16) {
          ForEach(shown) { person in
            PersonCard(model: model, person: person, selected: selection.contains(person.id),
                       opening: model.opening == person.id,
                       tap: { tap(person) }, open: { open(person) })
              .equatable()
          }
        }
        HStack(spacing: 12) {
          if let showAll, shown.count < model.people.count {
            Button("Show all \(model.people.count) people…", action: showAll)
              .help(model.readOnly ? "Every person found" : "Every person found, to name, merge and correct")
          }
          // Faces filed under nobody, clear enough to name (docs/design/17
          // "Correcting people"). Hidden with none, or with an older pack.
          if model.unknownCount > 0 && !model.readOnly {
            Button("Unknown faces (\(countText(UInt64(model.unknownCount))))") { showUnknown = true }
              .help("Faces not filed under anyone yet. Say who they are.")
              .accessibilityLabel("Unknown faces, \(model.unknownCount)")
          }
        }
      }
      if !model.peopleNote.isEmpty {
        HStack(spacing: 10) {
          Text(model.peopleNote).font(AITheme.font(12)).foregroundStyle(AITheme.title)
          if model.mergeUndo != nil {
            Button("Undo") { model.undoGridMerge() }
              .help("Put the people back as they were")
              .accessibilityLabel("Undo merge")
          }
        }
        .transition(.opacity)
      }
    }
    .padding(12)
    // The spring when a person comes or goes (a merge, a split, a new face
    // group), not for every count or re-sort while faces stream in: an
    // animation on the container re-lays the whole grid out per frame, up to
    // 500 ms of CPU per update at 200 people (PeopleGridBench); the counts and
    // the order change twice a second while indexing.
    .animation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.86), value: Set(model.people.map(\.id)))
    .animation(reduceMotion ? nil : .easeOut(duration: 0.18), value: selection)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.18), value: model.peopleNote)
    // A merged or regrouped person leaves the selection with the grid.
    .onChange(of: model.people) { _, people in
      let ids = Set(people.map(\.id))
      if !selection.isSubset(of: ids) { selection.formIntersection(ids) }
    }
    .onExitCommand { selection = [] }
    .sheet(isPresented: $showUnknown) {
      UnknownFacesSheet(model: model) { showUnknown = false }
    }
    .sheet(isPresented: Binding(get: { dedupeLook != nil }, set: { if !$0 { dedupeLook = nil } })) {
      if let plan = dedupeLook {
        DedupeSheet(model: model, plan: plan, cancel: { dedupeLook = nil }, apply: applyDuplicates)
      }
    }
  }

  /// The people this grid shows: the first `limit`, or everyone.
  private var shown: ArraySlice<Person> {
    guard let limit else { return model.people[...] }
    return model.people.prefix(limit)
  }

  /// The selected people, in grid order.
  private var selected: [Person] { model.people.filter { selection.contains($0.id) } }

  private var scopeHelp: String {
    switch model.peopleScope {
    case .folder: return "People with a face in the open folder only."
    case .tree: return "People with a face in the open folder and the folders inside it."
    case .all, .photos: return "Every person found, in every indexed folder."
    }
  }

  /// "People in · This folder | + Subfolders · Photos": the grid follows the
  /// folder the viewer has open (docs/design/17 "People in the open folder"), the
  /// search panel's words. Everywhere is not a choice here (owner,
  /// 2026-10-03): a folder shows its own people, and everyone shows when no
  /// folder is open (this bar is then absent).
  private var scopeBar: some View {
    HStack(spacing: 8) {
      Text("People in").font(AITheme.font(12)).foregroundStyle(AITheme.body)
      Picker("People in", selection: $model.peopleScope) {
        ForEach([SearchScope.folder, .tree]) { scope in
          Text(scope.label).tag(scope)
        }
      }
      .pickerStyle(.segmented).labelsHidden().fixedSize()
      .help(scopeHelp)
      Text(model.folderName).font(AITheme.font(12)).foregroundStyle(AITheme.title)
        .lineLimit(1).truncationMode(.middle)
        .help(model.folder)
    }
  }

  private func tap(_ person: Person) {
    let flags = NSEvent.modifierFlags
    if model.readOnly {
      model.showPhotos(of: person)  // nothing to select for: merging is the first window's
    } else if flags.contains(.command) || flags.contains(.shift) {
      if !selection.insert(person.id).inserted { selection.remove(person.id) }
    } else if !selection.isEmpty {
      selection = []
    } else {
      // A click shows who it is: their photos, in the gallery.
      model.showPhotos(of: person)
    }
  }

  /// "Merge duplicates" (docs/design/17): the whole library, on request; the note
  /// under the grid says what it did.
  private func mergeDuplicates() {
    guard !model.canDedupeDetail else { return lookDuplicates() }
    deduping = true
    Task {
      let result = await model.mergeDuplicates()
      deduping = false
      switch result {
      case nil:
        model.note("Couldn't check for duplicates. Try again.")
      case (0, 0)?:
        model.note("No duplicates found, and every face matches.")
      case let (m, f)?:
        model.note((m == 1 ? "1 person" : "\(m) people") + " merged, " + (f == 1 ? "1 face" : "\(f) faces") + " moved.")
      }
    }
  }

  /// Merge duplicates in detail, step 1: a look (MV_AI_DRY_RUN), nothing changed.
  private func lookDuplicates() {
    deduping = true
    Task {
      let plan = await model.dedupeDetail(dry: true)
      deduping = false
      switch plan {
      case nil: model.note("Couldn't check for duplicates. Try again.")
      case let p? where p.isEmpty: model.note("No duplicates found, and every face matches.")
      case let p?: dedupeLook = p
      }
    }
  }

  /// Step 2: for real; the note says what it did, with Undo of every merge.
  private func applyDuplicates() {
    dedupeLook = nil
    deduping = true
    Task {
      let plan = await model.dedupeDetail(dry: false)
      deduping = false
      guard let plan else { return model.note("Couldn't merge the duplicates. Try again.") }
      let tokens = model.table.canMergeUndo ? plan.undoTokens : []
      model.note(plan.summary, seconds: tokens.isEmpty ? 5 : 8, undo: tokens.isEmpty ? nil : tokens)
    }
  }

  /// "3 people selected · Merge into ▾ · Cancel". The target keeps its name.
  private var mergeBar: some View {
    HStack(spacing: 10) {
      Text("\(selected.count) people selected")
        .font(AITheme.font(12)).foregroundStyle(AITheme.title)
      Spacer()
      Menu("Merge into…") {
        ForEach(selected) { target in
          Button(model.displayName(target)) {
            model.merge(into: target.id, from: selected.map(\.id))
            selection = []
          }
        }
      }
      .fixedSize()
      Button("Cancel") { selection = [] }
    }
    .padding(10)
    .background(RoundedRectangle(cornerRadius: AITheme.radius(6)).fill(AITheme.accent.opacity(0.08)))
  }
}

/// One person. Not an observer of the model: at 200 people every publish
/// (the status line at 4 Hz while indexing, a note, an opening spinner) ran
/// 200 bodies, about 50 ms of CPU each tick (PeopleGridBench). The card takes what it
/// shows as values, compares them (Equatable, `.equatable()` in the grid) and
/// reads the model only in an action. The cover, with its drag, drop, hover
/// and menus, is its own Equatable view: a face count moving while faces
/// stream in re-renders the count, not the cover.
///
/// @MainActor is spelled out on these three: with no @ObservedObject left to
/// infer it, Swift 5.10's SwiftUI (Xcode 15, the CI runner) leaves a View
/// nonisolated, and an action calling the model would not compile there.
/// Their `==` reads only Sendable lets, so it stays nonisolated as Equatable
/// requires.
@MainActor
private struct PersonCard: View, Equatable {
  let model: ManagementModel
  let person: Person
  let selected: Bool
  /// Their photos are being opened (the spinner).
  let opening: Bool
  let tap: () -> Void
  let open: () -> Void

  nonisolated static func == (a: PersonCard, b: PersonCard) -> Bool {
    a.person == b.person && a.selected == b.selected && a.opening == b.opening
  }

  var body: some View {
    VStack(spacing: 6) {
      PersonCover(model: model, person: person, selected: selected, opening: opening, tap: tap, open: open)
        .equatable()
      PersonName(model: model, person: person)
        .equatable()
      Text(person.faces == 1 ? "1 photo" : "\(person.faces) photos")
        .font(AITheme.font(11)).foregroundStyle(AITheme.body)
        .lineLimit(1)
    }
    .frame(maxWidth: .infinity)
    .contextMenu {
      Button("Show photos") { model.showPhotos(of: person) }
      if !model.readOnly {  // a later window changes no person
        Button("Faces…") { open() }
        Menu("Merge into…") {
          ForEach(model.mergeTargets(for: person)) { other in
            Button(model.displayName(other)) { model.merge(into: other.id, from: person.id) }
          }
        }
      }
    }
  }
}

/// The circular cover: click, hover (the Faces… button), drag out, drop onto
/// (merge), selection and the opening spinner.
@MainActor
private struct PersonCover: View, Equatable {
  let model: ManagementModel
  let person: Person
  let selected: Bool
  let opening: Bool
  let tap: () -> Void
  let open: () -> Void
  @State private var hover = false
  @State private var dropTarget = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  nonisolated static func == (a: PersonCover, b: PersonCover) -> Bool {
    a.person.id == b.person.id && a.person.name == b.person.name && a.person.coverFace == b.person.coverFace
      && a.person.coverBox == b.person.coverBox && a.selected == b.selected && a.opening == b.opening
  }

  /// What a dragged person carries: its id, tagged so no other text merges.
  private static let dragPrefix = "mediaviewer-person:"

  var body: some View {
    FaceImage(table: model.table, faceID: person.coverFace, box: person.coverBox,
              monogram: String(person.name.prefix(1)).uppercased())
      .frame(width: 84, height: 84)
      .overlay(Circle().stroke(ring, lineWidth: selected || hover || dropTarget ? 2.5 : 1))
      .overlay {
        if opening {
          ZStack {
            Circle().fill(.black.opacity(0.35))
            ProgressView().controlSize(.small).tint(.white)
          }
          .transition(.opacity)
        }
      }
      .overlay(alignment: .topTrailing) {
        // Correcting a person's faces: here on hover, and in the context menu.
        if hover && !selected && !model.readOnly {
          Button(action: open) {
            Image(systemName: "ellipsis.circle.fill")
              .symbolRenderingMode(.palette)
              .foregroundStyle(.white, .black.opacity(0.6))
              .font(.system(size: 20))
          }
          .buttonStyle(.plain)
          .help("Faces… — rename, merge, or remove faces that are someone else")
          .accessibilityLabel("Faces")
          .transition(.opacity)
        }
      }
      .overlay(alignment: .bottomTrailing) {
        if selected {
          Image(systemName: "checkmark.circle.fill")
            .symbolRenderingMode(.palette)
            .foregroundStyle(.white, AITheme.accent)
            .font(.system(size: 20))
            .transition(.scale.combined(with: .opacity))
        }
      }
      .scaleEffect((hover || dropTarget) && !reduceMotion ? 1.06 : 1)
      .animation(.easeOut(duration: 0.15), value: hover)
      .animation(.easeOut(duration: 0.15), value: dropTarget)
      .onHover { hover = $0 }
      .onTapGesture { tap() }
      .draggable(Self.dragPrefix + String(person.id)) {
        FaceImage(table: model.table, faceID: person.coverFace, box: person.coverBox,
                  monogram: String(person.name.prefix(1)).uppercased())
          .frame(width: 60, height: 60)
      }
      .dropDestination(for: String.self) { items, _ in
        let ids = items.compactMap { item -> UInt64? in
          guard item.hasPrefix(Self.dragPrefix) else { return nil }
          return UInt64(item.dropFirst(Self.dragPrefix.count))
        }.filter { $0 != person.id }
        guard !ids.isEmpty, !model.readOnly else { return false }
        model.merge(into: person.id, from: ids)
        return true
      } isTargeted: { dropTarget = $0 }
      .help(model.readOnly ? "Click to see this person's photos."
            : "Click to see this person's photos. Drag onto another person to merge them.")
      // One element whatever it draws: once the cover decodes, FaceImage's
      // picture is decorative and the monogram is gone, and without this the
      // label had nothing to sit on, so VoiceOver skipped the person.
      .accessibilityElement(children: .ignore)
      .accessibilityLabel(person.name.isEmpty ? "Unnamed person" : person.name)
      .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
      .accessibilityHint("Shows their photos in the gallery")
      .accessibilityAction(named: "Faces") { if !model.readOnly { open() } }
  }

  private var ring: Color {
    selected || dropTarget ? .accentColor : hover ? AITheme.accent.opacity(0.7) : AITheme.hairline
  }
}

/// The name: a label until clicked (or Return on it), then a text field that
/// saves on Return or when focus leaves. 200 live NSTextFields were most of
/// the grid's layout and event cost.
@MainActor
private struct PersonName: View, Equatable {
  let model: ManagementModel
  let person: Person
  @State private var name = ""
  @State private var editingName = false
  @FocusState private var editing: Bool

  nonisolated static func == (a: PersonName, b: PersonName) -> Bool {
    a.person.id == b.person.id && a.person.name == b.person.name
  }

  var body: some View {
    if editingName {
      TextField("Add a name", text: $name)
        .textFieldStyle(.plain)
        .multilineTextAlignment(.center)
        .font(AITheme.font(12))
        .focused($editing)
        .onSubmit { commit() }
        .onChange(of: editing) { _, now in if !now { commit() } }
        .onAppear { editing = true }
    } else {
      Text(person.name.isEmpty ? (model.readOnly ? "Unnamed" : "Add a name") : person.name)
        .font(AITheme.font(12))
        .foregroundStyle(person.name.isEmpty ? AITheme.body : AITheme.title)
        .lineLimit(1).truncationMode(.tail)
        .frame(maxWidth: .infinity)
        .contentShape(Rectangle())
        .onTapGesture { beginEditing() }
        .focusable(!model.readOnly)
        .onKeyPress(.return) {
          beginEditing()
          return .handled
        }
        .help(model.readOnly ? "" : person.name.isEmpty ? "Click to name this person" : "Click to rename")
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(person.name.isEmpty ? "Add a name" : "Name, \(person.name)")
        .accessibilityHint("Edits the name")
    }
  }

  private func beginEditing() {
    guard !model.readOnly else { return }  // named in the first window
    name = person.name
    editingName = true
  }

  /// The edit ends (Return, or focus left): save a change, show the label.
  private func commit() {
    guard editingName else { return }
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed != person.name { model.rename(person.id, trimmed) }
    editingName = false
  }
}

/// Everyone, from "Show all … people" under the Settings grid: the same grid,
/// in a scroll view of its own so only the cards on screen are live.
struct AllPeopleSheet: View {
  @ObservedObject var model: ManagementModel
  let done: () -> Void
  @State private var openPerson: Person?

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .firstTextBaseline) {
        Text("People").font(AITheme.font(18)).foregroundStyle(AITheme.title)
        Text(model.people.count == 1 ? "1 person" : "\(model.people.count) people")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
        Spacer()
        Button("Done", action: done).keyboardShortcut(.defaultAction)
      }
      ScrollView {
        PeopleGrid(model: model, open: { openPerson = $0 })
      }
    }
    .padding(20)
    .frame(minWidth: 640, idealWidth: 820, minHeight: 420, idealHeight: 640)
    .onExitCommand(perform: done)  // Esc closes, as Done does (issue #192)
    // Their photos open in the gallery, as from Settings: the sheet steps aside.
    .onChange(of: model.opening) { _, id in
      if id != nil { done() }
    }
    .sheet(item: $openPerson) { person in
      PersonSheet(model: model, person: person) { openPerson = nil }
    }
  }
}

/// "Merge duplicates" in detail: the merges a look proposes, each as the
/// covers of the people that would go and the one they would join, with
/// Apply and Cancel. Nothing has changed while it is up.
struct DedupeSheet: View {
  @ObservedObject var model: ManagementModel
  let plan: DedupePlan
  let cancel: () -> Void
  let apply: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Merge duplicates").font(AITheme.font(18)).foregroundStyle(AITheme.title)
      Text(plan.merges.isEmpty ? "No duplicates found." : "These people look like the same person. Nothing changes until you apply.")
        .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      ScrollView {
        VStack(alignment: .leading, spacing: 14) {
          ForEach(plan.merges) { merge in row(merge) }
        }
        .padding(4)
      }
      .frame(minHeight: 200)
      if !plan.movedNote.isEmpty {
        Text(plan.movedNote).font(AITheme.font(12)).foregroundStyle(AITheme.body)
      }
      HStack {
        Spacer()
        Button("Cancel", action: cancel)
          .keyboardShortcut(.cancelAction)
          .help("Leave everyone as they are")
          .accessibilityLabel("Cancel, change nothing")
        Button("Apply", action: apply)
          .keyboardShortcut(.defaultAction)
          .help("Merge these people. Undo puts them back.")
          .accessibilityLabel(plan.merges.count == 1 ? "Apply the merge" : "Apply \(plan.merges.count) merges")
      }
    }
    .padding(20)
    .frame(width: 560, height: 480)
    .onExitCommand(perform: cancel)
  }

  private func faces(_ n: Int) -> String { n == 1 ? "1 face" : "\(n) faces" }

  private func row(_ merge: DedupePlan.Merge) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(merge.title).font(AITheme.font(13)).foregroundStyle(AITheme.title)
      HStack(alignment: .top, spacing: 12) {
        ForEach(merge.from) { p in
          cover(face: p.coverFace, box: p.coverBox, name: p.name, caption: faces(p.faces))
        }
        Image(systemName: "arrow.right").foregroundStyle(AITheme.body).padding(.top, 18)
          .accessibilityHidden(true)
        cover(face: merge.coverFace, box: merge.coverBox, name: merge.intoName,
              caption: merge.intoName.isEmpty ? faces(merge.intoFaces) : merge.intoName)
      }
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(merge.title + ", " + merge.from.map { faces($0.faces) }.joined(separator: " and ")
                        + " into " + faces(merge.intoFaces))
  }

  private func cover(face: UInt64, box: [Double], name: String, caption: String) -> some View {
    VStack(spacing: 4) {
      FaceImage(table: model.table, faceID: face, box: box, monogram: String(name.prefix(1)).uppercased())
        .frame(width: 56, height: 56)
      Text(caption).font(AITheme.font(11)).foregroundStyle(AITheme.body)
        .lineLimit(1).frame(maxWidth: 80)
    }
  }
}
