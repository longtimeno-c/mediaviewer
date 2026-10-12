// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens (docs/design/25 §10, PR 58): an add-on's view tree drawn with SwiftUI's
// own controls. The Windows twin is IslandHost.Screens.cs.
//
// The tree arrives checked by the core (src/addon/screen.h): every control and
// picture has a label, which becomes its VoiceOver name, and every control is a
// standard SwiftUI one, so it takes the keyboard, the theme (MVTheme tokens),
// increased contrast and text size like the rest of the chrome. A change the
// person makes goes back as an event, checked again by the core before the
// add-on's worker hears of it; the control keeps its new value until the
// add-on answers with a diff. A text field reports on Return and when it loses
// focus, never per key; a slider when it is let go.
import AppKit
import MVChromeBridge
import SwiftUI

// MARK: thumbnails by path

@MainActor
final class SlotThumbs: ObservableObject {
  static let shared = SlotThumbs()
  @Published private(set) var images: [String: NSImage] = [:]
  private var order: [String] = []
  private var failed: Set<String> = []
  private let limit = 600  // a few screens of a 2,000-tile grid

  private init() {
    mv_chrome_slots_set_thumb_callback { path, jpeg in
      guard let path else { return }
      let p = String(cString: path)
      let j = jpeg.map { String(cString: $0) }
      MainActor.assumeIsolated { SlotThumbs.shared.arrived(p, j) }
    }
  }

  func want(_ path: String) {
    guard images[path] == nil, !failed.contains(path) else { return }
    path.withCString { mv_chrome_slots_request_thumbnail($0) }
  }

  func drop(_ path: String) {
    guard images[path] == nil else { return }
    path.withCString { mv_chrome_slots_cancel_thumbnail($0) }
  }

  private func arrived(_ path: String, _ jpeg: String?) {
    guard let jpeg, let image = NSImage(contentsOfFile: jpeg) else {
      failed.insert(path)
      return
    }
    images[path] = image
    order.append(path)
    if order.count > limit {
      let gone = order.removeFirst()
      images[gone] = nil
    }
  }
}

private struct SlotThumbnail: View {
  let path: String
  let size: CGFloat
  let label: String
  @ObservedObject private var thumbs = SlotThumbs.shared

  var body: some View {
    Color.clear
      .frame(width: size, height: size)
      .overlay {
        if let image = thumbs.images[path] {
          Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
        } else {
          Rectangle().fill(MVTheme.surface)
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: 6))
      .onAppear { thumbs.want(path) }
      .onDisappear { thumbs.drop(path) }
      .accessibilityElement(children: .ignore)
      .accessibilityLabel(label)
      .accessibilityAddTraits(.isImage)
  }
}

// MARK: the renderer

struct ScreenNodeView: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int

  var body: some View {
    if node.hidden {
      EmptyView()
    } else {
      content
        .disabled(node.disabled)
        .help(node.string("help") ?? "")
    }
  }

  private func child(_ n: ScreenNode) -> ScreenNodeView {
    ScreenNodeView(node: n, address: address, revision: revision)
  }

  @ViewBuilder private var content: some View {
    switch node.type {
    case "stack":
      let spacing = CGFloat(node.int("spacing") ?? 8)
      if node.string("axis") == "horizontal" {
        HStack(alignment: .center, spacing: spacing) { ForEach(node.children) { child($0) } }
      } else {
        VStack(alignment: horizontalAlignment, spacing: spacing) { ForEach(node.children) { child($0) } }
          .frame(maxWidth: node.string("align") == "fill" ? .infinity : nil, alignment: .leading)
      }
    case "grid":
      LazyVGrid(columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: max(1, node.int("columns") ?? 2)),
                alignment: .leading, spacing: CGFloat(node.int("spacing") ?? 8)) {
        ForEach(node.children) { child($0) }
      }
    case "scroll":
      let height = node.int("height") ?? 0
      ScrollView(node.string("axis") == "horizontal" ? .horizontal : .vertical) {
        VStack(alignment: .leading, spacing: 8) { ForEach(node.children) { child($0) } }.padding(2)
      }
      .frame(maxHeight: height > 0 ? CGFloat(height) : .infinity)
    case "split":
      SplitNode(node: node, address: address, revision: revision)
    case "text":
      TextNode(node: node)
    case "image":
      ImageNode(node: node, address: address)
    case "thumbnail":
      SlotThumbnail(path: node.string("path") ?? "", size: CGFloat(node.int("size") ?? 96), label: node.label)
    case "thumbnail_grid":
      ThumbnailGridNode(node: node, address: address, revision: revision)
    case "list":
      ListNode(node: node, address: address, revision: revision)
    case "table":
      TableNode(node: node, address: address, revision: revision)
    case "button":
      Button {
        SlotStore.shared.send(address, node: node.id, kind: 1, value: nil)
      } label: {
        if let symbol = node.string("symbol") {
          Label(node.label, systemImage: slotSymbol(symbol))
        } else {
          Text(node.label)
        }
      }
      .buttonStyle(FlatButtonStyle(selected: node.string("style") == "primary", compact: true))
      .foregroundStyle(node.string("style") == "destructive" ? Color.red : MVTheme.title)
      .accessibilityLabel(node.label)
    case "toggle":
      ToggleNode(node: node, address: address, revision: revision)
    case "choice":
      ChoiceNode(node: node, address: address, revision: revision)
    case "slider":
      SliderNode(node: node, address: address, revision: revision)
    case "number":
      NumberNode(node: node, address: address, revision: revision)
    case "text_field":
      TextFieldNode(node: node, address: address, revision: revision)
    case "progress":
      let value = node.double("value") ?? -1
      Group {
        if value < 0 {
          ProgressView().controlSize(.small)
        } else {
          ProgressView(value: value).tint(MVTheme.accent)
        }
      }
      .accessibilityLabel(node.label)
    case "separator":
      Rectangle().fill(MVTheme.hairline).frame(height: 1).accessibilityHidden(true)
    case "surface":
      SurfaceNode(node: node, address: address, revision: revision)
    default:
      EmptyView()
    }
  }

  private var horizontalAlignment: HorizontalAlignment {
    switch node.string("align") {
    case "center": return .center
    case "end": return .trailing
    default: return .leading
    }
  }
}

private struct SplitNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int

  var body: some View {
    let fraction = node.double("fraction") ?? 0.5
    GeometryReader { geo in
      if node.string("axis") == "vertical" {
        VSplitView {
          part(0).frame(minHeight: 60, idealHeight: geo.size.height * fraction)
          part(1).frame(minHeight: 60)
        }
      } else {
        HSplitView {
          part(0).frame(minWidth: 120, idealWidth: geo.size.width * fraction)
          part(1).frame(minWidth: 120)
        }
      }
    }
  }

  @ViewBuilder private func part(_ i: Int) -> some View {
    if node.children.indices.contains(i) {
      ScreenNodeView(node: node.children[i], address: address, revision: revision)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
  }
}

private struct TextNode: View {
  let node: ScreenNode

  var body: some View {
    let style = node.string("style") ?? "body"
    let lines = node.int("lines") ?? 0
    let text = Text(node.string("text") ?? "")
      .font(font(style))
      .foregroundStyle(style == "secondary" || style == "caption" ? MVTheme.body : MVTheme.title)
      .lineLimit(lines > 0 ? lines : nil)
      .frame(maxWidth: .infinity, alignment: .leading)
    if node.bool("copyable") {
      text.textSelection(.enabled)
    } else {
      text
    }
  }

  private func font(_ style: String) -> Font {
    switch style {
    case "title": return MVTheme.font(15)
    case "heading": return MVTheme.font(17)
    case "secondary": return MVTheme.font(12)
    case "caption": return MVTheme.font(11)
    case "mono": return .system(.caption, design: .monospaced)
    default: return MVTheme.font(14)
    }
  }
}

private struct ImageNode: View {
  let node: ScreenNode
  let address: ScreenAddress

  var body: some View {
    let w = CGFloat(node.int("width") ?? 0)
    let h = CGFloat(node.int("height") ?? 0)
    Group {
      if let url = SlotStore.shared.imageURL(addon: address.addon, src: node.string("src") ?? ""),
         let image = NSImage(contentsOf: url) {
        Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
      } else {
        Rectangle().fill(MVTheme.surface)
      }
    }
    .frame(width: w > 0 ? w : nil, height: h > 0 ? h : nil)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(node.label)
    .accessibilityAddTraits(.isImage)
  }
}

/// The chrome draws the pin, in the accent colour, over the add-on's pixels
/// (docs/plans/metadata-search-and-place.md slice 3). The whole surface is one
/// element, named by its label.
///
/// An "interactive" surface (owner, 2026-10-12: zoom the map with the trackpad
/// or the mouse) reports a pinch or a mouse wheel as a zoom (scale and anchor),
/// a drag as a pan and a double-click, each to the add-on's worker once the
/// gesture ends. A trackpad two-finger scroll is not taken: the pane scrolls.
/// While a gesture runs the current pixels are scaled / moved at once, until
/// the add-on posts new ones. None of this presents the canvas.
private struct SurfaceNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var dropped = false
  // The provisional transform, reset when the add-on's next pixels arrive.
  @State private var scale: CGFloat = 1
  @State private var committedScale: CGFloat = 1
  @State private var committedOffset: CGSize = .zero
  @State private var anchor: UnitPoint = .center
  @State private var offset: CGSize = .zero
  @State private var shownRevision: UInt64 = 0
  @State private var hover: CGPoint?
  @State private var wheelMonitor: Any?
  @State private var wheelFlush: DispatchWorkItem?

  private var live: Bool {
    (node.props["interactive"] is [String: Any] || node.bool("interactive")) && !node.disabled
  }
  // The live zoom stays within these, relative to the posted pixels.
  private var bounds: ClosedRange<CGFloat> {
    let b = node.props["interactive"] as? [String: Any]
    let lo = (b?["min_scale"] as? NSNumber)?.doubleValue ?? 0.05
    let hi = (b?["max_scale"] as? NSNumber)?.doubleValue ?? 20
    return CGFloat(lo)...CGFloat(max(lo, hi))
  }
  private func bounded(_ s: CGFloat) -> CGFloat { min(bounds.upperBound, max(bounds.lowerBound, s)) }
  private var pinPoint: CGPoint? {
    guard let pin = node.props["pin"] as? [String: Any], let x = (pin["x"] as? NSNumber)?.doubleValue,
          let y = (pin["y"] as? NSNumber)?.doubleValue else { return nil }
    return CGPoint(x: x, y: y)
  }

  var body: some View {
    let height = CGFloat(node.int("height") ?? 120)
    let width = CGFloat(node.int("width") ?? 0)
    let pixels = SlotStore.surfacePixels(address, surface: node.string("surface") ?? "")
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        if let pixels {
          Image(decorative: pixels.image, scale: CGFloat(SlotStore.scale)).resizable()
        } else {
          Rectangle().fill(MVTheme.surface)
        }
        // The add-on's names, drawn by the chrome so the text is native.
        SurfaceMarks(marks: node.rows("marks"), pin: pinPoint, size: geo.size)
        if let pin = node.props["pin"] as? [String: Any], let x = (pin["x"] as? NSNumber)?.doubleValue,
           let y = (pin["y"] as? NSNumber)?.doubleValue {
          Image(systemName: "mappin.circle.fill")
            .font(.system(size: 22))
            .foregroundStyle(.white, MVTheme.accent)
            .shadow(color: .black.opacity(0.35), radius: 3, y: 2)
            .position(x: geo.size.width * x, y: geo.size.height * y - (dropped || reduceMotion ? 11 : 30))
            .opacity(dropped || reduceMotion ? 1 : 0)
        }
      }
      .scaleEffect(scale, anchor: anchor)
      .offset(offset)
      .contentShape(Rectangle())
      .gesture(live ? magnify(geo.size) : nil)
      .simultaneousGesture(live ? drag(geo.size) : nil)
      .onTapGesture(count: 2, coordinateSpace: .local) { at in
        guard live, geo.size.width > 0, geo.size.height > 0 else { return }
        send(12, ["x": clamp(at.x / geo.size.width), "y": clamp(at.y / geo.size.height)])
      }
      .onContinuousHover { phase in
        guard live else { return }
        switch phase {
        case .active(let at):
          hover = CGPoint(x: at.x / max(1, geo.size.width), y: at.y / max(1, geo.size.height))
          installWheel()
        case .ended:
          hover = nil
          removeWheel()
        }
      }
      .onChange(of: pixels?.revision ?? 0) { _, now in
        // The add-on's answer: its pixels replace the provisional transform.
        guard now != shownRevision else { return }
        shownRevision = now
        scale = 1
        offset = .zero
        committedScale = 1
        committedOffset = .zero
      }
    }
    .frame(width: width > 0 ? width : nil, height: height)
    .frame(maxWidth: width > 0 ? nil : .infinity)
    .clipShape(RoundedRectangle(cornerRadius: 6))
    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(MVTheme.hairline, lineWidth: 1))
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(node.label)
    .accessibilityAddTraits(.isImage)
    .onAppear {
      guard !dropped else { return }
      if reduceMotion {
        dropped = true
      } else {
        withAnimation(.spring(response: 0.35, dampingFraction: 0.6)) { dropped = true }
      }
    }
    .onDisappear { removeWheel() }
  }

  private func clamp(_ v: CGFloat) -> Double { Double(min(1, max(0, v))) }

  private func send(_ kind: UInt32, _ value: [String: Double]) {
    guard let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]),
          let text = String(data: data, encoding: .utf8) else { return }
    _ = address.addon.withCString { a in
      address.screen.withCString { s in node.id.withCString { n in text.withCString { mv_chrome_slots_event(a, s, n, kind, $0) } } }
    }
  }

  // A trackpad pinch: shown at once, reported when it ends.
  private func magnify(_ size: CGSize) -> some Gesture {
    MagnifyGesture()
      .onChanged { v in
        anchor = v.startAnchor
        scale = bounded(committedScale * v.magnification)
      }
      .onEnded { v in
        let step = bounded(committedScale * v.magnification) / committedScale
        committedScale = scale
        send(10, ["scale": Double(max(0.05, min(20, step))), "x": clamp(v.startAnchor.x),
                  "y": clamp(v.startAnchor.y)])
      }
  }

  // A drag: the pixels follow, the move is reported in surface widths / heights.
  private func drag(_ size: CGSize) -> some Gesture {
    DragGesture(minimumDistance: 3)
      .onChanged { v in
        offset = CGSize(width: committedOffset.width + v.translation.width,
                        height: committedOffset.height + v.translation.height)
      }
      .onEnded { v in
        committedOffset = offset
        guard size.width > 0, size.height > 0 else { return }
        send(11, ["dx": Double(max(-10, min(10, v.translation.width / size.width))),
                  "dy": Double(max(-10, min(10, v.translation.height / size.height)))])
      }
  }

  // A mouse wheel (no precise deltas) zooms about the pointer; a trackpad's
  // two-finger scroll (precise deltas) is left for the pane. Steps arriving
  // together are one zoom, reported 150 ms after the last.
  private func installWheel() {
    guard wheelMonitor == nil else { return }
    wheelMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
      guard !event.hasPreciseScrollingDeltas, let at = hover else { return event }
      MainActor.assumeIsolated {
        anchor = UnitPoint(x: at.x, y: at.y)
        let before = committedScale
        scale = bounded(scale * pow(1.1, event.scrollingDeltaY))
        wheelFlush?.cancel()
        let work = DispatchWorkItem { [at] in
          MainActor.assumeIsolated {
            let step = scale / before
            committedScale = scale
            send(10, ["scale": Double(max(0.05, min(20, step))), "x": clamp(at.x), "y": clamp(at.y)])
          }
        }
        wheelFlush = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15, execute: work)
      }
      return nil
    }
  }

  private func removeWheel() {
    if let m = wheelMonitor { NSEvent.removeMonitor(m) }
    wheelMonitor = nil
  }
}

private struct ToggleNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var on = false

  var body: some View {
    Toggle(node.label, isOn: Binding(get: { on }, set: { value in
      on = value
      SlotStore.shared.send(address, node: node.id, kind: 2, value: value)
    }))
    .toggleStyle(.switch)
    .tint(MVTheme.accent)
    .font(MVTheme.font(14))
    .onAppear { on = node.bool("value") }
    .onChange(of: revision) { _, _ in on = node.bool("value") }
  }
}

private struct ChoiceNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var value = ""

  var body: some View {
    Picker(node.label, selection: Binding(get: { value }, set: { v in
      value = v
      SlotStore.shared.send(address, node: node.id, kind: 2, value: v)
    })) {
      ForEach(node.rows("options").compactMap { $0["id"] as? String }, id: \.self) { id in
        Text(node.rows("options").first { ($0["id"] as? String) == id }?["text"] as? String ?? id).tag(id)
      }
    }
    .font(MVTheme.font(14))
    .onAppear { value = node.string("value") ?? "" }
    .onChange(of: revision) { _, _ in value = node.string("value") ?? "" }
  }
}

private struct SliderNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var value = 0.0

  var body: some View {
    let lo = node.double("min") ?? 0
    let hi = max(lo + 1e-9, node.double("max") ?? 1)
    VStack(alignment: .leading, spacing: 2) {
      Text("\(node.label): \(format(value))").font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      Group {
        if let step = node.double("step"), step > 0 {
          Slider(value: $value, in: lo...hi, step: step) { Text(node.label) } onEditingChanged: { editing in
            if !editing { SlotStore.shared.send(address, node: node.id, kind: 2, value: value) }
          }
        } else {
          Slider(value: $value, in: lo...hi) { Text(node.label) } onEditingChanged: { editing in
            if !editing { SlotStore.shared.send(address, node: node.id, kind: 2, value: value) }
          }
        }
      }
      .labelsHidden()
      .tint(MVTheme.accent)
      .accessibilityLabel(node.label)
      .accessibilityValue(format(value))
    }
    .onAppear { value = node.double("value") ?? lo }
    .onChange(of: revision) { _, _ in value = node.double("value") ?? lo }
  }

  private func format(_ v: Double) -> String { v == v.rounded() ? String(Int(v)) : String(format: "%.2f", v) }
}

private struct NumberNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var value = 0.0

  var body: some View {
    let lo = node.double("min") ?? -1e12
    let hi = node.double("max") ?? 1e12
    let step = node.double("step") ?? 1
    Stepper(value: Binding(get: { value }, set: { v in
      value = min(hi, max(lo, v))
      SlotStore.shared.send(address, node: node.id, kind: 2, value: value)
    }), in: lo...hi, step: step) {
      Text("\(node.label): \(value == value.rounded() ? String(Int(value)) : String(value))")
        .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
    }
    .accessibilityLabel(node.label)
    .accessibilityValue(String(value))
    .onAppear { value = node.double("value") ?? max(lo, 0) }
    .onChange(of: revision) { _, _ in value = node.double("value") ?? max(lo, 0) }
  }
}

private struct TextFieldNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var text = ""
  @FocusState private var focused: Bool

  var body: some View {
    Group {
      if node.bool("multiline") {
        TextField(node.string("placeholder") ?? node.label, text: $text, axis: .vertical).lineLimit(3...8)
      } else {
        TextField(node.string("placeholder") ?? node.label, text: $text)
      }
    }
    .textFieldStyle(.roundedBorder)
    .font(MVTheme.font(14))
    .focused($focused)
    .onSubmit { SlotStore.shared.send(address, node: node.id, kind: 3, value: text) }
    .onChange(of: focused) { _, now in
      if !now, text != (node.string("value") ?? "") {
        SlotStore.shared.send(address, node: node.id, kind: 2, value: text)
      }
    }
    .accessibilityLabel(node.label)
    .onAppear { text = node.string("value") ?? "" }
    .onChange(of: revision) { _, _ in if !focused { text = node.string("value") ?? "" } }
  }
}

private struct ListNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var selection: Set<String> = []

  var body: some View {
    let rows = node.rows("items")
    List(selection: Binding(get: { selection }, set: { s in
      selection = node.bool("multi") ? s : Set(s.prefix(1))
      SlotStore.shared.send(address, node: node.id, kind: 4, value: Array(selection).sorted())
    })) {
      ForEach(rows.indices, id: \.self) { i in
        let id = rows[i]["id"] as? String ?? ""
        VStack(alignment: .leading, spacing: 2) {
          Text(rows[i]["text"] as? String ?? "").font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
          if let detail = rows[i]["detail"] as? String {
            Text(detail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          }
        }
        .tag(id)
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { SlotStore.shared.send(address, node: node.id, kind: 5, value: id) }
      }
    }
    .listStyle(.plain)
    .scrollContentBackground(.hidden)
    .frame(minHeight: 80)
    .onKeyPress(.return) {
      guard let first = selection.first else { return .ignored }
      SlotStore.shared.send(address, node: node.id, kind: 5, value: first)
      return .handled
    }
    .accessibilityLabel(node.label)
    .onAppear { selection = Set(node.props["selection"] as? [String] ?? []) }
    .onChange(of: revision) { _, _ in selection = Set(node.props["selection"] as? [String] ?? []) }
  }
}

private struct TableNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var selection: Set<String> = []

  var body: some View {
    let columns = node.rows("columns")
    let rows = node.rows("rows")
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: 8) {
        ForEach(columns.indices, id: \.self) { c in
          Text(columns[c]["title"] as? String ?? "").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      .padding(.horizontal, 8)
      .padding(.vertical, 4)
      .accessibilityAddTraits(.isHeader)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      List(selection: Binding(get: { selection }, set: { s in
        selection = node.bool("multi") ? s : Set(s.prefix(1))
        SlotStore.shared.send(address, node: node.id, kind: 4, value: Array(selection).sorted())
      })) {
        ForEach(rows.indices, id: \.self) { r in
          let id = rows[r]["id"] as? String ?? ""
          let cells = rows[r]["cells"] as? [String] ?? []
          HStack(spacing: 8) {
            ForEach(columns.indices, id: \.self) { c in
              Text(c < cells.count ? cells[c] : "").font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
          }
          .tag(id)
          .accessibilityElement(children: .combine)
          .contentShape(Rectangle())
          .onTapGesture(count: 2) { SlotStore.shared.send(address, node: node.id, kind: 5, value: id) }
        }
      }
      .listStyle(.plain)
      .scrollContentBackground(.hidden)
      .frame(minHeight: 80)
      .onKeyPress(.return) {
        guard let first = selection.first else { return .ignored }
        SlotStore.shared.send(address, node: node.id, kind: 5, value: first)
        return .handled
      }
    }
    .accessibilityLabel(node.label)
    .onAppear { selection = Set(node.props["selection"] as? [String] ?? []) }
    .onChange(of: revision) { _, _ in selection = Set(node.props["selection"] as? [String] ?? []) }
  }
}

/// A virtualised grid: LazyVGrid builds only the tiles on screen, and each
/// asks for its thumbnail as it appears and cancels as it leaves. Arrow keys
/// move a cursor, Space selects, Return activates.
private struct ThumbnailGridNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var selection: Set<String> = []
  @State private var cursor = 0
  @State private var columns = 1
  @FocusState private var focused: Bool

  var body: some View {
    let tile = CGFloat(node.int("tile") ?? 128)
    let items = node.rows("items")
    GeometryReader { geo in
      let cols = max(1, Int((geo.size.width - 16 + 8) / (tile + 8)))
      ScrollViewReader { proxy in
        ScrollView {
          LazyVGrid(columns: Array(repeating: GridItem(.fixed(tile), spacing: 8), count: cols), spacing: 8) {
            ForEach(items.indices, id: \.self) { i in
              let path = items[i]["path"] as? String ?? ""
              let name = items[i]["label"] as? String ?? (path as NSString).lastPathComponent
              SlotThumbnail(path: path, size: tile, label: name)
                .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(
                  selection.contains(path) ? MVTheme.accent : (focused && i == cursor ? MVTheme.hairline : .clear),
                  lineWidth: 2))
                .id(i)
                .onTapGesture(count: 2) { SlotStore.shared.send(address, node: node.id, kind: 5, value: path) }
                .onTapGesture {
                  cursor = i
                  select(path, items: items)
                }
                .accessibilityAddTraits(selection.contains(path) ? [.isButton, .isSelected] : .isButton)
            }
          }
          .padding(8)
        }
        .focusable()
        .focused($focused)
        .onKeyPress(keys: [.leftArrow, .rightArrow, .upArrow, .downArrow]) { press in
          guard !items.isEmpty else { return .ignored }
          let step = press.key == .leftArrow ? -1 : press.key == .rightArrow ? 1 : press.key == .upArrow ? -cols : cols
          cursor = min(items.count - 1, max(0, cursor + step))
          proxy.scrollTo(cursor)
          return .handled
        }
        .onKeyPress(.space) {
          guard items.indices.contains(cursor), let path = items[cursor]["path"] as? String else { return .ignored }
          select(path, items: items)
          return .handled
        }
        .onKeyPress(.return) {
          guard items.indices.contains(cursor), let path = items[cursor]["path"] as? String else { return .ignored }
          SlotStore.shared.send(address, node: node.id, kind: 5, value: path)
          return .handled
        }
      }
      .onAppear { columns = cols }
    }
    .frame(minHeight: tile + 16)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(node.label)
    .onAppear { selection = Set(node.props["selection"] as? [String] ?? []) }
    .onChange(of: revision) { _, _ in selection = Set(node.props["selection"] as? [String] ?? []) }
  }

  private func select(_ path: String, items: [[String: Any]]) {
    if node.bool("multi") {
      if selection.contains(path) { selection.remove(path) } else { selection.insert(path) }
    } else {
      selection = [path]
    }
    SlotStore.shared.send(address, node: node.id, kind: 4, value: Array(selection).sorted())
  }
}

// MARK: info-pane sections

/// The add-ons' sections at the foot of the Metadata pane's summary (docs/design/25
/// PR 59): a heading per section and its tree. Absent when none answered.
struct AddonInfoSections: View {
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    if !slots.sections.isEmpty {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(slots.sections) { section in
          Rectangle().fill(MVTheme.hairline).frame(height: 1)
          VStack(alignment: .leading, spacing: 6) {
            Text(section.title).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
              .accessibilityAddTraits(.isHeader)
            ScreenNodeView(node: section.tree, address: ScreenAddress(addon: section.addon, screen: section.section),
                           revision: section.revision)
          }
          .padding(14)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }
}

// MARK: windows, panes and sheets

/// The window, docked pane or sheet of one add-on screen.
struct SlotScreenRoot: View {
  let address: ScreenAddress
  let place: String
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    let screen = slots.screens.first { $0.addon == address.addon && $0.screen == address.screen }
    VStack(alignment: .leading, spacing: 0) {
      if place != "window" {
        HStack {
          Text(screen?.title ?? "").font(MVTheme.font(15)).foregroundStyle(MVTheme.title)
            .accessibilityAddTraits(.isHeader)
          Spacer()
          Button("Done") { slots.close(address) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .keyboardShortcut(.cancelAction)
        }
        .padding(12)
        Rectangle().fill(MVTheme.hairline).frame(height: 1)
      }
      if let tree = screen?.tree {
        ScreenNodeView(node: tree, address: address, revision: screen?.revision ?? 0)
          .padding(12)
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .background(MVTheme.canvas)
    .onExitCommand { slots.close(address) }
  }
}

/// Opens, keeps and closes the AppKit windows the screens live in.
@MainActor
final class SlotWindows: NSObject, NSWindowDelegate {
  static let shared = SlotWindows()
  private var windows: [ScreenAddress: NSWindow] = [:]
  private var closingFromCore: Set<ScreenAddress> = []

  func sync(_ screens: [SlotScreen]) {
    let wanted = Set(screens.map { ScreenAddress(addon: $0.addon, screen: $0.screen) })
    for (address, window) in windows where !wanted.contains(address) {
      closingFromCore.insert(address)
      close(window)
      windows[address] = nil
      closingFromCore.remove(address)
    }
    for s in screens {
      let address = ScreenAddress(addon: s.addon, screen: s.screen)
      // An open one stays where the person left it: a diff or a surface post
      // (a progress bar, a map redrawing) never takes focus from the viewer.
      if windows[address] != nil { continue }
      open(s, address)
    }
  }

  private func open(_ s: SlotScreen, _ address: ScreenAddress) {
    let host = NSHostingView(rootView: ThemedRoot(content: SlotScreenRoot(address: address, place: s.place)))
    let main = NSApp.mainWindow ?? NSApp.windows.first { $0.isVisible && !($0 is NSPanel) }
    switch s.place {
    case "sheet":
      let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: s.width > 0 ? s.width : 520,
                                           height: s.height > 0 ? s.height : 420),
                       styleMask: [.titled, .resizable], backing: .buffered, defer: false)
      w.contentView = host
      w.title = s.title
      w.isReleasedWhenClosed = false  // held in `windows`; close() must not free it
      windows[address] = w
      if let main {
        main.beginSheet(w) { _ in }
      } else {
        w.delegate = self
        w.makeKeyAndOrderFront(nil)
      }
    case "pane":
      // Docked to the right edge of the viewer window: a child panel that
      // moves with it.
      let width = s.width > 0 ? s.width : 320
      let frame = main.map { NSRect(x: $0.frame.maxX - width, y: $0.frame.minY, width: width,
                                    height: $0.contentLayoutRect.height) }
        ?? NSRect(x: 0, y: 0, width: width, height: 600)
      let p = NSPanel(contentRect: frame, styleMask: [.titled, .closable, .utilityWindow, .resizable],
                      backing: .buffered, defer: false)
      p.title = s.title
      p.contentView = host
      p.isReleasedWhenClosed = false
      p.isFloatingPanel = false
      p.becomesKeyOnlyIfNeeded = false
      p.delegate = self
      windows[address] = p
      main?.addChildWindow(p, ordered: .above)
      p.makeKeyAndOrderFront(nil)
    default:
      let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: s.width > 0 ? s.width : 720,
                                           height: s.height > 0 ? s.height : 520),
                       styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
      w.title = s.title
      w.contentView = host
      w.isReleasedWhenClosed = false
      w.delegate = self
      w.center()
      windows[address] = w
      w.makeKeyAndOrderFront(nil)
    }
  }

  func bringForward(_ address: ScreenAddress) {
    guard let w = windows[address], w.sheetParent == nil else { return }
    w.makeKeyAndOrderFront(nil)
  }

  private func close(_ window: NSWindow) {
    if let parent = window.sheetParent {
      parent.endSheet(window)
    } else {
      window.parent?.removeChildWindow(window)
      window.close()
    }
  }

  /// The person closed it (the close button, ⌘W): tell the core, which tells
  /// the add-on.
  func windowWillClose(_ notification: Notification) {
    guard let window = notification.object as? NSWindow,
          let address = windows.first(where: { $0.value === window })?.key,
          !closingFromCore.contains(address)
    else { return }
    windows[address] = nil
    SlotStore.shared.close(address)
  }
}
