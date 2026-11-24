// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/16 "Drag and drop into folders": files dropped on the gallery (the
// folder on screen), a folder tile, a breadcrumb segment or a sidebar place go
// into that folder. From Finder or another app they are copied; dragged from
// this window they are moved, Option copies (shell/file_drop.h decides, through
// mv_chrome_drop_verdict, so both chromes and the keyboard's Paste agree). The
// file work is the host's job on its pool, with a row in the Jobs pane.
//
// One DropState for the window: the target under the pointer, the verdict for
// it, and the files of a drag that started here (FileDragSource sets them).
// Targets observe it, but it publishes only when the hovered target or its
// verdict changes, never per mouse move.
import AppKit
import MVChromeBridge
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class DropState: ObservableObject {
  static let shared = DropState()

  /// The folder path under the pointer, nil when no drag is over a target.
  @Published private(set) var hover: String?
  @Published private(set) var verdict = DropVerdict()
  /// Files in the drag: known for ours, the providers' count for another app's.
  @Published private(set) var count: Int?
  /// The hovered target's name, for the label over the grid.
  @Published private(set) var targetName = ""
  /// The refusal's words (the host's: drop_refusal_words).
  @Published private(set) var reason = ""

  /// The paths of a drag that began in this window, until it ends.
  private(set) var ownPaths: [String]?
  private var flags: DropFlags = []
  private var spring: DispatchWorkItem?

  func ownDragBegan(_ paths: [String]) { ownPaths = paths }
  func ownDragEnded() {
    ownPaths = nil
    leaveAll()
  }

  private static func currentFlags(fromApp: Bool) -> DropFlags {
    let m = NSEvent.modifierFlags
    return .of(fromApp: fromApp, option: m.contains(.option), command: m.contains(.command))
  }

  private func ask(_ target: String, _ flags: DropFlags) -> DropVerdict {
    let paths = ownPaths ?? []
    let raw = withCStrings(paths) { ptrs in
      target.withCString { mv_chrome_drop_verdict(ptrs, Int32(paths.count), $0, flags.rawValue) }
    }
    return DropVerdict(raw: raw)
  }

  /// A drag entered `target` (named `name`); `springOpen`, when given, runs
  /// once the pointer has rested there for SpringLoad.delay.
  func enter(_ target: String, name: String, info: DropInfo, springOpen: (() -> Void)?) {
    spring?.cancel()
    spring = nil
    let fromApp = ownPaths != nil
    flags = Self.currentFlags(fromApp: fromApp)
    let v = ask(target, flags)
    if hover != target { hover = target }
    if targetName != name { targetName = name }
    if verdict != v { verdict = v }
    let n = ownPaths?.count ?? info.itemProviders(for: [UTType.fileURL]).count
    if count != n { count = n }
    let words = v.refusedVisibly ? Self.reason(v, target: target) : ""
    if reason != words { reason = words }
    if let springOpen {
      let work = DispatchWorkItem { [weak self] in
        MainActor.assumeIsolated {
          guard let self, self.hover == target else { return }
          self.leaveAll()
          springOpen()
        }
      }
      spring = work
      DispatchQueue.main.asyncAfter(deadline: .now() + SpringLoad.delay, execute: work)
    }
  }

  /// The same target, the pointer moved: only a modifier change asks again.
  func update(_ target: String) -> DropProposal {
    guard hover == target else { return DropProposal(operation: .forbidden) }
    let now = Self.currentFlags(fromApp: ownPaths != nil)
    if now != flags {
      flags = now
      let v = ask(target, now)
      if verdict != v {
        verdict = v
        reason = v.refusedVisibly ? Self.reason(v, target: target) : ""
      }
    }
    switch verdict.verb {
    case .move: return DropProposal(operation: .move)
    case .copy: return DropProposal(operation: .copy)
    case .none: return DropProposal(operation: .forbidden)
    }
  }

  func exit(_ target: String) {
    guard hover == target else { return }
    leaveAll()
  }

  private func leaveAll() {
    spring?.cancel()
    spring = nil
    if hover != nil { hover = nil }
    if count != nil { count = nil }
  }

  /// The drop itself. Ours are paths already; another app's are read from the
  /// providers off the main thread, then handed to the host on it.
  func perform(_ target: String, info: DropInfo) -> Bool {
    let flags = Self.currentFlags(fromApp: ownPaths != nil)
    let allowed = ask(target, flags).allowed
    leaveAll()
    guard allowed else { return false }
    if let paths = ownPaths {
      Self.drop(paths, into: target, flags: flags)
      return true
    }
    let providers = info.itemProviders(for: [UTType.fileURL])
    guard !providers.isEmpty else { return false }
    let group = DispatchGroup()
    let lock = NSLock()
    var urls = [(Int, String)]()
    for (i, provider) in providers.enumerated() {
      group.enter()
      _ = provider.loadObject(ofClass: URL.self) { url, _ in
        if let url, url.isFileURL {
          lock.lock()
          urls.append((i, url.path))
          lock.unlock()
        }
        group.leave()
      }
    }
    group.notify(queue: .main) {
      let paths = urls.sorted { $0.0 < $1.0 }.map(\.1)
      guard !paths.isEmpty else { return }
      MainActor.assumeIsolated { Self.drop(paths, into: target, flags: flags) }
    }
    return true
  }

  private static func drop(_ paths: [String], into target: String, flags: DropFlags) {
    _ = withCStrings(paths) { ptrs in
      target.withCString { mv_chrome_drop_files(ptrs, Int32(paths.count), $0, flags.rawValue) }
    }
  }

  /// A remote folder this computer may not change says why in the server's terms
  /// ("Home lets this computer view only…"); every other refusal is the rule's words.
  private static func reason(_ v: DropVerdict, target: String) -> String {
    if v.refusal == .remoteTarget {
      var buf = [CChar](repeating: 0, count: 512)
      let n = buf.withUnsafeMutableBufferPointer { ptr in
        target.withCString { mv_chrome_write_problem($0, ptr.baseAddress, Int32(ptr.count)) }
      }
      if n > 0 { return String(cString: buf) }
    }
    return refusalText(v.refusal)
  }

  private static func refusalText(_ r: DropVerdict.Refusal) -> String {
    var buf = [CChar](repeating: 0, count: 256)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_drop_refusal_text(r.rawValue, $0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }
}

/// `body` with a C array of the strings' UTF-8 copies, freed after.
private func withCStrings<R>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>?) -> R) -> R {
  let copies: [UnsafeMutablePointer<CChar>?] = strings.map { strdup($0) }
  defer { copies.forEach { free($0) } }
  let ptrs: [UnsafePointer<CChar>?] = copies.map { $0.map { UnsafePointer($0) } }
  return ptrs.withUnsafeBufferPointer { body($0.baseAddress) }
}

/// The SwiftUI side of one target.
struct FolderDropDelegate: DropDelegate {
  let target: String
  let name: String
  var springOpen: (() -> Void)? = nil

  func validateDrop(info: DropInfo) -> Bool { info.hasItemsConforming(to: [UTType.fileURL]) }
  func dropEntered(info: DropInfo) {
    MainActor.assumeIsolated { DropState.shared.enter(target, name: name, info: info, springOpen: springOpen) }
  }
  func dropUpdated(info: DropInfo) -> DropProposal? {
    MainActor.assumeIsolated { DropState.shared.update(target) }
  }
  func dropExited(info: DropInfo) {
    MainActor.assumeIsolated { DropState.shared.exit(target) }
  }
  func performDrop(info: DropInfo) -> Bool {
    MainActor.assumeIsolated { DropState.shared.perform(target, info: info) }
  }
}

/// The look of a hovered target: the accent ring and a light wash when it takes
/// the drop, a dashed ring in the body ink when it refuses. Instant on, as
/// Finder's highlight is; nothing animates while the pointer moves.
private struct DropHighlight: ViewModifier {
  @ObservedObject private var drop = DropState.shared
  let target: String
  let radius: CGFloat
  let inset: CGFloat

  func body(content: Content) -> some View {
    let on = drop.hover == target
    content.overlay {
      if on {
        let shape = RoundedRectangle(cornerRadius: radius).inset(by: -inset)
        if drop.verdict.allowed {
          shape.fill(MVTheme.accent.opacity(0.14))
            .overlay(shape.strokeBorder(MVTheme.accent, lineWidth: 2))
            .allowsHitTesting(false)
        } else if drop.verdict.refusedVisibly {
          shape.strokeBorder(MVTheme.body, style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
            .allowsHitTesting(false)
        }
      }
    }
  }
}

extension View {
  /// Makes this view a drop target for the folder at `path`.
  func folderDropTarget(_ path: String, name: String, radius: CGFloat = MVTheme.radius(6), inset: CGFloat = 0,
                        springOpen: (() -> Void)? = nil) -> some View {
    onDrop(of: [UTType.fileURL], delegate: FolderDropDelegate(target: path, name: name, springOpen: springOpen))
      .modifier(DropHighlight(target: path, radius: radius, inset: inset))
  }
}

/// The line at the foot of the grid while a drag hovers any target: what the
/// drop will do, or why it will not.
struct DropLabel: View {
  @ObservedObject private var drop = DropState.shared

  var body: some View {
    if drop.hover != nil, drop.verdict.allowed || drop.verdict.refusedVisibly {
      HStack(spacing: MVTheme.space(6)) {
        Image(systemName: drop.verdict.allowed
              ? (drop.verdict.verb == .move ? "arrow.right.doc.on.clipboard" : "plus.circle.fill")
              : "nosign")
        Text(drop.verdict.label(count: drop.count, target: drop.targetName, reason: drop.reason))
          .lineLimit(1)
          .truncationMode(.middle)
      }
      .font(MVTheme.font(13))
      .fontWeight(.medium)
      .foregroundStyle(MVTheme.title)
      .padding(.horizontal, MVTheme.space(12))
      .padding(.vertical, MVTheme.space(6))
      .background(.regularMaterial, in: Capsule())
      .overlay(Capsule().strokeBorder(MVTheme.hairline, lineWidth: 1))
      .padding(.bottom, MVTheme.space(14))
      .allowsHitTesting(false)
      .accessibilityElement(children: .combine)
    }
  }
}
