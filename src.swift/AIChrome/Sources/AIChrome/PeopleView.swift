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
private struct FaceImage: View {
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
  /// ⌘-click (or ⇧-click) picks several people to merge.
  @State private var selection = Set<UInt64>()
  /// "Merge duplicates" is running in the pack.
  @State private var deduping = false
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
            Text("Click a person to see their photos. The same person twice? Drag one onto the other, or ⌘-click several and merge them.")
              .font(AITheme.font(12)).foregroundStyle(AITheme.body)
              .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            if model.canDedupe {
              if deduping {
                ProgressView().controlSize(.small)
              }
              Button("Merge duplicates") { mergeDuplicates() }
                .disabled(deduping)
                .help("Re-check every face and merge people who are the same person. Two people you named differently are never merged; undo with Split.")
            }
          }
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 104, maximum: 132), spacing: 14)], spacing: 16) {
          ForEach(model.people) { person in
            PersonCard(model: model, person: person, selected: selection.contains(person.id),
                       opening: model.opening == person.id,
                       tap: { tap(person) }, open: { open(person) })
              .equatable()
          }
        }
      }
      if !model.peopleNote.isEmpty {
        Text(model.peopleNote).font(AITheme.font(12)).foregroundStyle(AITheme.title)
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
    if flags.contains(.command) || flags.contains(.shift) {
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
    .background(RoundedRectangle(cornerRadius: 6).fill(Color.accentColor.opacity(0.08)))
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
      Button("Faces…") { open() }
      Menu("Merge into…") {
        ForEach(model.people.filter { $0.id != person.id }) { other in
          Button(model.displayName(other)) { model.merge(into: other.id, from: person.id) }
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
        if hover && !selected {
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
            .foregroundStyle(.white, Color.accentColor)
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
        guard !ids.isEmpty else { return false }
        model.merge(into: person.id, from: ids)
        return true
      } isTargeted: { dropTarget = $0 }
      .help("Click to see this person's photos. Drag onto another person to merge them.")
      .accessibilityLabel(person.name.isEmpty ? "Unnamed person" : person.name)
      .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
      .accessibilityHint("Shows their photos in the gallery")
      .accessibilityAction(named: "Faces") { open() }
  }

  private var ring: Color {
    selected || dropTarget ? .accentColor : hover ? Color.accentColor.opacity(0.7) : AITheme.hairline
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
      Text(person.name.isEmpty ? "Add a name" : person.name)
        .font(AITheme.font(12))
        .foregroundStyle(person.name.isEmpty ? AITheme.body : AITheme.title)
        .lineLimit(1).truncationMode(.tail)
        .frame(maxWidth: .infinity)
        .contentShape(Rectangle())
        .onTapGesture { beginEditing() }
        .focusable()
        .onKeyPress(.return) {
          beginEditing()
          return .handled
        }
        .help(person.name.isEmpty ? "Click to name this person" : "Click to rename")
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(person.name.isEmpty ? "Add a name" : "Name, \(person.name)")
        .accessibilityHint("Edits the name")
    }
  }

  private func beginEditing() {
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

/// A person's faces: "Not this person", split, merge, show photos.
struct PersonSheet: View {
  @ObservedObject var model: ManagementModel
  let person: Person
  let done: () -> Void
  @State private var faces: [Face] = []
  @State private var selection = Set<UInt64>()
  @State private var loading = true
  @State private var refining = false
  @State private var refineNote = ""
  @FocusState private var focused: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text(person.name.isEmpty ? "Unnamed person" : person.name)
          .font(AITheme.font(18)).foregroundStyle(AITheme.title)
        Spacer()
        Button("Show photos") { done(); model.showPhotos(of: person) }
        Menu("Merge into…") {
          ForEach(model.people.filter { $0.id != person.id }) { other in
            Button(model.displayName(other)) {
              model.merge(into: other.id, from: person.id)
              done()
            }
          }
        }
        .fixedSize()
      }
      Text("Select faces that are someone else. ⌘-click selects several; Delete removes them from this person.")
        .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      ScrollView {
        if loading {
          ProgressView().frame(maxWidth: .infinity).padding(40)
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 76, maximum: 92), spacing: 10)], spacing: 10) {
          ForEach(faces) { face in
            FaceCell(table: model.table, face: face, selected: selection.contains(face.id),
                     reject: { reject([face.id]) })
              .onTapGesture {
                if NSEvent.modifierFlags.contains(.command) || NSEvent.modifierFlags.contains(.shift) {
                  if !selection.insert(face.id).inserted { selection.remove(face.id) }
                } else {
                  selection = [face.id]
                }
              }
          }
        }
        .padding(4)
      }
      .frame(minHeight: 280)
      .focusable()
      .focused($focused)
      .focusEffectDisabled()
      .onKeyPress(keys: [.delete, .deleteForward]) { _ in
        guard !selection.isEmpty else { return .ignored }
        reject(Array(selection))
        return .handled
      }
      HStack {
        Button("Not this person") { reject(Array(selection)) }
          .disabled(selection.isEmpty)
        Button("Split into new person") {
          model.split(Array(selection))
          faces.removeAll { selection.contains($0.id) }
          selection = []
        }
        .disabled(selection.count < 1)
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
    .padding(20)
    .frame(width: 620, height: 520)
    .task {
      faces = await model.faces(of: person.id)
      loading = false
      focused = true
    }
  }

  private func refine() {
    refining = true
    refineNote = ""
    selection = []
    Task {
      let removed = await model.refine(person.id)
      let now = await model.faces(of: person.id)
      withAnimation(.easeOut(duration: 0.18)) { faces = now }
      refining = false
      switch removed {
      case nil: refineNote = "Couldn't refine"
      case 0: refineNote = "All faces match"
      case 1: refineNote = "1 face moved out"
      case let n?: refineNote = "\(n) faces moved out"
      }
    }
  }

  private func reject(_ ids: [UInt64]) {
    model.reject(ids)
    withAnimation(.easeOut(duration: 0.18)) {
      faces.removeAll { ids.contains($0.id) }
      selection.subtract(ids)
    }
  }
}

private struct FaceCell: View {
  let table: AITable
  let face: Face
  let selected: Bool
  let reject: () -> Void
  @State private var hover = false

  var body: some View {
    FaceImage(table: table, faceID: face.id, box: face.box, monogram: "")
      .frame(width: 72, height: 72)
      .overlay(Circle().stroke(selected ? Color.accentColor : .clear, lineWidth: 2.5).padding(-2))
      .overlay(alignment: .topTrailing) {
        if hover {
          Button(action: reject) {
            Image(systemName: "xmark.circle.fill")
              .symbolRenderingMode(.palette)
              .foregroundStyle(.white, .black.opacity(0.65))
              .font(.system(size: 16))
          }
          .buttonStyle(.plain)
          .help("Not this person")
          .accessibilityLabel("Not this person")
          .transition(.opacity)
        }
      }
      .onHover { h in withAnimation(.easeOut(duration: 0.12)) { hover = h } }
      .accessibilityAddTraits(selected ? .isSelected : [])
  }
}
