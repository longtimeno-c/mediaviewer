// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Import.bundle's principal class (docs/design/18 "Mac chrome"): the host
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
  /// Jobs whose preset turned "Notify me when done" off.
  private var quiet = Set<UInt64>()
  /// A failed or interrupted import, for the command bar until Import opens.
  private var problem: String?
  /// When the jobs were last read (systemUptime): events and the fallback
  /// timer do not both read them (issue #194).
  private var lastTick: TimeInterval = 0

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
    m.duplicates.chrome = self
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
      if problem != nil {
        problem = nil  // the summary in the window says it now
        if jobs.isEmpty { setStatus(nil) }
      }
      model.refreshSources(select: source.isEmpty ? nil : source)
      model.checkUnfinished()
      model.showFirstUseExplainerIfNeeded()
    }
  }

  /// docs/design/25: the commands the manifest contributes, by their ids, with the
  /// payload each row asked for (an array of paths here).
  @objc(runCommand:payload:)
  public func runCommand(_ name: String, payload json: String) -> Bool {
    let paths = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String] ?? []
    switch name {
    case "open":
      open(source: "", marks: paths)
      return true
    case "import_now":
      if paths.isEmpty { return false }
      importNow(json)
      return true
    default:
      return false
    }
  }

  @objc(importNow:)
  public func importNow(_ pathsJSON: String) {
    guard let table else { return }
    var id: UInt64 = 0
    if table.call({ table.api.pointee.import_now!(table.ctx, pathsJSON, &id) }) == MV_OK { track(id, label: "marked files") }
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
      case MV_ADDON_EVENT_DUPLICATES_DONE.rawValue:
        tick()
        model?.duplicates.refresh(identifier)
        duplicatesFinished(identifier, state: UInt32(truncatingIfNeeded: payload))
      case MV_ADDON_EVENT_DUPLICATE_TRASHED.rawValue:
        model?.duplicates.refresh(identifier)
      default:
        break
      }
    }
  }

  /// Before the host unloads the pack. Closes the table: a detached call
  /// still running (a RAW thumbnail, an eject, a sources read) fails instead
  /// of calling into an unloaded library, and this waits (at most 2 s) for the
  /// calls already inside the pack (issue #193).
  @objc public func shutdown() {
    _ = close(wait: 2)
  }

  /// Quit: the same, without waiting. True when no call was in flight; false
  /// tells the host to leave the pack running for the process exit rather
  /// than free it under that call.
  @objc public func shutdownForQuit() -> Bool {
    close(wait: 0)
  }

  private func close(wait: TimeInterval) -> Bool {
    MainActor.assumeIsolated {
      timer?.invalidate()
      timer = nil
      window?.close()
      window = nil
      model = nil
      setStatus(nil)
      let idle = table?.close(timeout: wait) ?? true
      table = nil
      return idle
    }
  }

  // MARK: host services (selectors on the host object, addons_mac.mm)

  func openInViewer(_ path: String) {
    host?.perform(NSSelectorFromString("openInViewer:"), with: path as NSString)
  }

  private func setStatus(_ text: String?) {
    host?.perform(NSSelectorFromString("setStatus:"), with: (text ?? "") as NSString)
  }

  /// `notify`: the preset's "Notify me when done" for a job the window
  /// started (issue #194); a card's auto-import and Import Now always say.
  func track(_ job: UInt64, label: String, notify: Bool = true) {
    jobs[job] = label
    if !notify { quiet.insert(job) }
    if timer == nil {
      // A fallback only: progress arrives as JOB_PROGRESS events (at most
      // 4 Hz while bytes move). This catches a copy that is paused or waiting,
      // which posts none, and ticks only when no event has for a second.
      timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated {
          guard let self, ProcessInfo.processInfo.systemUptime - self.lastTick >= 0.9 else { return }
          self.tick()
        }
      }
    }
    tick()
  }

  /// A change the engine posts no event for (Pause): read the jobs now.
  func pollNow() { tick() }

  private func tick() {
    lastTick = ProcessInfo.processInfo.systemUptime
    guard let table else { return }
    var line: String?
    for job in jobs.keys {
      var p = mv_import_progress()
      guard table.call({ table.api.pointee.progress!(table.ctx, job, &p) }) == MV_OK else { jobs[job] = nil; continue }
      MainActor.assumeIsolated {
        model?.progressed(job, p)
        model?.duplicates.progressed(job, p)
      }
      if p.state == MV_IMPORT_JOB_RUNNING.rawValue || p.state == MV_IMPORT_JOB_PAUSED.rawValue ||
          p.state == MV_IMPORT_JOB_QUEUED.rawValue {
        let pct = p.bytes_total == 0 ? 0 : Int(100 * Double(p.bytes_read) / Double(p.bytes_total))
        if jobs[job] == "duplicates" {
          line = "Finding duplicates · \(pct)%"
        } else {
          line = (p.state == MV_IMPORT_JOB_PAUSED.rawValue ? "Import paused · " : "Importing · ") + "\(pct)%"
        }
      }
    }
    setStatus(line ?? problem)
    if jobs.isEmpty { timer?.invalidate(); timer = nil }
  }

  private func duplicatesFinished(_ job: UInt64, state: UInt32) {
    guard jobs.removeValue(forKey: job) != nil else { return }
    let headline = MainActor.assumeIsolated { () -> String? in
      guard let d = model?.duplicates, d.job == job else { return nil }
      return d.headline
    }
    let text: String
    if state == MV_IMPORT_JOB_DONE.rawValue, let headline {
      text = headline
    } else {
      text = state == MV_IMPORT_JOB_CANCELLED.rawValue ? "Stopped." : "The folder could not be read."
    }
    host?.perform(NSSelectorFromString("notifyTitle:body:"), with: "Find duplicates" as NSString,
                  with: text as NSString)
    if jobs.isEmpty { setStatus(problem) }
  }

  private func finished(_ job: UInt64, state: UInt32) {
    MainActor.assumeIsolated { model?.finished(job) }
    guard jobs.removeValue(forKey: job) != nil else { return }
    let notify = quiet.remove(job) == nil
    let text: String
    switch state {
    case MV_IMPORT_JOB_DONE.rawValue: text = "Finished. Every copy verified."
    case MV_IMPORT_JOB_FAILED.rawValue: text = "Finished with files that could not be copied."
    case MV_IMPORT_JOB_INTERRUPTED.rawValue: text = "Interrupted. Reconnect and resume in Import."
    default: text = "Cancelled. Nothing partial was left behind."
    }
    if notify {
      host?.perform(NSSelectorFromString("notifyTitle:body:"), with: "Import" as NSString, with: text as NSString)
    }
    // A failure the window is not showing stays in the command bar until
    // Import is opened again (issue #194), notification or not.
    let failed = state == MV_IMPORT_JOB_FAILED.rawValue || state == MV_IMPORT_JOB_INTERRUPTED.rawValue
    if failed && !MainActor.assumeIsolated({ window?.isVisible == true }) {
      problem = state == MV_IMPORT_JOB_FAILED.rawValue
        ? "Import finished with files not copied · open Import"
        : "Import interrupted · open Import to resume"
    }
    if jobs.isEmpty { setStatus(problem) }
  }
}
