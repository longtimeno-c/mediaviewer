// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S8 (§3.2 Faces): the Faces tool's panel in the
// Enhance tab. While the tool is armed the canvas draws a numbered box per
// face (a click chooses it or leaves it out); here each face has its
// thumbnail, the person's name from People and "from 37 photos of Anna", and
// its fixes: Open eyes (only where the eyes look shut), Improve face and,
// with Enhance, Restore face. Do all runs each chosen face's best fix into
// one new file. With People off, a button opens Local search to turn it on.
// The host owns the state (shell/faces_tool.h through mv_chrome_faces_panel /
// mv_chrome_face_card). The fixes are a table (FaceAction,
// shell::kFaceActions): S10's Restore face is the third case, run by
// Enhance's docked panel on the chosen faces. The Windows twin is
// IslandHost.Faces.cs.
import AppKit
import MVChromeBridge
import SwiftUI

/// shell::face_action, in wire order (append only, with shell/faces_tool.h).
enum FaceAction: Int32, CaseIterable, Identifiable {
  case openEyes = 0, improve = 1
  /// S10's model through Enhance's docked panel, on this face (offered while
  /// Enhance has its "faces" command; never mv_adjust_faces).
  case restore = 2
  var id: Int32 { rawValue }

  var label: String {
    switch self {
    case .openEyes: return "Open eyes"
    case .improve: return "Improve face"
    case .restore: return "Restore face"
    }
  }

  var busy: String {
    switch self {
    case .openEyes: return "Opening eyes…"
    case .improve: return "Finding a better face…"
    case .restore: return "Restoring in Enhance…"
    }
  }

  var symbol: String {
    switch self {
    case .openEyes: return "eye"
    case .improve: return "wand.and.stars"
    case .restore: return "person.crop.circle.badge.checkmark"
    }
  }

  var help: String {
    switch self {
    case .openEyes:
      return "This person's open eyes from a photo of the same moment or from People, matched to this light. Saves a new file."
    case .improve:
      return "A clearly better face of the same person from your photos, turned the same way, in this photo's light. Saves a new file."
    case .restore:
      return "Enhance's face model makes this face sharper and clearer, checked by People to still look like the same person. Saves a new file."
    }
  }
}

/// One face as the panel shows it.
struct FaceCard: Identifiable, Equatable {
  let id: Int
  let person: Int64
  let name: String
  let source: String
  let actions: Int32
  let shut: Bool
  let thumb: NSImage?

  func offers(_ a: FaceAction) -> Bool { actions & (1 << a.rawValue) != 0 }
  var title: String { !name.isEmpty ? name : (person != 0 ? "Unnamed person" : "Not in People yet") }

  static func == (a: FaceCard, b: FaceCard) -> Bool {
    a.id == b.id && a.person == b.person && a.name == b.name && a.source == b.source && a.actions == b.actions
      && a.shut == b.shut && (a.thumb === b.thumb)
  }
}

/// The Faces tool's state, polled from the host (re-read when its revision moves).
@MainActor
final class FacesStore: ObservableObject {
  static let shared = FacesStore()

  /// shell::faces_status.
  enum Status: Int32 { case idle = 0, looking, ready, noPack, peopleOff, failed }
  /// shell::faces_op.
  enum Op: Int32 { case run = 0, toggle, chooseAll, chooseNone, people, retry, hover }

  @Published private(set) var armed = false
  @Published private(set) var status: Status = .idle
  @Published private(set) var faces: [FaceCard] = []
  @Published private(set) var chosen: Int32 = 0
  @Published private(set) var running = false
  @Published private(set) var runningFace: Int32 = -1
  @Published private(set) var runningAction: Int32 = 0

  private var revision: UInt32 = .max

  private init() {
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  var chosenCount: Int { (0..<faces.count).filter { isChosen($0) }.count }
  func isChosen(_ i: Int) -> Bool { i < 32 && chosen & (1 << Int32(i)) != 0 }
  /// "3 faces · 2 chosen".
  var summary: String {
    switch status {
    case .idle, .looking: return "Looking for faces…"
    case .noPack: return "Needs Local search"
    case .peopleOff: return "Needs People"
    case .failed: return "Could not read the faces"
    case .ready:
      if faces.isEmpty { return "No faces found" }
      return "\(faces.count == 1 ? "1 face" : "\(faces.count) faces") · \(chosenCount) chosen"
    }
  }

  private func poll() {
    var v = mv_faces_panel_view()
    guard mv_chrome_faces_panel(&v) else { return }
    if (v.armed != 0) != armed { armed = v.armed != 0 }
    guard v.revision != revision else { return }
    revision = v.revision
    let wasReady = status == .ready
    status = Status(rawValue: v.status) ?? .idle
    chosen = v.chosen
    running = v.running != 0
    runningFace = v.running_face
    runningAction = v.running_action
    var cards: [FaceCard] = []
    var nameBuf = [CChar](repeating: 0, count: 128)
    var sourceBuf = [CChar](repeating: 0, count: 160)
    var rgba = [UInt8](repeating: 0, count: 256 * 256 * 4)
    for i in 0..<Int(max(0, v.count)) {
      var c = mv_face_card_view()
      let ok = mv_chrome_face_card(Int32(i), &c, &nameBuf, Int32(nameBuf.count), &sourceBuf, Int32(sourceBuf.count),
                                   &rgba, Int32(rgba.count))
      guard ok else { continue }
      // Unchanged faces keep their image (no new NSImage per poll).
      let old = i < faces.count ? faces[i] : nil
      let name = String(cString: nameBuf), source = String(cString: sourceBuf)
      let thumb: NSImage?
      // The same read (a choice changed): the same pictures.
      if let old, wasReady, status == .ready, old.thumb != nil, old.person == c.person, old.name == name,
         old.shut == (c.shut != 0) {
        thumb = old.thumb
      } else {
        thumb = Self.image(rgba, Int(c.thumb_w), Int(c.thumb_h))
      }
      cards.append(FaceCard(id: i, person: c.person, name: name, source: source, actions: c.actions,
                            shut: c.shut != 0, thumb: thumb))
    }
    if cards != faces { faces = cards }
  }

  private static func image(_ rgba: [UInt8], _ w: Int, _ h: Int) -> NSImage? {
    guard w > 0, h > 0, rgba.count >= w * h * 4 else { return nil }
    let data = Data(rgba[0..<(w * h * 4)])
    guard let provider = CGDataProvider(data: data as CFData),
          let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                           space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    else { return nil }
    return NSImage(cgImage: cg, size: NSSize(width: w / 2, height: h / 2))
  }

  /// shell::pack_faces_op.
  private func send(_ op: Op, face: Int = -1, action: Int32 = 0) {
    mv_chrome_faces_op(op.rawValue * 100000 + Int32(face + 1) * 100 + (action + 1))
  }
  func run(_ a: FaceAction, face: Int = -1) { send(.run, face: face, action: a.rawValue) }
  func doAll() { send(.run, face: -1, action: -1) }
  func toggle(_ face: Int) { send(.toggle, face: face) }
  func chooseAll() { send(.chooseAll) }
  func chooseNone() { send(.chooseNone) }
  /// People lives in Local search's settings: open them there.
  func turnOnPeople() {
    send(.people)
    AddonNav.shared.show(AddonNav.localSearchPage)
  }
  /// Read the faces again (after a failure, or once People is on).
  func retry() { send(.retry) }
  /// The row under the pointer lights its box on the photo (-1 none).
  func hover(_ face: Int) { send(.hover, face: face) }
  func done() { EditStore.shared.toolAction(.done) }
}

/// The panel: the faces, their fixes and Do all.
struct FacesPanelView: View {
  @ObservedObject private var store = FacesStore.shared
  @ObservedObject private var edit = EditStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(10)) {
      header
      switch store.status {
      case .idle, .looking: looking
      case .noPack:
        EmptyFaces(symbol: "magnifyingglass", title: "Faces uses Local search",
                   text: "Local search's People finds who is who, and their other photos give the open eyes and better faces. Everything runs on this Mac.",
                   action: "Get Local search…", recheck: { store.retry() }) {
          AddonNav.shared.show(AddonNav.localSearchPage)
        }
      case .peopleOff:
        EmptyFaces(symbol: "person.crop.circle.badge.plus", title: "Turn on People",
                   text: "People in Local search finds each face's other photos on this Mac, so a fix can borrow their open eyes or a clearer face.",
                   action: "Turn on People…", recheck: { store.retry() }) { store.turnOnPeople() }
      case .failed:
        EmptyFaces(symbol: "exclamationmark.triangle", title: "Could not read this photo's faces",
                   text: "Try again, or check the photo opens.", action: "Try again") { store.retry() }
      case .ready:
        if store.faces.isEmpty {
          EmptyFaces(symbol: "face.dashed", title: "No faces found",
                     text: "Faces works on photos where People can see a face, front on or turned a little.",
                     action: nil) {}
        } else {
          list
          footer
        }
      }
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(10)).fill(MVTheme.surface2))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(10)).stroke(MVTheme.accent, lineWidth: 1.5))
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Faces, \(store.summary)")
  }

  private var header: some View {
    HStack(spacing: MVTheme.space(10)) {
      Image(systemName: "face.smiling")
        .font(.system(size: 15, weight: .medium))
        .foregroundStyle(MVTheme.accent)
        .frame(width: 30, height: 30)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.selectionWash))
      VStack(alignment: .leading, spacing: 1) {
        Text("Faces").font(MVTheme.font(15)).foregroundStyle(MVTheme.title)
          .accessibilityAddTraits(.isHeader)
        Text(store.summary).font(MVTheme.font(11)).foregroundStyle(MVTheme.body).lineLimit(1)
      }
      Spacer(minLength: MVTheme.space(4))
      Button("Done") { store.done() }
        .buttonStyle(AccentButtonStyle(filled: false))
        .help("Put the Faces tool down (Esc)")
    }
  }

  private var looking: some View {
    HStack(spacing: MVTheme.space(8)) {
      ProgressView().controlSize(.small)
      Text("Looking for faces in this photo…").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
    }
    .padding(.vertical, MVTheme.space(6))
  }

  private var list: some View {
    VStack(spacing: MVTheme.space(8)) {
      Text("Click a face's box on the photo, or its picture here, to choose it.")
        .font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
        .frame(maxWidth: .infinity, alignment: .leading)
        .fixedSize(horizontal: false, vertical: true)
      ForEach(store.faces) { face in
        FaceRow(face: face)
      }
    }
  }

  private var footer: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      HStack(spacing: MVTheme.space(6)) {
        Button {
          store.doAll()
        } label: {
          HStack(spacing: MVTheme.space(6)) {
            if store.running && store.runningAction == -1 {
              ProgressView().controlSize(.small)
              Text("Working…")
            } else {
              Image(systemName: "sparkles")
              Text(store.chosenCount == store.faces.count ? "Do all" : "Do all \(store.chosenCount) chosen")
            }
          }
        }
        .buttonStyle(AccentButtonStyle())
        .disabled(store.running || store.chosenCount == 0 || edit.eyesBusy)
        .help("Each chosen face's best fix, in one new file: a clearly better face of the same person, else open eyes where they are shut.")
        Spacer(minLength: 0)
        Button("All") { store.chooseAll() }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(store.running || store.chosenCount == store.faces.count)
          .help("Choose every face")
        Button("None") { store.chooseNone() }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(store.running || store.chosenCount == 0)
          .help("Leave every face out")
      }
      Text("Each fix writes a new file; this photo is never changed.")
        .font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
    }
  }
}

/// One face: its picture (a click chooses it), who it is, and its fixes.
private struct FaceRow: View {
  let face: FaceCard
  @ObservedObject private var store = FacesStore.shared
  @State private var hover = false

  private var chosen: Bool { store.isChosen(face.id) }
  private var busyHere: Bool { store.running && (store.runningFace == face.id || (store.runningFace < 0 && chosen)) }

  var body: some View {
    HStack(alignment: .top, spacing: MVTheme.space(10)) {
      Button { store.toggle(face.id) } label: { thumb }
        .buttonStyle(.plain)
        .disabled(store.running)
        .help(chosen ? "Chosen: click to leave this face out" : "Click to choose this face")
        .accessibilityLabel("Face \(face.id + 1), \(face.title)")
        .accessibilityAddTraits(chosen ? [.isSelected] : [])
      VStack(alignment: .leading, spacing: MVTheme.space(3)) {
        HStack(spacing: MVTheme.space(6)) {
          Text(face.title).font(MVTheme.font(14))
            .foregroundStyle(face.name.isEmpty ? MVTheme.body : MVTheme.title).lineLimit(1)
          if face.shut {
            Text("Eyes closed")
              .font(MVTheme.font(10)).foregroundStyle(MVTheme.accent)
              .padding(.horizontal, MVTheme.space(6)).padding(.vertical, 1)
              .background(Capsule().fill(MVTheme.selectionWash))
              .fixedSize()
          }
        }
        if !face.source.isEmpty {
          Text(face.source).font(MVTheme.font(11)).foregroundStyle(MVTheme.body).lineLimit(1)
        }
        if busyHere, let a = FaceAction(rawValue: store.runningAction) {
          HStack(spacing: MVTheme.space(6)) {
            ProgressView().controlSize(.mini)
            Text(a.busy).font(MVTheme.font(11)).foregroundStyle(MVTheme.body)
          }
        } else {
          ViewThatFits(in: .horizontal) {
            HStack(spacing: MVTheme.space(4)) { actions(compact: false) }
            HStack(spacing: MVTheme.space(4)) { actions(compact: true) }
          }
        }
      }
      Spacer(minLength: 0)
    }
    .padding(MVTheme.space(8))
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8))
      .fill(chosen ? MVTheme.selectionWash.opacity(0.55) : (hover ? MVTheme.hoverWash : Color.clear)))
    .onHover { inside in
      hover = inside
      store.hover(inside ? face.id : -1)  // its box on the photo lights up
    }
    .animation(.easeOut(duration: 0.12), value: chosen)
    .accessibilityElement(children: .contain)
  }

  private var thumb: some View {
    ZStack(alignment: .topLeading) {
      Group {
        if let image = face.thumb {
          Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fill)
        } else {
          Image(systemName: "person.fill").font(.system(size: 22)).foregroundStyle(MVTheme.body)
            .frame(maxWidth: .infinity, maxHeight: .infinity).background(MVTheme.surface2)
        }
      }
      .frame(width: 56, height: 56)
      .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(10)))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(10))
        .stroke(chosen ? MVTheme.accent : MVTheme.hairline, lineWidth: chosen ? 2 : 0.5))
      // The number the box on the photo wears.
      Text("\(face.id + 1)")
        .font(.system(size: 10, weight: .semibold)).monospacedDigit()
        .foregroundStyle(chosen ? MVTheme.onAccent : Color.white)
        .frame(width: 18, height: 18)
        .background(Circle().fill(chosen ? MVTheme.accent : Color.black.opacity(0.7)))
        .overlay(Circle().stroke(Color.white.opacity(chosen ? 0 : 0.9), lineWidth: 1))
        .offset(x: -5, y: -5)
    }
    .opacity(chosen || store.status != .ready ? 1 : 0.6)
  }

  @ViewBuilder
  private func actions(compact: Bool) -> some View {
    ForEach(FaceAction.allCases.filter { face.offers($0) }) { a in
      Button {
        store.run(a, face: face.id)
      } label: {
        HStack(spacing: MVTheme.space(4)) {
          Image(systemName: a.symbol)
          if !compact { Text(a.label) }
        }
      }
      .buttonStyle(FlatButtonStyle(compact: true))
      .disabled(store.running || EditStore.shared.eyesBusy)
      .help(a.help)
      .accessibilityLabel(a.label)
    }
  }
}

/// An empty state: what is missing and the one button that fixes it.
private struct EmptyFaces: View {
  let symbol: String
  let title: String
  let text: String
  let action: String?
  /// "Check again": a new read, once what was missing is fixed elsewhere.
  var recheck: (() -> Void)? = nil
  let run: () -> Void

  var body: some View {
    HStack(alignment: .top, spacing: MVTheme.space(10)) {
      Image(systemName: symbol).font(.system(size: 18)).foregroundStyle(MVTheme.accent)
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
        Text(title).font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        Text(text).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
        if let action {
          HStack(spacing: MVTheme.space(6)) {
            Button(action, action: run).buttonStyle(AccentButtonStyle())
            if let recheck {
              Button("Check again", action: recheck)
                .buttonStyle(FlatButtonStyle(compact: true))
                .help("Read this photo's faces again")
            }
          }
        }
      }
    }
    .padding(.vertical, MVTheme.space(4))
  }
}
