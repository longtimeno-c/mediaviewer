// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An add-on's notices and its keep-alive, on the Mac (#452; docs/design/28 "In
// the background"). The core's registry holds what add-ons asked for
// (mv_chrome_slots_notices, change bit 64); this draws it:
//
// - a notice is a UNUserNotificationCenter notification, unless its screen is
//   the front window of the active app (the card is in plain sight). Clicking
//   it opens that screen; its one button (Driftwire's Decline) goes back to
//   the add-on. Without permission, or in the unbundled lab binary (which has
//   no notification centre), the screen opens instead, as it did before #452;
// - permission is asked when an add-on asks (a toggle turned on), never at
//   launch;
// - while an add-on keeps the process, a menu-bar item: its label, Open
//   <screen>, Open MediaViewer, Quit MediaViewer. main_mac.mm hides the
//   viewer rather than quitting while the list is not empty.
//
// Nothing here runs until the registry has a notice, an ask or a keep: with
// Driftwire's toggles off the notification centre is never touched.
// The Windows twin is IslandHost.Notices.cs.
import AppKit
import MVChromeBridge
@preconcurrency import UserNotifications

@MainActor
final class AddonNotices: NSObject, UNUserNotificationCenterDelegate {
  static let shared = AddonNotices()

  private struct Notice: Equatable {
    var addon, id, screen, title, body, action, actionLabel: String
    var revision: Int
    var key: String { "\(addon)/\(id)" }
  }
  private struct Keep: Equatable { var addon, screen, title, label: String }

  private var shown: [String: Notice] = [:]
  private var asked = 0
  private var keeps: [Keep] = []
  private var statusItem: NSStatusItem?
  private var categories: Set<String> = []
  private var centerReady = false

  /// The lab binary has no bundle: UNUserNotificationCenter.current() raises there.
  private static let canNotify: Bool = {
    Bundle.main.bundleURL.pathExtension == "app" && Bundle.main.bundleIdentifier != nil
  }()

  private var center: UNUserNotificationCenter? {
    guard Self.canNotify else { return nil }
    let c = UNUserNotificationCenter.current()
    if !centerReady {
      centerReady = true
      c.delegate = self
    }
    return c
  }

  /// Bit 64: re-read the table.
  func read(_ doc: [String: Any]) {
    let ask = doc["ask"] as? Int ?? 0
    if ask > asked {
      asked = ask
      center?.requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }
    let k: [Keep] = (doc["keep"] as? [[String: Any]] ?? []).compactMap {
      guard let a = $0["addon"] as? String, let s = $0["screen"] as? String else { return nil }
      return Keep(addon: a, screen: s, title: $0["title"] as? String ?? s, label: $0["label"] as? String ?? "")
    }
    if k != keeps {
      keeps = k
      syncStatusItem()
      mv_chrome_background_changed()
    }
    let list: [Notice] = (doc["notices"] as? [[String: Any]] ?? []).compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String, let s = $0["screen"] as? String else {
        return nil
      }
      return Notice(addon: a, id: id, screen: s, title: $0["title"] as? String ?? "", body: $0["body"] as? String ?? "",
                    action: $0["action"] as? String ?? "", actionLabel: $0["action_label"] as? String ?? "",
                    revision: $0["revision"] as? Int ?? 0)
    }
    let now = Dictionary(uniqueKeysWithValues: list.map { ($0.key, $0) })
    let gone = shown.keys.filter { now[$0] == nil }
    if !gone.isEmpty, let c = center {
      c.removeDeliveredNotifications(withIdentifiers: gone)
      c.removePendingNotificationRequests(withIdentifiers: gone)
    }
    for key in gone { shown[key] = nil }
    for n in list where shown[n.key] != n {
      shown[n.key] = n
      present(n)
    }
  }

  private func present(_ n: Notice) {
    let address = ScreenAddress(addon: n.addon, screen: n.screen)
    // In plain sight: the card itself is the notice.
    if NSApp.isActive, SlotWindows.shared.isFront(address) { return }
    guard let c = center else { return fallback(n) }
    c.getNotificationSettings { settings in
      let allowed = settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional
      Task { @MainActor in
        guard self.shown[n.key] == n else { return }  // replaced or taken back meanwhile
        allowed ? self.post(n, on: c) : self.fallback(n)
      }
    }
  }

  private func post(_ n: Notice, on c: UNUserNotificationCenter) {
    let content = UNMutableNotificationContent()
    content.title = n.title
    content.body = n.body
    content.sound = .default
    content.userInfo = ["addon": n.addon, "id": n.id, "screen": n.screen]
    if !n.action.isEmpty {
      let category = "mv.notice.\(n.addon).\(n.action)"
      if !categories.contains(category) {
        categories.insert(category)
        let action = UNNotificationAction(identifier: n.action, title: n.actionLabel, options: [.destructive])
        let made = UNNotificationCategory(identifier: category, actions: [action], intentIdentifiers: [])
        c.getNotificationCategories { have in
          c.setNotificationCategories(have.filter { $0.identifier != category }.union([made]))
        }
      }
      content.categoryIdentifier = category
    }
    c.add(UNNotificationRequest(identifier: n.key, content: content, trigger: nil)) { error in
      guard error != nil else { return }
      Task { @MainActor in self.fallback(n) }
    }
  }

  /// No notifications: the screen opens, as before #452, and the Dock bounces.
  private func fallback(_ n: Notice) {
    mv_chrome_background_window_shown()
    SlotStore.shared.openScreen(addon: n.addon, screen: n.screen)
    NSApp.requestUserAttention(.informationalRequest)
  }

  // MARK: UNUserNotificationCenterDelegate

  nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                          withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
    // The app is active but the window is behind another one: show the banner.
    completionHandler([.banner, .list, .sound])
  }

  nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                          withCompletionHandler completionHandler: @escaping () -> Void) {
    let info = response.notification.request.content.userInfo
    let addon = info["addon"] as? String ?? "", id = info["id"] as? String ?? ""
    let screen = info["screen"] as? String ?? ""
    let action = response.actionIdentifier
    Task { @MainActor in
      defer { completionHandler() }
      guard !addon.isEmpty else { return }
      switch action {
      case UNNotificationDefaultActionIdentifier:
        mv_chrome_background_window_shown()
        SlotStore.shared.openScreen(addon: addon, screen: screen)
      case UNNotificationDismissActionIdentifier:
        break
      default:
        _ = addon.withCString { a in id.withCString { i in action.withCString { mv_chrome_slots_notice_action(a, i, $0) } } }
      }
    }
  }

  // MARK: the menu-bar item

  private func syncStatusItem() {
    guard let keep = keeps.first else {
      if let item = statusItem { NSStatusBar.system.removeStatusItem(item) }
      statusItem = nil
      return
    }
    let item = statusItem ?? NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    statusItem = item
    if let button = item.button {
      let image = NSImage(systemSymbolName: "dot.radiowaves.left.and.right", accessibilityDescription: keep.label)
      image?.isTemplate = true
      button.image = image
      button.toolTip = keep.label
    }
    handlers.removeAll()
    let menu = NSMenu()
    let head = NSMenuItem(title: keep.label, action: nil, keyEquivalent: "")
    head.isEnabled = false
    menu.addItem(head)
    menu.addItem(.separator())
    for k in keeps {
      menu.addItem(entry("Open \(k.title)") {
        mv_chrome_background_window_shown()
        SlotStore.shared.openScreen(addon: k.addon, screen: k.screen)
      })
    }
    menu.addItem(entry("Open MediaViewer") { mv_chrome_background_show_viewer() })
    menu.addItem(.separator())
    menu.addItem(entry("Quit MediaViewer") { mv_chrome_background_quit() })
    item.menu = menu
  }

  private final class Handler: NSObject {
    let run: () -> Void
    init(_ run: @escaping () -> Void) { self.run = run }
    @objc func fire(_ sender: Any?) { run() }
  }
  private var handlers: [Handler] = []

  private func entry(_ title: String, _ run: @escaping @MainActor () -> Void) -> NSMenuItem {
    let h = Handler { MainActor.assumeIsolated { run() } }
    handlers.append(h)
    let item = NSMenuItem(title: title, action: #selector(Handler.fire(_:)), keyEquivalent: "")
    item.target = h
    return item
  }
}
