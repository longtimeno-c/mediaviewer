// SPDX-License-Identifier: GPL-2.0-or-later
// People (plan/17 PR 24; the chrome brief's management panel): circular covers
// cut from the cover picture with cover_box in memory (never written to disk),
// editable names, "Show photos", "Merge into…", and a person's faces with
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

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      if model.people.isEmpty {
        Text("No people yet. Faces are grouped as your folders are indexed.")
          .font(AITheme.font(12)).foregroundStyle(AITheme.body)
      } else {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 104, maximum: 132), spacing: 14)], spacing: 16) {
          ForEach(model.people) { person in
            PersonCard(model: model, person: person, open: { open(person) })
          }
        }
      }
    }
    .padding(12)
  }
}

private struct PersonCard: View {
  @ObservedObject var model: ManagementModel
  let person: Person
  let open: () -> Void
  @State private var name = ""
  @State private var hover = false
  @FocusState private var editing: Bool
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(spacing: 6) {
      FaceImage(table: model.table, faceID: person.coverFace, box: person.coverBox,
                monogram: String(person.name.prefix(1)).uppercased())
        .frame(width: 84, height: 84)
        .overlay(Circle().stroke(hover ? Color.accentColor : AITheme.hairline, lineWidth: hover ? 2 : 1))
        .scaleEffect(hover && !reduceMotion ? 1.04 : 1)
        .animation(.easeOut(duration: 0.15), value: hover)
        .onHover { hover = $0 }
        .onTapGesture { open() }
        .accessibilityLabel(person.name.isEmpty ? "Unnamed person" : person.name)
        .accessibilityAddTraits(.isButton)
      TextField("Add a name", text: $name)
        .textFieldStyle(.plain)
        .multilineTextAlignment(.center)
        .font(AITheme.font(12))
        .focused($editing)
        .onSubmit { commit() }
        .onChange(of: editing) { _, now in if !now { commit() } }
      Text(person.faces == 1 ? "1 photo" : "\(person.faces) photos")
        .font(AITheme.font(11)).foregroundStyle(AITheme.body)
    }
    .onAppear { name = person.name }
    .onChange(of: person.name) { _, n in if !editing { name = n } }
    .contextMenu {
      Button("Show photos") { model.showPhotos(of: person) }
      Button("Faces…") { open() }
      Menu("Merge into…") {
        ForEach(model.people.filter { $0.id != person.id }) { other in
          Button(other.name.isEmpty ? "Unnamed (\(other.faces))" : other.name) {
            model.merge(into: other.id, from: person.id)
          }
        }
      }
    }
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
            Button(other.name.isEmpty ? "Unnamed (\(other.faces))" : other.name) {
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
    for id in ids { model.reject(id) }
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
