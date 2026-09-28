// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Dragging a search result's original out of the panel, as Finder does. The
// same helper as MediaViewerChrome's FileDrag.swift (this bundle links nothing
// of the app, so it keeps its own copy):
// one NSDraggingItem per file whose pasteboard writer is the file's NSURL
// (public.file-url), so Final Cut Pro, Finder, Mail and the rest read the
// original in place. No promise, no copy, no decode: the drag image is the
// thumbnail already on screen, else the type's icon (no file I/O).
//
// Started from a SwiftUI DragGesture (`fileDrag` below) rather than an
// AppKit overlay view: the cells keep their own tap, hover and scroll handling
// untouched, and a click that does not move past the threshold is still a tap.
// Copy-only, like the canvas's ⌘-drag (main_mac.mm): dropping in Finder on the
// same volume copies, never moves, an original. Refused inside the app, so a
// slip onto our own window does not reopen the folder.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

final class FileDragSource: NSObject, NSDraggingSource {
  @MainActor static let shared = FileDragSource()

  /// A session is running: a gesture's late onChanged must not start another.
  @MainActor private var active = false

  /// Starts a drag of the files `list()` names (path, thumbnail) from the
  /// mouse event being handled. `list` runs only when a drag really starts.
  /// Call from a gesture callback, on the main thread.
  @MainActor
  func begin(_ list: () -> [(path: String, image: CGImage?)]) {
    guard !active,
          let event = NSApp.currentEvent,
          event.type == .leftMouseDragged || event.type == .leftMouseDown,
          let view = event.window?.contentView else { return }
    let files = list()
    guard !files.isEmpty else { return }
    let p = view.convert(event.locationInWindow, from: nil)
    var items: [NSDraggingItem] = []
    items.reserveCapacity(files.count)
    for (i, file) in files.enumerated() {
      let url = URL(fileURLWithPath: file.path)
      let item = NSDraggingItem(pasteboardWriter: url as NSURL)
      let image = Self.dragImage(file.image, url: url)
      // Stacked a little, the first few only; the rest sit under them.
      let step = CGFloat(min(i, 4)) * 4
      let origin = NSPoint(x: p.x - image.size.width / 2 + step,
                           y: p.y - image.size.height / 2 + step)
      item.setDraggingFrame(NSRect(origin: origin, size: image.size), contents: image)
      items.append(item)
    }
    active = true
    let session = view.beginDraggingSession(with: items, event: event, source: self)
    session.animatesToStartingPositionsOnCancelOrFail = true
    session.draggingFormation = .default
  }

  @MainActor
  private static func dragImage(_ thumb: CGImage?, url: URL) -> NSImage {
    let edge: CGFloat = 96
    if let thumb, thumb.width > 0, thumb.height > 0 {
      let w = CGFloat(thumb.width), h = CGFloat(thumb.height)
      let scale = edge / max(w, h)
      return NSImage(cgImage: thumb, size: NSSize(width: max(1, w * scale), height: max(1, h * scale)))
    }
    // The type from the extension, not from the file: no disk read on the UI thread.
    let type = UTType(filenameExtension: url.pathExtension) ?? .item
    let icon = NSWorkspace.shared.icon(for: type)
    icon.size = NSSize(width: 64, height: 64)
    return icon
  }

  func draggingSession(_ session: NSDraggingSession,
                       sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
    context == .outsideApplication ? .copy : []
  }

  func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint,
                       operation: NSDragOperation) {
    MainActor.assumeIsolated { active = false }
  }
}

extension View {
  /// Drags the files `files()` names once the mouse moves 4 pt with the button
  /// down. Simultaneous, so a tap on the same view is unchanged; `files` runs
  /// once, when the drag starts.
  func fileDrag(_ files: @escaping @MainActor () -> [(path: String, image: CGImage?)]) -> some View {
    simultaneousGesture(
      DragGesture(minimumDistance: 4)
        .onChanged { _ in
          MainActor.assumeIsolated { FileDragSource.shared.begin(files) }
        }
    )
  }
}
