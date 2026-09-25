// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Import.bundle's principal class (plan/18 "Mac chrome"): the host
// (src/shell/addons_mac.mm) instantiates it with NSBundle and talks to it by
// message send. It owns the one Import window and every running job, so
// closing the window keeps an import running with a line in the command bar.
import AppKit
import CImportApi
import SwiftUI

@objc(MVImportChrome)
public final class MVImportChrome: NSObject {
  private var table: ImportTable?
  private var host: NSObject?
  private var model: ImportModel?
  private var window: NSWindow?
  private var timer: Timer?
  private var jobs: [UInt64: String] = [:]

  @objc public override init() { super.init() }

  @objc(attachWithTable:host:)
  public func attach(table: NSValue, host: NSObject) {
    guard let raw = table.pointerValue else { return }
    self.table = ImportTable(raw.assumingMemoryBound(to: mv_import_api.self))
    self.host = host
  }

  @MainActor private func ensureModel() -> ImportModel? {
    if let model { return model }
    guard let table else { return nil }
    let m = ImportModel(table: table)
    m.chrome = self
    model = m
    return m
  }

  @objc(openWithSource:marks:)
  public func open(source: String, marks: [String]) {
    MainActor.assumeIsolated {
      guard let model = ensureModel() else { return }
      model.marks = marks
      if window == nil {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 760),
                         styleMask: [.titled, .closable, .resizable, .miniaturizable],
                         backing: .buffered, defer: false)
        w.title = "Import"
        w.isReleasedWhenClosed = false
        w.contentView = NSHostingView(rootView: ImportView(model: model))
        w.center()
        window = w
      }
      window?.makeKeyAndOrderFront(nil)
      model.refreshSources(select: source.isEmpty ? nil : source)
      model.checkUnfinished()
    }
  }

  @objc(importNow:)
  public func importNow(_ pathsJSON: String) {
    guard let table else { return }
    var id: UInt64 = 0
    if table.api.pointee.import_now!(table.ctx, pathsJSON, &id) == MV_OK { track(id, label: "marked files") }
  }

  @objc(deliverEvent:status:identifier:payload:)
  public func deliver(event: UInt32, status: UInt32, identifier: UInt64, payload: Int64) {
    MainActor.assumeIsolated {
      switch event {
      case MV_ADDON_EVENT_VOLUME_ARRIVED.rawValue:
        if payload == Int64(MV_IMPORT_ARRIVAL_AUTO.rawValue) {
          track(identifier, label: "auto-import")
        } else if payload == Int64(MV_IMPORT_ARRIVAL_OPEN.rawValue), let table {
          let root = table.path { table.api.pointee.arrival_root!(table.ctx, identifier, $0, $1) } ?? ""
          if !root.isEmpty { open(source: root, marks: model?.marks ?? []) }
        }
        model?.refreshSources(select: nil)
      case MV_ADDON_EVENT_VOLUME_REMOVED.rawValue:
        model?.refreshSources(select: nil)
      case MV_ADDON_EVENT_SCAN_DONE.rawValue:
        if identifier == 0 { model?.refreshSources(select: nil) } else { model?.scanDone(identifier, status) }
      case MV_ADDON_EVENT_PLAN_READY.rawValue:
        model?.planReady(identifier, status)
      case MV_ADDON_EVENT_JOB_PROGRESS.rawValue:
        tick()
      case MV_ADDON_EVENT_JOB_DONE.rawValue, MV_ADDON_EVENT_VERIFY_DONE.rawValue:
        tick()
        finished(identifier, state: UInt32(truncatingIfNeeded: payload))
      default:
        break
      }
    }
  }

  @objc public func shutdown() {
    MainActor.assumeIsolated {
      timer?.invalidate()
      timer = nil
      window?.close()
      window = nil
      model = nil
      setStatus(nil)
      table = nil
    }
  }

  // MARK: host services (selectors on the host object, addons_mac.mm)

  func openInViewer(_ path: String) {
    host?.perform(NSSelectorFromString("openInViewer:"), with: path as NSString)
  }

  private func setStatus(_ text: String?) {
    host?.perform(NSSelectorFromString("setStatus:"), with: (text ?? "") as NSString)
  }

  func track(_ job: UInt64, label: String) {
    jobs[job] = label
    if timer == nil {
      timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated { self?.tick() }
      }
    }
    tick()
  }

  private func tick() {
    guard let table else { return }
    var line: String?
    for job in jobs.keys {
      var p = mv_import_progress()
      guard table.api.pointee.progress!(table.ctx, job, &p) == MV_OK else { jobs[job] = nil; continue }
      MainActor.assumeIsolated { model?.progressed(job, p) }
      if p.state == MV_IMPORT_JOB_RUNNING.rawValue || p.state == MV_IMPORT_JOB_PAUSED.rawValue ||
          p.state == MV_IMPORT_JOB_QUEUED.rawValue {
        let pct = p.bytes_total == 0 ? 0 : Int(100 * Double(p.bytes_read) / Double(p.bytes_total))
        line = (p.state == MV_IMPORT_JOB_PAUSED.rawValue ? "Import paused · " : "Importing · ") + "\(pct)%"
      }
    }
    setStatus(line)
    if jobs.isEmpty { timer?.invalidate(); timer = nil }
  }

  private func finished(_ job: UInt64, state: UInt32) {
    MainActor.assumeIsolated { model?.finished(job) }
    guard jobs.removeValue(forKey: job) != nil else { return }
    let text: String
    switch state {
    case MV_IMPORT_JOB_DONE.rawValue: text = "Finished. Every copy verified."
    case MV_IMPORT_JOB_FAILED.rawValue: text = "Finished with files that could not be copied."
    case MV_IMPORT_JOB_INTERRUPTED.rawValue: text = "Interrupted. Reconnect and resume in Import."
    default: text = "Cancelled. Nothing partial was left behind."
    }
    host?.perform(NSSelectorFromString("notifyTitle:body:"), with: "Import" as NSString, with: text as NSString)
    if jobs.isEmpty { setStatus(nil) }
  }
}
