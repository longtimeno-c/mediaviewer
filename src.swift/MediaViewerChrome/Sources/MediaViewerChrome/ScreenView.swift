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
//
// An add-on's own window, pane or sheet (#508) draws in the system face (the
// theme's when it names one) on the ramp Windows uses, with filled buttons and
// a switch on the right of its label; a section in the info pane keeps the
// chrome's face, among the chrome's own text. A new revision keeps each
// scroller's offset and the focused control by itself: every node is built
// under its id (ForEach over ScreenNode.id), so SwiftUI keeps their state.
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
  @Environment(\.screenWindow) private var window

  var body: some View {
    Color.clear
      .frame(width: size, height: size)
      .overlay {
        if let image = thumbs.images[path] {
          Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
        } else {
          // #508: on a card (itself the surface colour) an empty or loading
          // tile still shows as a tile.
          Rectangle().fill(window ? MVTheme.hairline : MVTheme.surface)
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
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

  @MainActor @ViewBuilder private var content: some View {
    switch node.type {
    case "stack":
      let spacing = CGFloat(node.int("spacing") ?? 8)
      Group {
        if node.string("axis") == "horizontal" {
          HStack(alignment: .center, spacing: spacing) { ForEach(node.children) { child($0) } }
        } else {
          VStack(alignment: horizontalAlignment, spacing: spacing) { ForEach(node.children) { child($0) } }
            .frame(maxWidth: node.string("align") == "fill" || node.string("look") == "card" ? .infinity : nil,
                   alignment: node.string("align") == "center" ? .center : .leading)
            // Text in a centred stack is centred too (#442's code and accept cards).
            .environment(\.screenCentered, node.string("align") == "center")
        }
      }
      // "look": "card" (#442): a rounded surface, as Settings' cards.
      .modifier(CardLook(on: node.string("look") == "card"))
    case "grid":
      LazyVGrid(columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: max(1, node.int("columns") ?? 2)),
                alignment: .leading, spacing: CGFloat(node.int("spacing") ?? 8)) {
        ForEach(node.children) { child($0) }
      }
    case "scroll":
      ScrollNode(node: node, address: address, revision: revision)
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
      ButtonNode(node: node, address: address)
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
        if node.string("look") == "ring" {
          // #442: a ring that fills, its percentage in the middle.
          ScreenRing(value: value, size: CGFloat(node.int("size") ?? 64))
        } else if node.string("look") == "glow" {
          // Contribution API 9: the glow colours, a shimmer while it moves.
          GlowBar(value: value)
        } else if value < 0 {
          ProgressView().controlSize(.small)
        } else {
          ProgressView(value: value).tint(MVTheme.accent)
        }
      }
      .accessibilityLabel(node.label)
      .accessibilityValue(value < 0 ? "In progress" : "\(Int((value * 100).rounded())) percent")
    case "separator":
      Rectangle().fill(MVTheme.hairline).frame(height: 1).accessibilityHidden(true)
    case "surface":
      SurfaceNode(node: node, address: address, revision: revision)
    case "tiles":
      TilesNode(node: node, address: address, revision: revision)
    case "link":
      LinkNode(node: node)
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

private struct ScreenCenteredKey: EnvironmentKey {
  static let defaultValue = false
}

private struct ScreenWindowKey: EnvironmentKey {
  static let defaultValue = false
}

private struct ScreenRootInsetKey: EnvironmentKey {
  static let defaultValue: EdgeInsets? = nil
}

extension EnvironmentValues {
  /// Inside a vertical stack with "align": "center".
  var screenCentered: Bool {
    get { self[ScreenCenteredKey.self] }
    set { self[ScreenCenteredKey.self] = newValue }
  }

  /// #508: drawn in an add-on's own window, pane or sheet, not the info pane.
  var screenWindow: Bool {
    get { self[ScreenWindowKey.self] }
    set { self[ScreenWindowKey.self] = newValue }
  }

  /// #508: the window's margin, for a tree whose root is a scroller to put
  /// inside it (so the bar sits on the window's edge); nil below the root.
  var screenRootInset: EdgeInsets? {
    get { self[ScreenRootInsetKey.self] }
    set { self[ScreenRootInsetKey.self] = newValue }
  }
}

/// #508: a screen's face. In a window, the system's (the theme's `font` when it
/// names one installed here) at `size`; in the info pane, the chrome's face, as
/// before, without a weight.
@MainActor
func screenFont(_ size: CGFloat, _ weight: Font.Weight = .regular, window: Bool) -> Font {
  guard window else { return MVTheme.font(size) }
  if MVTheme.family != nil { return MVTheme.font(size).weight(weight) }
  return .system(size: size * MVTheme.typeScale, weight: weight)
}

/// How tall a root scroller's content is, for a window to open no taller.
private struct ScreenContentHeight: PreferenceKey {
  static let defaultValue: CGFloat = 0
  static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

private struct ScrollNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @Environment(\.screenRootInset) private var rootInset

  var body: some View {
    let height = node.int("height") ?? 0
    ScrollView(node.string("axis") == "horizontal" ? .horizontal : .vertical) {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        ForEach(node.children) { ScreenNodeView(node: $0, address: address, revision: revision) }
      }
      .padding(rootInset ?? EdgeInsets(top: MVTheme.space(2), leading: MVTheme.space(2),
                                       bottom: MVTheme.space(2), trailing: MVTheme.space(2)))
      .background {
        if rootInset != nil {
          GeometryReader { g in Color.clear.preference(key: ScreenContentHeight.self, value: g.size.height) }
        }
      }
      .environment(\.screenRootInset, nil)
    }
    .frame(maxHeight: height > 0 ? CGFloat(height) : .infinity)
  }
}

@MainActor
private struct TextNode: View {
  let node: ScreenNode
  @Environment(\.screenCentered) private var centered
  @Environment(\.screenWindow) private var window

  var body: some View {
    let style = node.string("style") ?? "body"
    let lines = node.int("lines") ?? 0
    let text = Text(node.string("text") ?? "")
      .font(font(style))
      .foregroundStyle(style == "secondary" || style == "caption" ? MVTheme.body : MVTheme.title)
      .lineLimit(lines > 0 ? lines : nil)
      .multilineTextAlignment(centered ? .center : .leading)
      .frame(maxWidth: .infinity, alignment: centered ? .center : .leading)
      .accessibilityAddTraits(style == "heading" ? .isHeader : [])
    if node.bool("copyable") {
      text.textSelection(.enabled)
    } else {
      text
    }
  }

  private func font(_ style: String) -> Font {
    switch style {
    case "mono": return .system(.caption, design: .monospaced)
    // #442: a short code read off the screen across a room.
    case "display": return .system(size: 34, weight: .semibold, design: .rounded).monospacedDigit()
    default: break
    }
    // #508: in a window, the ramp Windows uses (IslandHost.ScreenLook.cs):
    // body 14, title 14 semibold, heading 17 semibold, secondary 13, caption 12.
    if window {
      switch style {
      case "title": return screenFont(14, .semibold, window: true)
      case "heading": return screenFont(17, .semibold, window: true)
      case "secondary": return screenFont(13, window: true)
      case "caption": return screenFont(12, window: true)
      default: return screenFont(14, window: true)
      }
    }
    switch style {
    case "title": return MVTheme.font(15)
    case "heading": return MVTheme.font(17)
    case "secondary": return MVTheme.font(12)
    case "caption": return MVTheme.font(11)
    default: return MVTheme.font(14)
    }
  }
}

/// #508: a screen window's button, filled as Settings' are (the accent with
/// white ink for "primary", red text for "destructive"); in the info pane, the
/// chrome's flat button.
struct ScreenButtonStyle: ButtonStyle {
  var style = "plain"
  var window = true

  @ViewBuilder func makeBody(configuration: Configuration) -> some View {
    if window {
      ScreenButtonBody(configuration: configuration, style: style)
    } else {
      FlatButtonStyle(selected: style == "primary", compact: true).makeBody(configuration: configuration)
        .foregroundStyle(style == "destructive" ? Color.red : MVTheme.title)
    }
  }

  private struct ScreenButtonBody: View {
    let configuration: Configuration
    let style: String
    @State private var hover = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
      let primary = style == "primary"
      let wash = configuration.isPressed ? 0.16 : (hover && enabled ? 0.11 : 0.08)
      configuration.label
        .font(screenFont(14, window: true))
        .foregroundStyle(primary ? Color.white : style == "destructive" ? Color(nsColor: .systemRed) : MVTheme.title)
        .padding(.horizontal, MVTheme.space(12)).padding(.vertical, MVTheme.space(6))
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(
          primary ? AnyShapeStyle(MVTheme.accent.opacity(configuration.isPressed ? 0.8 : 1))
                  : AnyShapeStyle(Color.primary.opacity(wash))))
        .opacity(enabled ? 1 : 0.5)
        .contentShape(Rectangle())
        .onHover { hover = $0 }
    }
  }
}

// MARK: #442 additions: card, ring, tiles, code entry

/// "look": "card" on a stack: padding on a rounded surface with a hairline.
private struct CardLook: ViewModifier {
  let on: Bool
  func body(content: Content) -> some View {
    if on {
      content
        .padding(MVTheme.space(14))
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(10)).fill(MVTheme.surface))
        .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(10)).stroke(MVTheme.hairline, lineWidth: 1))
    } else {
      content
    }
  }
}

/// A progress ring: the track in the hairline colour, the fill in the accent;
/// indeterminate (value < 0) is a still third, never a spinning animation that
/// would tick while nothing moves.
private struct ScreenRing: View {
  let value: Double
  let size: CGFloat

  var body: some View {
    let line = max(3, size / 14)
    ZStack {
      Circle().stroke(MVTheme.hairline, lineWidth: line)
      Circle()
        .trim(from: 0, to: value < 0 ? 0.33 : min(1, max(0, value)))
        .stroke(MVTheme.accent, style: StrokeStyle(lineWidth: line, lineCap: .round))
        .rotationEffect(.degrees(-90))
        .animation(.easeOut(duration: 0.2), value: value)
      if value >= 0 && size >= 40 {
        Text("\(Int((value * 100).rounded()))%")
          .font(.system(size: size / 4.5, weight: .semibold, design: .rounded).monospacedDigit())
          .foregroundStyle(MVTheme.title)
      }
    }
    .frame(width: size, height: size)
    .padding(line / 2)
  }
}

/// Large round tiles (#442): a monogram in a circle, the name under it and a
/// caption; a ring around the circle while `progress` >= 0; a done / failed
/// mark. A click (or Space / Return on the focused tile) is `activate`; a
/// "static" set is shown, not chosen.
@MainActor
private struct TilesNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int

  var body: some View {
    let size = CGFloat(node.int("size") ?? 72)
    let items = node.rows("items")
    let selected = Set(node.props["selection"] as? [String] ?? [])
    let still = node.bool("static")
    Group {
      if still {
        // Shown, not chosen (a sender on an offer): centred, no button.
        HStack(spacing: MVTheme.space(12)) {
          ForEach(items.indices, id: \.self) { i in
            Tile(item: items[i], size: size, selected: false).accessibilityElement(children: .combine)
          }
        }
      } else {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: size + 40, maximum: size + 64), alignment: .top)],
                  alignment: .leading, spacing: MVTheme.space(12)) {
          ForEach(items.indices, id: \.self) { i in
            let item = items[i]
            let id = item["id"] as? String ?? ""
            Button { SlotStore.shared.send(address, node: node.id, kind: 5, value: id) } label: {
              Tile(item: item, size: size, selected: selected.contains(id))
            }
            .buttonStyle(TilePress())
            .accessibilityLabel(Tile.name(item))
            .accessibilityAddTraits(selected.contains(id) ? [.isSelected] : [])
          }
        }
      }
    }
    .frame(maxWidth: .infinity, alignment: still ? .center : .leading)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(node.label)
  }
}

private struct TilePress: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .contentShape(Rectangle())
      .scaleEffect(configuration.isPressed ? 0.96 : 1)
      .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
  }
}

private struct Tile: View {
  @Environment(\.screenWindow) private var window
  let item: [String: Any]
  let size: CGFloat
  let selected: Bool
  @State private var hovered = false
  @Environment(\.isEnabled) private var enabled

  static func name(_ item: [String: Any]) -> String {
    let text = item["text"] as? String ?? ""
    if let detail = item["detail"] as? String, !detail.isEmpty { return "\(text), \(detail)" }
    return text
  }

  var body: some View {
    let progress = (item["progress"] as? NSNumber)?.doubleValue ?? -1
    let mark = item["mark"] as? String
    VStack(spacing: MVTheme.space(6)) {
      ZStack {
        Circle()
          .fill(LinearGradient(colors: [MVTheme.accent.opacity(0.85), MVTheme.accent.opacity(0.55)],
                               startPoint: .top, endPoint: .bottom))
        Text(item["initials"] as? String ?? "?")
          .font(.system(size: size * 0.36, weight: .semibold, design: .rounded))
          .foregroundStyle(.white)
        if progress >= 0 {
          Circle().stroke(MVTheme.hairline, lineWidth: 4).padding(-6)
          Circle()
            .trim(from: 0, to: min(1, progress))
            .stroke(MVTheme.accent, style: StrokeStyle(lineWidth: 4, lineCap: .round))
            .rotationEffect(.degrees(-90))
            .padding(-6)
            .animation(.easeOut(duration: 0.2), value: progress)
        } else if selected || hovered {
          Circle().stroke(MVTheme.accent.opacity(selected ? 1 : 0.5), lineWidth: 2).padding(-5)
        }
        if let mark {
          Image(systemName: mark == "done" ? "checkmark.circle.fill" : "exclamationmark.circle.fill")
            .font(.system(size: size * 0.26))
            .foregroundStyle(.white, mark == "done" ? Color.green : Color.orange)
            .offset(x: size * 0.36, y: size * 0.36)
        }
      }
      .frame(width: size, height: size)
      .padding(6)
      Text(item["text"] as? String ?? "")
        .font(screenFont(13, window: window)).foregroundStyle(MVTheme.title)
        .lineLimit(2).multilineTextAlignment(.center)
      if let detail = item["detail"] as? String, !detail.isEmpty {
        Text(detail).font(screenFont(11, window: window)).foregroundStyle(MVTheme.body).lineLimit(1)
      }
    }
    .frame(width: size + 40)
    .opacity(enabled ? 1 : 0.55)
    .onHover { hovered = $0 }
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
  // The provisional transform, in surface points. `committed` holds every
  // gesture already reported and not yet answered with pixels, each composed
  // about its own anchor (a second pinch before the first one's pixels arrive
  // zooms about where it started, not where the first did); `liveZoom` and
  // `livePan` are the gesture still under the fingers. New pixels (a newer
  // revision) answer what was reported, so `committed` resets then.
  @State private var committed: CGAffineTransform = .identity
  @State private var liveZoom: CGAffineTransform = .identity
  @State private var livePan: CGSize = .zero
  @State private var shownRevision: UInt64 = 0
  @State private var size: CGSize = .zero
  @State private var hover: CGPoint?
  @State private var wheelMonitor: Any?
  @State private var wheelFlush: DispatchWorkItem?
  // Wheel steps shown but not reported yet: one zoom about one anchor.
  @State private var wheelStep: CGFloat = 1
  @State private var wheelAnchor: CGPoint = .zero

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
  // A zoom step that keeps the provisional total scale within `bounds`.
  private func step(_ factor: CGFloat) -> CGFloat {
    let base = max(committed.a, 0.0001)  // uniform scale: a == d, no rotation
    return bounded(base * factor) / base
  }
  // Scale by `s` about `p` (surface points): p stays where it is.
  private static func zoom(_ s: CGFloat, about p: CGPoint) -> CGAffineTransform {
    CGAffineTransform(a: s, b: 0, c: 0, d: s, tx: p.x * (1 - s), ty: p.y * (1 - s))
  }
  private func point(_ unit: CGPoint) -> CGPoint { CGPoint(x: unit.x * size.width, y: unit.y * size.height) }
  private var shown: CGAffineTransform {
    committed.concatenating(liveZoom).concatenating(CGAffineTransform(translationX: livePan.width, y: livePan.height))
  }
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
      .transformEffect(shown)
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
        // The add-on's answer: its pixels replace what was reported. Revisions
        // only grow (registry-wide), so an older one never resets. A gesture
        // still under the fingers keeps going; wheel steps not reported yet
        // are not in these pixels, so they stay shown.
        guard now > shownRevision else { return }
        shownRevision = now
        committed = wheelStep != 1 ? Self.zoom(wheelStep, about: point(wheelAnchor)) : .identity
      }
      .onAppear { size = geo.size }
      .onChange(of: geo.size) { _, now in size = now }
    }
    .frame(width: width > 0 ? width : nil, height: height)
    .frame(maxWidth: width > 0 ? nil : .infinity)
    .clipShape(RoundedRectangle(cornerRadius: MVTheme.radius(6)))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(6)).strokeBorder(MVTheme.hairline, lineWidth: 1))
    .modifier(GlowEdge(state: node.string("glow"), radius: MVTheme.radius(6)))
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

  // A trackpad pinch: shown at once about where it started, reported when it
  // ends. The anchor is in the surface as it looks now, which is how the
  // add-on applies its events: in order, each on top of the last.
  private func magnify(_ size: CGSize) -> some Gesture {
    MagnifyGesture()
      .onChanged { v in
        let at = CGPoint(x: v.startAnchor.x, y: v.startAnchor.y)
        liveZoom = Self.zoom(step(v.magnification), about: point(at))
      }
      .onEnded { v in
        let at = CGPoint(x: v.startAnchor.x, y: v.startAnchor.y)
        let s = step(v.magnification)
        committed = committed.concatenating(Self.zoom(s, about: point(at)))
        liveZoom = .identity
        send(10, ["scale": Double(max(0.05, min(20, s))), "x": clamp(at.x), "y": clamp(at.y)])
      }
  }

  // A drag: the pixels follow, the move is reported in surface widths / heights.
  private func drag(_ size: CGSize) -> some Gesture {
    DragGesture(minimumDistance: 3)
      .onChanged { v in livePan = v.translation }
      .onEnded { v in
        committed = committed.concatenating(CGAffineTransform(translationX: v.translation.width,
                                                              y: v.translation.height))
        livePan = .zero
        guard size.width > 0, size.height > 0 else { return }
        send(11, ["dx": Double(max(-10, min(10, v.translation.width / size.width))),
                  "dy": Double(max(-10, min(10, v.translation.height / size.height)))])
      }
  }

  // A mouse wheel (no precise deltas) zooms about the pointer; a trackpad's
  // two-finger scroll (precise deltas) is left for the pane. Steps arriving
  // together about one point are one zoom, reported 150 ms after the last; a
  // step about another point reports the steps before it first, so every
  // report is exactly what was shown.
  private func installWheel() {
    guard wheelMonitor == nil else { return }
    wheelMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
      guard !event.hasPreciseScrollingDeltas, let at = hover else { return event }
      MainActor.assumeIsolated {
        if wheelStep != 1 && at != wheelAnchor { flushWheel() }
        let s = step(pow(1.1, event.scrollingDeltaY))
        committed = committed.concatenating(Self.zoom(s, about: point(at)))
        wheelStep *= s
        wheelAnchor = at
        wheelFlush?.cancel()
        let work = DispatchWorkItem { MainActor.assumeIsolated { flushWheel() } }
        wheelFlush = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15, execute: work)
      }
      return nil
    }
  }

  private func flushWheel() {
    wheelFlush?.cancel()
    wheelFlush = nil
    guard wheelStep != 1 else { return }
    send(10, ["scale": Double(max(0.05, min(20, wheelStep))), "x": clamp(wheelAnchor.x),
              "y": clamp(wheelAnchor.y)])
    wheelStep = 1
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
  @Environment(\.screenWindow) private var window
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    let binding = Binding(get: { on }, set: { value in
      on = value
      SlotStore.shared.send(address, node: node.id, kind: 2, value: value)
    })
    // #508: the switch on the right of its label, as Settings' rows and the
    // Windows twin; clicking the words flips it too. The switch is the one
    // element VoiceOver sees, named by the label.
    HStack(spacing: MVTheme.space(16)) {
      Text(node.label)
        .font(screenFont(14, window: window))
        .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .onTapGesture { if enabled { binding.wrappedValue.toggle() } }
        .accessibilityHidden(true)
      Toggle(node.label, isOn: binding)
        .labelsHidden()
        .toggleStyle(.switch)
        .tint(MVTheme.accent)
    }
    .frame(minHeight: 24)
    .onAppear { on = node.bool("value") }
    .onChange(of: revision) { _, _ in on = node.bool("value") }
  }
}

private struct ChoiceNode: View {
  @Environment(\.screenWindow) private var window
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
    .font(screenFont(14, window: window))
    .onAppear { value = node.string("value") ?? "" }
    .onChange(of: revision) { _, _ in value = node.string("value") ?? "" }
  }
}

private struct SliderNode: View {
  @Environment(\.screenWindow) private var window
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var value = 0.0

  var body: some View {
    let lo = node.double("min") ?? 0
    let hi = max(lo + 1e-9, node.double("max") ?? 1)
    VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      Text("\(node.label): \(format(value))").font(screenFont(13, window: window)).foregroundStyle(MVTheme.body)
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
  @Environment(\.screenWindow) private var window
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
        .font(screenFont(14, window: window)).foregroundStyle(MVTheme.title)
    }
    .accessibilityLabel(node.label)
    .accessibilityValue(String(value))
    .onAppear { value = node.double("value") ?? max(lo, 0) }
    .onChange(of: revision) { _, _ in value = node.double("value") ?? max(lo, 0) }
  }
}

// @MainActor as a whole: its helpers (the folder dialog, the field) send events
// from outside `body`, which older compilers do not treat as isolated.
@MainActor
private struct TextFieldNode: View {
  @Environment(\.screenWindow) private var window
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var text = ""
  @FocusState private var focused: Bool

  var body: some View {
    if node.string("look") == "code" {
      codeField
    } else {
      HStack(spacing: MVTheme.space(8)) {
        field
        // "pick": "folder" (#410): the system folder dialog, reported as the field's change.
      if node.string("pick") == "folder" {
        Button("Choose\u{2026}") { chooseFolder() }
          .buttonStyle(ScreenButtonStyle(window: window))
          .accessibilityLabel("Choose a folder for \(node.label)")
        }
      }
    }
  }

  /// "look": "code" (#442): large, centred, digits only; it submits by itself
  /// once "digits" are typed (Return submits too), then clears for a retry.
  private var codeField: some View {
    let digits = node.int("digits") ?? 6
    return TextField(node.string("placeholder") ?? node.label, text: $text)
      .textFieldStyle(.plain)
      .multilineTextAlignment(.center)
      .font(.system(size: 30, weight: .semibold, design: .rounded).monospacedDigit())
      .padding(.vertical, MVTheme.space(8))
      .frame(maxWidth: 260)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.canvas))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(8))
        .stroke(focused ? MVTheme.accent : MVTheme.hairline, lineWidth: focused ? 2 : 1))
      .focused($focused)
      .onChange(of: text) { _, now in
        // Digits only, grouped as "123 456" while typing; at full length it goes.
        let only = String(now.filter(\.isNumber).prefix(digits))
        let shown = only.count > 3 ? String(only.prefix(3)) + " " + String(only.dropFirst(3)) : only
        if shown != now { text = shown }
        if only.count == digits {
          SlotStore.shared.send(address, node: node.id, kind: 3, value: only)
          text = ""
        }
      }
      .onSubmit {
        let only = text.filter(\.isNumber)
        if !only.isEmpty { SlotStore.shared.send(address, node: node.id, kind: 3, value: only) }
      }
      .accessibilityLabel(node.label)
      .onAppear { focused = true }
  }

  private func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.allowsMultipleSelection = false
    panel.canCreateDirectories = true
    panel.prompt = "Choose"
    panel.message = node.label
    let current = text.isEmpty ? (node.string("value") ?? "") : text
    if !current.isEmpty { panel.directoryURL = URL(fileURLWithPath: current, isDirectory: true) }
    let done: (NSApplication.ModalResponse) -> Void = { response in
      guard response == .OK, let url = panel.url else { return }
      // AppKit calls a sheet's handler on the main thread.
      MainActor.assumeIsolated {
        text = url.path
        SlotStore.shared.send(address, node: node.id, kind: 2, value: url.path)
      }
    }
    if let window = NSApp.keyWindow {
      panel.beginSheetModal(for: window, completionHandler: done)
    } else {
      done(panel.runModal())
    }
  }

  private var field: some View {
    Group {
      if node.bool("multiline") {
        TextField(node.string("placeholder") ?? node.label, text: $text, axis: .vertical).lineLimit(3...8)
      } else {
        TextField(node.string("placeholder") ?? node.label, text: $text)
      }
    }
    .textFieldStyle(.roundedBorder)
    .font(screenFont(14, window: window))
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
  @Environment(\.screenWindow) private var window
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
        VStack(alignment: .leading, spacing: MVTheme.space(2)) {
          Text(rows[i]["text"] as? String ?? "").font(screenFont(14, window: window)).foregroundStyle(MVTheme.title)
          if let detail = rows[i]["detail"] as? String {
            Text(detail).font(screenFont(12, window: window)).foregroundStyle(MVTheme.body)
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
  @Environment(\.screenWindow) private var window
  let node: ScreenNode
  let address: ScreenAddress
  let revision: Int
  @State private var selection: Set<String> = []

  var body: some View {
    let columns = node.rows("columns")
    let rows = node.rows("rows")
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: MVTheme.space(8)) {
        ForEach(columns.indices, id: \.self) { c in
          Text(columns[c]["title"] as? String ?? "").font(screenFont(12, window: window)).foregroundStyle(MVTheme.body)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      .padding(.horizontal, MVTheme.space(8))
      .padding(.vertical, MVTheme.space(4))
      .accessibilityAddTraits(.isHeader)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      List(selection: Binding(get: { selection }, set: { s in
        selection = node.bool("multi") ? s : Set(s.prefix(1))
        SlotStore.shared.send(address, node: node.id, kind: 4, value: Array(selection).sorted())
      })) {
        ForEach(rows.indices, id: \.self) { r in
          let id = rows[r]["id"] as? String ?? ""
          let cells = rows[r]["cells"] as? [String] ?? []
          HStack(spacing: MVTheme.space(8)) {
            ForEach(columns.indices, id: \.self) { c in
              Text(c < cells.count ? cells[c] : "").font(screenFont(13, window: window)).foregroundStyle(MVTheme.title)
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
          LazyVGrid(columns: Array(repeating: GridItem(.fixed(tile), spacing: MVTheme.space(8)), count: cols), spacing: MVTheme.space(8)) {
            ForEach(items.indices, id: \.self) { i in
              let path = items[i]["path"] as? String ?? ""
              let name = items[i]["label"] as? String ?? (path as NSString).lastPathComponent
              SlotThumbnail(path: path, size: tile, label: name)
                .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(6)).strokeBorder(
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
          .padding(MVTheme.space(8))
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

  @MainActor private func select(_ path: String, items: [[String: Any]]) {
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
          VStack(alignment: .leading, spacing: MVTheme.space(6)) {
            Text(section.title).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
              .accessibilityAddTraits(.isHeader)
            ScreenNodeView(node: section.tree, address: ScreenAddress(addon: section.addon, screen: section.section),
                           revision: section.revision)
          }
          .padding(MVTheme.space(14))
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
          Text(screen?.title ?? "").font(screenFont(17, .semibold, window: true)).foregroundStyle(MVTheme.title)
            .accessibilityAddTraits(.isHeader)
          Spacer()
          Button("Done") { slots.close(address) }
            .buttonStyle(ScreenButtonStyle())
            .keyboardShortcut(.cancelAction)
        }
        .padding(.horizontal, MVTheme.space(16)).padding(.vertical, MVTheme.space(12))
        Rectangle().fill(MVTheme.hairline).frame(height: 1)
      }
      if let tree = screen?.tree {
        // #508: a scrolling tree's margin is inside the scroller, so its bar
        // sits on the window's edge, not over the cards (the Windows twin's
        // 16 / 14 / 16 / 18); any other tree keeps it outside.
        let scrolls = tree.type == "scroll" && tree.string("axis") != "horizontal"
        ScreenNodeView(node: tree, address: address, revision: screen?.revision ?? 0)
          .environment(\.screenRootInset, scrolls ? EdgeInsets(
            top: MVTheme.space(place == "window" ? 14 : 12), leading: MVTheme.space(16),
            bottom: MVTheme.space(18), trailing: MVTheme.space(16)) : nil)
          .padding(scrolls ? EdgeInsets() : EdgeInsets(top: MVTheme.space(12), leading: MVTheme.space(16),
                                                        bottom: MVTheme.space(16), trailing: MVTheme.space(16)))
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
          .onPreferenceChange(ScreenContentHeight.self) { height in
            guard place == "window" else { return }
            SlotWindows.shared.fit(address, content: height, asked: screen?.height ?? 0)
          }
      } else {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .environment(\.screenWindow, true)
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
  private var fitted: Set<ScreenAddress> = []

  /// #508, once per window, when its first tree has laid out: a window taller
  /// than what it shows shrinks to its content (never below a floor), keeping
  /// its top edge, so it opens without an empty band at the bottom. One whose
  /// content is taller keeps its height and scrolls. The Windows twin is
  /// FitScreenWindow.
  func fit(_ address: ScreenAddress, content height: CGFloat, asked: CGFloat) {
    guard height > 0, !fitted.contains(address), let w = windows[address], w.sheetParent == nil else { return }
    fitted.insert(address)
    let shown = w.contentLayoutRect.height
    guard shown - height >= 8 else { return }
    let floor = min(asked > 0 ? asked : 480, 360)
    let target = max(floor, height.rounded(.up))
    guard target < shown else { return }
    var frame = w.frame
    frame.origin.y += shown - target
    frame.size.height -= shown - target
    w.setFrame(frame, display: true)
  }

  func sync(_ screens: [SlotScreen]) {
    // A settings page is drawn in Settings (AddonSettingsScreen), not here.
    let screens = screens.filter { $0.place != "settings" }
    let wanted = Set(screens.map { ScreenAddress(addon: $0.addon, screen: $0.screen) })
    for (address, window) in windows where !wanted.contains(address) {
      closingFromCore.insert(address)
      close(window)
      windows[address] = nil
      closingFromCore.remove(address)
      fitted.remove(address)
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
      // Held to the right edge of the viewer window's content (docs/design/25 §10,
      // decision log 2026-10-11): a child panel over the canvas's right side (the
      // canvas does not reflow), kept there as the viewer moves, resizes, enters or
      // leaves full screen, and moved to whichever viewer window becomes main.
      let width = s.width > 0 ? s.width : 320
      let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: width, height: 600),
                      styleMask: [.titled, .closable, .utilityWindow, .resizable],
                      backing: .buffered, defer: false)
      p.title = s.title
      p.contentView = host
      p.isReleasedWhenClosed = false
      p.isFloatingPanel = false
      p.becomesKeyOnlyIfNeeded = false
      // Follows its viewer into a full-screen Space.
      p.collectionBehavior.insert(.fullScreenAuxiliary)
      p.delegate = self
      windows[address] = p
      if let viewer = Self.viewerWindow(preferring: main) { dock(p, to: viewer) }
      hold(p)
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

  // MARK: the docked pane

  private var paneWatch: [ObjectIdentifier: [NSObjectProtocol]] = [:]

  /// A viewer window (the one AppKit made full-screen-primary), not Settings, an
  /// add-on window or a panel: `preferring` if it is one, else the main or first visible one.
  private static func viewerWindow(preferring w: NSWindow?) -> NSWindow? {
    func isViewer(_ w: NSWindow) -> Bool {
      !(w is NSPanel) && w.isVisible && w.sheetParent == nil &&
        w.collectionBehavior.contains(.fullScreenPrimary)
    }
    if let w, isViewer(w) { return w }
    if let m = NSApp.mainWindow, isViewer(m) { return m }
    return NSApp.windows.first(where: isViewer)
  }

  /// The pane against the right edge of `viewer`'s content, full content height,
  /// at the width the person left it.
  private func dock(_ p: NSPanel, to viewer: NSWindow) {
    if p.parent !== viewer {
      p.parent?.removeChildWindow(p)
      viewer.addChildWindow(p, ordered: .above)
    }
    let content = viewer.convertToScreen(viewer.contentLayoutRect)
    let width = min(p.frame.width, content.width)
    let frame = NSRect(x: content.maxX - width, y: content.minY, width: width, height: content.height)
    if frame != p.frame { p.setFrame(frame, display: true) }
  }

  private func hold(_ p: NSPanel) {
    let nc = NotificationCenter.default
    var tokens: [NSObjectProtocol] = []
    // The viewer it is docked to resized or changed full-screen state: re-dock.
    for name in [NSWindow.didResizeNotification, NSWindow.didEnterFullScreenNotification,
                 NSWindow.didExitFullScreenNotification] {
      tokens.append(nc.addObserver(forName: name, object: nil, queue: .main) { [weak self, weak p] note in
        MainActor.assumeIsolated {
          guard let self, let p, let viewer = note.object as? NSWindow, viewer === p.parent else { return }
          self.dock(p, to: viewer)
        }
      })
    }
    // Another viewer window became main: the pane goes with it.
    tokens.append(nc.addObserver(forName: NSWindow.didBecomeMainNotification, object: nil, queue: .main) {
      [weak self, weak p] note in
      MainActor.assumeIsolated {
        guard let self, let p, let w = note.object as? NSWindow, w !== p.parent,
              let viewer = Self.viewerWindow(preferring: w), viewer === w else { return }
        self.dock(p, to: viewer)
      }
    })
    paneWatch[ObjectIdentifier(p)] = tokens
  }

  private func release(_ window: NSWindow) {
    guard let tokens = paneWatch.removeValue(forKey: ObjectIdentifier(window)) else { return }
    tokens.forEach { NotificationCenter.default.removeObserver($0) }
  }

  /// #452: the window is on screen and in front (key or main): a notice about
  /// it would only repeat what the person sees.
  func isFront(_ address: ScreenAddress) -> Bool {
    guard let w = windows[address], w.isVisible, !w.isMiniaturized else { return false }
    return w.isKeyWindow || w.isMainWindow
  }

    func bringForward(_ address: ScreenAddress) {
    guard let w = windows[address], w.sheetParent == nil else { return }
    w.makeKeyAndOrderFront(nil)
  }

  private func close(_ window: NSWindow) {
    release(window)
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
    guard let window = notification.object as? NSWindow else { return }
    release(window)
    guard let address = windows.first(where: { $0.value === window })?.key,
          !closingFromCore.contains(address)
    else { return }
    windows[address] = nil
    fitted.remove(address)
    SlotStore.shared.close(address)
  }
}

// MARK: 2026-10-19 additions: a button's copy, the link picture

/// A button. With "copy" its click also puts that text on the pasteboard
/// (here, never by the add-on) and its label says "Copied" for a moment.
@MainActor
private struct ButtonNode: View {
  let node: ScreenNode
  let address: ScreenAddress
  @State private var copied = false
  @Environment(\.screenWindow) private var window

  var body: some View {
    Button {
      if let text = node.string("copy") {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        copied = true
        Task { @MainActor in
          try? await Task.sleep(nanoseconds: 1_500_000_000)
          copied = false
        }
      }
      SlotStore.shared.send(address, node: node.id, kind: 1, value: nil)
    } label: {
      if copied {
        Label("Copied", systemImage: "checkmark")
      } else if let symbol = node.string("symbol") {
        Label(node.label, systemImage: slotSymbol(symbol))
      } else if node.string("copy") != nil {
        Label(node.label, systemImage: "doc.on.doc")
      } else {
        Text(node.label)
      }
    }
    .buttonStyle(ScreenButtonStyle(style: node.string("style") ?? "plain", window: window))
    .accessibilityLabel(copied ? "Copied" : node.label)
  }
}

/// The path a transfer takes: this computer on the left, the other on the
/// right, the bearer's badge between them (a third computer for a relay), and
/// dots that travel the way the bytes go. Each bearer has its own shape and
/// colour: Wi-Fi a straight line, Bluetooth short dashes, the internet an arc,
/// the DHT an arc through a scatter of nodes, a relay a dip through the middle
/// computer. It animates only while "moving" or "connecting", at 30 fps at
/// most, and never with Reduce Motion; done and failed are still.
@MainActor
private struct LinkNode: View {
  @Environment(\.screenWindow) private var window
  let node: ScreenNode
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private var via: String { node.string("via") ?? "wifi" }
  private var state: String { node.string("state") ?? "connecting" }
  private var sending: Bool { node.string("direction") != "receive" }
  private var value: Double { node.double("value") ?? -1 }

  private var tint: Color {
    switch via {
    case "bluetooth": return Color(nsColor: .systemBlue)
    case "wifi": return Color(nsColor: .systemTeal)
    case "internet": return Color(nsColor: .systemIndigo)
    case "dht": return Color(nsColor: .systemOrange)
    default: return Color(nsColor: .systemPurple)  // relay, server
    }
  }

  private var badge: String {
    switch via {
    case "bluetooth": return "dot.radiowaves.left.and.right"
    case "wifi": return "wifi"
    case "internet": return "globe"
    case "dht": return "point.3.connected.trianglepath.dotted"
    case "relay": return "desktopcomputer"
    default: return "server.rack"
    }
  }

  private var animating: Bool { !reduceMotion && (state == "moving" || state == "connecting") }

  var body: some View {
    VStack(spacing: MVTheme.space(6)) {
      // Top-aligned (a two-line name grows downward); the path's middle is lifted to the
      // circles' middle (24), so it meets each computer at its centre.
      HStack(alignment: .top, spacing: 0) {
        device(symbol: "laptopcomputer", name: node.string("from") ?? "This computer", mark: nil)
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !animating)) { t in
          Canvas { ctx, size in
            draw(ctx, size, phase: t.date.timeIntervalSinceReferenceDate)
          }
          .overlay(badgeView)
        }
        .frame(height: 64)
        .padding(.top, 24 - 32)
        device(symbol: nil, name: node.string("to") ?? "", mark: state == "done" ? "checkmark.circle.fill"
               : state == "failed" ? "exclamationmark.circle.fill" : nil)
      }
      HStack(spacing: MVTheme.space(6)) {
        Text(node.string("caption") ?? "")
          .font(screenFont(12, window: window))
          .foregroundStyle(MVTheme.body)
        if value >= 0 {
          Text("\(Int((value * 100).rounded()))%")
            .font(screenFont(12, window: window).monospacedDigit())
            .foregroundStyle(MVTheme.title)
        }
      }
    }
    .frame(maxWidth: .infinity)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(node.label)
    .accessibilityValue(value < 0 ? stateWords : "\(Int((value * 100).rounded())) percent")
  }

  private var stateWords: String {
    switch state {
    case "moving": return "Moving"
    case "paused": return "Paused"
    case "done": return "Done"
    case "failed": return "Stopped"
    default: return "Connecting"
    }
  }

  private func device(symbol: String?, name: String, mark: String?) -> some View {
    VStack(spacing: 4) {
      ZStack(alignment: .bottomTrailing) {
        Circle().fill(MVTheme.surface)
          .overlay(Circle().stroke(MVTheme.hairline, lineWidth: 1))
          .frame(width: 48, height: 48)
          .overlay {
            if let symbol {
              Image(systemName: symbol).font(.system(size: 20)).foregroundStyle(MVTheme.title)
            } else {
              Text(initials(name)).font(.system(size: 17, weight: .semibold, design: .rounded))
                .foregroundStyle(MVTheme.title)
            }
          }
        if let mark {
          Image(systemName: mark)
            .font(.system(size: 16))
            .foregroundStyle(state == "done" ? Color(nsColor: .systemGreen) : Color(nsColor: .systemRed))
            .background(Circle().fill(MVTheme.canvas))
            .transition(.scale)
        }
      }
      Text(name).font(screenFont(11, window: window)).foregroundStyle(MVTheme.body).lineLimit(2)
        .multilineTextAlignment(.center).frame(maxWidth: 88)
    }
    .frame(width: 88)
  }

  private var badgeView: some View {
    ZStack {
      Circle().fill(MVTheme.canvas).frame(width: 34, height: 34)
      Circle().fill(tint.opacity(state == "failed" ? 0.08 : 0.16)).frame(width: 34, height: 34)
      Circle().stroke(tint.opacity(0.6), lineWidth: 1.5).frame(width: 34, height: 34)
      Image(systemName: state == "paused" ? "pause.fill" : badge)
        .font(.system(size: 15, weight: .medium))
        .foregroundStyle(state == "failed" ? MVTheme.disabled : tint)
      // A relay: the computer in between, named under it.
      if let through = node.string("through"), !through.isEmpty {
        Text(through).font(screenFont(10, window: window)).foregroundStyle(MVTheme.body).lineLimit(1)
          .fixedSize().offset(y: 27)
      }
    }
    .offset(y: arcLift * -0.5)
    .accessibilityHidden(true)
  }

  /// How far the path bows up in the middle (an arc for the internet).
  private var arcLift: CGFloat {
    switch via {
    case "internet", "dht", "relay", "server": return 22
    default: return 0
    }
  }

  private func point(_ s: CGFloat, _ size: CGSize) -> CGPoint {
    let y0 = size.height / 2 + arcLift * 0.25
    let x = 6 + (size.width - 12) * s
    let y = y0 - arcLift * 4 * s * (1 - s)
    return CGPoint(x: x, y: y)
  }

  private func path(_ size: CGSize, from a: CGFloat = 0, to b: CGFloat = 1) -> Path {
    var p = Path()
    let steps = 48
    for i in 0...steps {
      let s = a + (b - a) * CGFloat(i) / CGFloat(steps)
      let q = point(s, size)
      if i == 0 { p.move(to: q) } else { p.addLine(to: q) }
    }
    return p
  }

  private func draw(_ ctx: GraphicsContext, _ size: CGSize, phase: Double) {
    let failed = state == "failed"
    let dash: [CGFloat] = via == "bluetooth" ? [3, 5] : via == "dht" ? [1, 6] : []
    // The track.
    ctx.stroke(path(size), with: .color(failed ? MVTheme.disabled.opacity(0.5) : tint.opacity(0.28)),
               style: StrokeStyle(lineWidth: 3, lineCap: .round, dash: dash))
    // What has moved, from the sender's end.
    let done = state == "done" ? 1.0 : max(0, value)
    if done > 0 && !failed {
      let p = sending ? path(size, from: 0, to: CGFloat(done)) : path(size, from: 1 - CGFloat(done), to: 1)
      ctx.stroke(p, with: .color(tint), style: StrokeStyle(lineWidth: 3, lineCap: .round))
    }
    // The DHT: a few still nodes off the arc, the ones it met through.
    if via == "dht" {
      for (s, dy) in [(0.22, -14.0), (0.36, 12.0), (0.64, -13.0), (0.8, 11.0)] {
        let q = point(CGFloat(s), size)
        let r = CGRect(x: q.x - 2.5, y: q.y + CGFloat(dy) - 2.5, width: 5, height: 5)
        ctx.fill(Path(ellipseIn: r), with: .color(tint.opacity(0.45)))
      }
    }
    // Dots travelling the way the bytes go; slower and fainter while connecting.
    guard state == "moving" || state == "connecting" || (reduceMotion && state != "done" && !failed) else { return }
    let moving = state == "moving"
    let count = moving ? 5 : 3
    let speed = moving ? (via == "bluetooth" ? 0.35 : via == "wifi" ? 0.7 : 0.5) : 0.25
    for i in 0..<count {
      var s = (phase * speed + Double(i) / Double(count)).truncatingRemainder(dividingBy: 1)
      if reduceMotion { s = Double(i + 1) / Double(count + 1) }
      if !sending { s = 1 - s }
      let q = point(CGFloat(s), size)
      // Fade in and out at the ends so dots do not pop.
      let edge = min(s, 1 - s) * 6
      let alpha = (moving ? 1.0 : 0.55) * min(1, edge)
      let r: CGFloat = moving ? 3.5 : 3
      ctx.fill(Path(ellipseIn: CGRect(x: q.x - r, y: q.y - r, width: r * 2, height: r * 2)),
               with: .color(tint.opacity(alpha)))
    }
  }

  private func initials(_ name: String) -> String {
    let words = name.split(whereSeparator: { $0 == " " || $0 == "-" || $0 == "_" || $0 == "." })
    let s = words.prefix(2).compactMap { $0.first.map { String($0).uppercased() } }.joined()
    return s.isEmpty ? "?" : s
  }
}

// ---- contribution API 9: the glow (2026-10-28, Enhance) ----------------------------

/// The glow's colours: violet, pink, amber, teal and blue, the same on both
/// hosts (IslandHost.Screens.cs GlowColors).
enum GlowPalette {
  static let colors: [Color] = [
    Color(red: 0.55, green: 0.36, blue: 0.96), Color(red: 0.98, green: 0.36, blue: 0.62),
    Color(red: 1.00, green: 0.62, blue: 0.25), Color(red: 0.25, green: 0.78, blue: 0.85),
    Color(red: 0.36, green: 0.52, blue: 1.00), Color(red: 0.55, green: 0.36, blue: 0.96),
  ]
}

/// A soft multicolour edge round a surface ("glow"): a thin ring and a blurred
/// halo behind it. "moving" turns it once every six seconds, at 30 fps at most,
/// and never with Reduce Motion; "still" holds it. The canvas is never touched:
/// this is the sheet's own drawing, and it stops when the node does.
private struct GlowEdge: ViewModifier {
  let state: String?
  let radius: CGFloat
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  func body(content: Content) -> some View {
    if let state {
      let moving = state == "moving" && !reduceMotion
      content.background {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !moving)) { t in
          let turn = moving ? t.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 6) / 6 : 0
          let gradient = AngularGradient(colors: GlowPalette.colors, center: .center, angle: .degrees(turn * 360))
          ZStack {
            RoundedRectangle(cornerRadius: radius + 3).fill(gradient).blur(radius: 14).opacity(0.55)
            RoundedRectangle(cornerRadius: radius + 3).fill(gradient)
          }
          .padding(-3)
        }
        .accessibilityHidden(true)
      }
    } else {
      content
    }
  }
}

/// A progress bar in the glow's colours. A soft highlight sweeps along it
/// while it moves (never with Reduce Motion); not started (value < 0), the
/// whole bar shows dimmed.
private struct GlowBar: View {
  let value: Double
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    GeometryReader { geo in
      let filled = value < 0 ? geo.size.width : geo.size.width * min(1, max(0, value))
      ZStack(alignment: .leading) {
        Capsule().fill(MVTheme.hairline)
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) { t in
          let phase = reduceMotion ? -1 : t.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.4) / 2.4
          Capsule()
            .fill(LinearGradient(colors: GlowPalette.colors, startPoint: .leading, endPoint: .trailing))
            .overlay(shimmer(at: phase))
            .clipShape(Capsule())
            .frame(width: max(0, filled))
            .opacity(value < 0 ? 0.55 : 1)
        }
      }
    }
    .frame(height: 6)
    .frame(minWidth: 120)
  }

  @ViewBuilder private func shimmer(at phase: Double) -> some View {
    if phase >= 0 {
      // A band 30 % of the bar wide, from just off its left end to just off its right.
      let centre = -0.15 + phase * 1.3
      LinearGradient(stops: [
        .init(color: .clear, location: 0),
        .init(color: .clear, location: min(1, max(0, centre - 0.15))),
        .init(color: .white.opacity(0.5), location: min(1, max(0, centre))),
        .init(color: .clear, location: min(1, max(0, centre + 0.15))),
        .init(color: .clear, location: 1),
      ], startPoint: .leading, endPoint: .trailing)
    }
  }
}
