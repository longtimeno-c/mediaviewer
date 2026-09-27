// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People (plan/17 PR 24; the chrome brief's management panel): circular covers
// cut from the cover picture with cover_box in memory (never written to disk),
// editable names, "Show photos", merging (drag a person onto another, ⌘-click
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
      let t = table, f = faceID, b = box
      let cut = await Task.detached(priority: .utility) { ImageLoad.face(t, faceID: f, box: b) }.value
      withAnimation(.easeOut(duration: 0.2)) { image = cut }
    }
  }
}

struct PeopleGrid: View {
  @ObservedObject var model: ManagementModel
  let open: (Person) -> Void
  /// ⌘-click (or ⇧-click) picks several people to merge.
  @State private var selection = Set<UInt64>()
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      if model.people.isEmpty {
        Text("No people yet. Faces are grouped as your folders are indexed.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      } else {
        if selected.count >= 2 {
          mergeBar
            .transition(.opacity.combined(with: .move(edge: .top)))
        } else {
          Text("The same person twice? Drag one onto the other, or ⌘-click several and merge them.")
            .font(AITheme.font(12)).foregroundStyle(AITheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 104, maximum: 132), spacing: 14)], spacing: 16) {
          ForEach(model.people) { person in
            PersonCard(model: model, person: person, selected: selection.contains(person.id),
                       tap: { tap(person) }, open: { open(person) })
          }
        }
      }
      if !model.peopleNote.isEmpty {
        Text(model.peopleNote).font(AITheme.font(12)).foregroundStyle(AITheme.title)
          .transition(.opacity)
      }
    }
    .padding(12)
    .animation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.86), value: model.people)
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

  private func tap(_ person: Person) {
    let flags = NSEvent.modifierFlags
    if flags.contains(.command) || flags.contains(.shift) {
      if !selection.insert(person.id).inserted { selection.remove(person.id) }
    } else if !selection.isEmpty {
      selection = []
    } else {
      open(person)
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

private struct PersonCard: View {
  @ObservedObject var model: ManagementModel
  let person: Person
  let selected: Bool
  let tap: () -> Void
  let open: () -> Void
  @State private var name = ""
  @State private var hover = false
  @State private var dropTarget = false
  @FocusState private var editing: Bool
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  /// What a dragged person carries: its id, tagged so no other text merges.
  private static let dragPrefix = "mediaviewer-person:"

  var body: some View {
    VStack(spacing: 6) {
      FaceImage(table: model.table, faceID: person.coverFace, box: person.coverBox,
                monogram: String(person.name.prefix(1)).uppercased())
        .frame(width: 84, height: 84)
        .overlay(Circle().stroke(ring, lineWidth: selected || hover || dropTarget ? 2.5 : 1))
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
        .help("Click to see this person's faces. Drag onto another person to merge them.")
        .accessibilityLabel(person.name.isEmpty ? "Unnamed person" : person.name)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
      TextField("Add a name", text: $name)
        .textFieldStyle(.plain)
        .multilineTextAlignment(.center)
        .font(AITheme.font(12))
        .focused($editing)
        .onSubmit { commit() }
        .onChange(of: editing) { _, now in if !now { commit() } }
      Text(person.faces == 1 ? "1 photo" : "\(person.faces) photos")
        .font(AITheme.font(11)).foregroundStyle(AITheme.body)
        .contentTransition(.numericText())
    }
    .onAppear { name = person.name }
    .onChange(of: person.name) { _, n in if !editing { name = n } }
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

  private var ring: Color {
    selected || dropTarget ? .accentColor : hover ? Color.accentColor.opacity(0.7) : AITheme.hairline
  }

  private func commit() {
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed != person.name { model.rename(person.id, trimmed) }
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
