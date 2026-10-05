// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Add-ons → From others (docs/design/25): add-ons from other makers, one
// `.mvaddon` file each, installed from a file or a link. The Mac twin of
// IslandHost.OpenAddons.cs.
//
// The core reads, checks and installs (src/addon/open_store.h, through
// mv_open_addons_*); this file asks, downloads and shows. It decides nothing
// about trust: what the sheet says an add-on is, can and cannot do, and why
// one is refused, all come from the core's JSON, the same on both hosts.
//
// A link is fetched only on a click, with a plain GET: no cookies, no cache,
// one fixed User-Agent, nothing about the user's files (rule 6). Nothing is
// ever fetched in the background: an add-on's maker cannot learn when
// MediaViewer runs.
import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers
import MVChromeBridge

struct OpenAddon: Identifiable, Equatable {
  struct Theme: Equatable {
    let id: String
    let name: String
  }
  let folder: String
  let addonID: String
  let name: String
  let version: String
  let state: String        // ok | needs_update | invalid
  let description: String
  let licence: String
  let size: Int
  let updateURL: String
  let publisher: String
  let publisherURL: String
  let fingerprint: String
  let adds: String
  let can: [String]
  let cannot: String
  let themes: [Theme]

  var id: String { folder }

  init(_ obj: [String: Any]) {
    let publisherObj = obj["publisher"] as? [String: Any] ?? [:]
    addonID = obj["id"] as? String ?? ""
    folder = obj["folder"] as? String ?? addonID
    name = obj["name"] as? String ?? addonID
    version = obj["version"] as? String ?? ""
    state = obj["state"] as? String ?? ""
    description = obj["description"] as? String ?? ""
    licence = obj["licence"] as? String ?? ""
    size = obj["size"] as? Int ?? 0
    updateURL = obj["update_url"] as? String ?? ""
    publisher = publisherObj["name"] as? String ?? ""
    publisherURL = publisherObj["url"] as? String ?? ""
    fingerprint = publisherObj["fingerprint"] as? String ?? ""
    adds = obj["adds"] as? String ?? ""
    can = obj["can"] as? [String] ?? []
    cannot = obj["cannot"] as? String ?? ""
    themes = (obj["themes"] as? [[String: Any]] ?? []).compactMap {
      guard let id = $0["id"] as? String else { return nil }
      return Theme(id: id, name: $0["name"] as? String ?? id)
    }
  }

  /// "acme.example" of "https://acme.example/about", for the sheet's From line.
  var publisherHost: String { URL(string: publisherURL)?.host ?? "" }
}

/// A package that has been looked at and is waiting for an answer.
struct OpenAddonOffer: Equatable {
  let path: String
  /// A download of ours, deleted once answered; a file the user picked stays.
  let temporary: Bool
  let ok: Bool
  let message: String
  let sha256: String
  let relation: String
  let installedVersion: String
  let addon: OpenAddon?
}

@MainActor
final class OpenAddonStore: ObservableObject {
  static let shared = OpenAddonStore()

  @Published private(set) var installed: [OpenAddon] = []
  /// The first read of what is installed has landed.
  @Published private(set) var known = false
  @Published private(set) var offer: OpenAddonOffer?
  @Published private(set) var busy = false
  @Published private(set) var progress: AddonChannel.Phase?
  @Published var message = ""
  @Published var enteringLink = false
  @Published var link = ""
  @Published var confirmingRemove: String?

  private init() {}

  /// Every theme of every add-on that verifies, for Settings → Appearance.
  var themes: [(key: String, name: String)] {
    installed.filter { $0.state == "ok" }.flatMap { addon in
      addon.themes.map { theme in
        (key: ThemeStore.key(addon: addon.addonID, theme: theme.id),
         name: addon.themes.count == 1 && theme.name == addon.name
           ? theme.name : "\(theme.name) (\(addon.name))")
      }
    }
  }

  func refresh() {
    Task.detached(priority: .utility) {
      let json = OpenAddonBridge.read { mv_open_addons_list($0, $1) }
      let rows = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [[String: Any]] ?? []
      let list = rows.map(OpenAddon.init)
      await MainActor.run {
        self.installed = list
        self.known = true
      }
    }
  }

  // MARK: from a file

  func chooseFile() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    panel.message = "Choose a MediaViewer add-on (.mvaddon)."
    if let type = UTType(filenameExtension: "mvaddon") { panel.allowedContentTypes = [type] }
    guard panel.runModal() == .OK, let url = panel.url else { return }
    offer(file: url.path)
  }

  func offer(file path: String) {
    inspect(path: path, temporary: false, updating: nil)
  }

  // MARK: from a link

  static func isHTTPS(_ text: String) -> URL? {
    guard let url = URL(string: text), url.scheme?.lowercased() == "https",
          let host = url.host, !host.isEmpty, url.user == nil, url.password == nil
    else { return nil }
    return url
  }

  func offerLink() {
    let text = link.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let url = Self.isHTTPS(text) else {
      message = "A link to an add-on starts with https://."
      return
    }
    download(url, updating: nil)
  }

  func checkForUpdate(_ addon: OpenAddon) {
    guard let url = Self.isHTTPS(addon.updateURL) else { return }
    download(url, updating: addon)
  }

  private func download(_ url: URL, updating: OpenAddon?) {
    guard !busy else { return }
    busy = true
    message = ""
    enteringLink = false
    progress = .downloading(done: 0, total: -1)
    let target = FileManager.default.temporaryDirectory
      .appendingPathComponent("mediaviewer-addon-\(UUID().uuidString).mvaddon")
    Task.detached(priority: .userInitiated) {
      var failure = ""
      do {
        let code = try await OpenAddonDownload.run(url, to: target) { done, total in
          Task { @MainActor in self.progress = .downloading(done: done, total: total) }
        }
        if code == 404 {
          failure = "There is no add-on at that link."
        } else if code != 200 {
          failure = "The server did not send the add-on."
        }
      } catch OpenAddonDownload.Refused.tooLarge {
        failure = "That download is larger than 64 MB, the most MediaViewer installs from a link."
      } catch OpenAddonDownload.Refused.notHTTPS {
        failure = "The link led somewhere that is not https, so it was not followed."
      } catch {
        failure = "The add-on could not be downloaded. Check the link and the connection."
      }
      let failed = failure
      await MainActor.run {
        self.progress = nil
        if !failed.isEmpty {
          try? FileManager.default.removeItem(at: target)
          self.busy = false
          self.message = failed
          self.enteringLink = updating == nil  // the link is still there to fix
          return
        }
        self.busy = false
        self.inspect(path: target.path, temporary: true, updating: updating)
      }
    }
  }

  // MARK: the answer

  private func inspect(path: String, temporary: Bool, updating: OpenAddon?) {
    guard !busy else { return }
    busy = true
    message = ""
    progress = .checking
    discardOffer()
    Task.detached(priority: .userInitiated) {
      let json = OpenAddonBridge.read { mv_open_addons_inspect(path, $0, $1) }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      let found = OpenAddonOffer(
        path: path, temporary: temporary, ok: obj["ok"] as? Bool ?? false,
        message: obj["message"] as? String ?? "This file could not be read.",
        sha256: obj["sha256"] as? String ?? "", relation: obj["relation"] as? String ?? "",
        installedVersion: obj["installed_version"] as? String ?? "",
        addon: obj["publisher"] != nil ? OpenAddon(obj) : nil)
      await MainActor.run {
        self.busy = false
        self.progress = nil
        if let updating {
          self.offer = found  // so a download of ours is deleted with it
          guard let served = found.addon, served.addonID == updating.addonID else {
            // An update link that serves something else is not an update.
            self.discardOffer()
            self.message = "The update link of \(updating.name) did not serve \(updating.name), so nothing was installed."
            return
          }
          if found.ok, found.relation == "repair" {
            self.discardOffer()
            self.message = "\(updating.name) \(updating.version) is the newest version."
          }
          return
        }
        self.offer = found
      }
    }
  }

  func confirm() {
    guard let o = offer, o.ok, !busy else { return }
    busy = true
    progress = .installing
    Task.detached(priority: .userInitiated) {
      // Once: never the ask-for-the-size-first call, which would install twice.
      let json = OpenAddonBridge.read(capacity: 4096, retry: false) {
        mv_open_addons_install(o.path, o.sha256, $0, $1)
      }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      let ok = obj["ok"] as? Bool ?? false
      let why = obj["message"] as? String ?? ""
      await MainActor.run {
        self.busy = false
        self.progress = nil
        let name = o.addon?.name ?? "The add-on"
        let version = o.addon?.version ?? ""
        if ok {
          self.message = o.relation == "update" ? "\(name) updated to \(version)."
            : "\(name) \(version) installed."
          if let themes = o.addon?.themes, !themes.isEmpty, o.relation == "fresh" {
            self.message += " Choose its theme under Appearance."
          }
        } else {
          self.message = why.isEmpty ? "\(name) could not be installed." : why
        }
        self.discardOffer()
        self.refresh()
        ThemeStore.shared.addonsChanged()
      }
    }
  }

  func cancel() {
    discardOffer()
    message = ""
  }

  private func discardOffer() {
    if let o = offer, o.temporary { try? FileManager.default.removeItem(atPath: o.path) }
    offer = nil
  }

  func remove(_ addon: OpenAddon) {
    confirmingRemove = nil
    guard !busy else { return }
    busy = true
    let folder = addon.folder
    let name = addon.name
    Task.detached(priority: .userInitiated) {
      let removed = mv_open_addons_remove(folder)
      await MainActor.run {
        self.busy = false
        self.message = removed ? "\(name) removed." : "\(name) could not be removed."
        self.refresh()
        ThemeStore.shared.addonsChanged()
      }
    }
  }
}

/// One GET of a link the user gave: https only (redirects too), 64 MB at most,
/// no cookies, no cache. Foundation only.
enum OpenAddonDownload {
  enum Refused: Error { case tooLarge, notHTTPS }
  static let maxBytes: Int64 = 64 << 20

  static func run(_ url: URL, to destination: URL,
                  progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> Int {
    let delegate = Delegate(destination: destination, progress: progress)
    let session = URLSession(configuration: AddonChannel.configuration, delegate: delegate,
                             delegateQueue: nil)
    defer { session.finishTasksAndInvalidate() }
    let task = session.downloadTask(with: url)
    return try await withTaskCancellationHandler {
      try await withCheckedThrowingContinuation { (c: CheckedContinuation<Int, Error>) in
        delegate.begin(c)
        task.resume()
      }
    } onCancel: {
      task.cancel()
    }
  }

  private final class Delegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let destination: URL
    private let progress: @Sendable (Int64, Int64) -> Void
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Int, Error>?
    private var refusal: Error?
    private var moveError: Error?
    private var lastReport = DispatchTime(uptimeNanoseconds: 0)

    init(destination: URL, progress: @escaping @Sendable (Int64, Int64) -> Void) {
      self.destination = destination
      self.progress = progress
    }

    func begin(_ c: CheckedContinuation<Int, Error>) {
      lock.lock()
      continuation = c
      lock.unlock()
    }

    private func finish(_ result: Result<Int, Error>) {
      lock.lock()
      let c = continuation
      continuation = nil
      lock.unlock()
      c?.resume(with: result)
    }

    // A redirect is followed only to https: a link that was safe to read must
    // not end somewhere that is not.
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
      if request.url?.scheme?.lowercased() == "https" {
        completionHandler(request)
      } else {
        refusal = Refused.notHTTPS
        completionHandler(nil)
        task.cancel()
      }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
      if totalBytesWritten > OpenAddonDownload.maxBytes || totalBytesExpectedToWrite > OpenAddonDownload.maxBytes {
        refusal = Refused.tooLarge
        downloadTask.cancel()
        return
      }
      let now = DispatchTime.now()
      guard now.uptimeNanoseconds &- lastReport.uptimeNanoseconds >= 100_000_000 else { return }
      lastReport = now
      progress(totalBytesWritten, totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : -1)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
      guard (downloadTask.response as? HTTPURLResponse)?.statusCode == 200 else { return }
      let size = (try? FileManager.default.attributesOfItem(atPath: location.path)[.size] as? Int64) ?? 0
      if size > OpenAddonDownload.maxBytes {
        refusal = Refused.tooLarge
        return
      }
      do {
        try FileManager.default.moveItem(at: location, to: destination)
      } catch {
        moveError = error
      }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
      if let refusal {
        finish(.failure(refusal))
      } else if let error {
        finish(.failure(error))
      } else if let moveError {
        finish(.failure(moveError))
      } else {
        finish(.success((task.response as? HTTPURLResponse)?.statusCode ?? 0))
      }
    }
  }
}

/// Settings → Add-ons → From others.
struct OpenAddonsSection: View {
  @ObservedObject var store = OpenAddonStore.shared
  @ObservedObject var settings = SettingsStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("From others").font(MVTheme.font()).foregroundStyle(MVTheme.title)
      note("Add-ons made by other people, installed from a file or a link. MediaViewer does not check them or who made them. The ones it installs are data, such as themes: they cannot run code, read your files, or use the network.")

      if let offer = store.offer {
        OpenAddonSheet(offer: offer)
      } else {
        ForEach(store.installed) { addon in
          row(addon)
        }
        if store.enteringLink {
          HStack {
            TextField("https://…/name.mvaddon", text: $store.link)
              .textFieldStyle(.roundedBorder).font(MVTheme.font(13))
              .frame(maxWidth: 420)
              .onSubmit { store.offerLink() }
            Button("Download") { store.offerLink() }.disabled(store.busy)
            Button("Cancel") { store.enteringLink = false }
          }
          note("The server you name will see this computer's network address, as any download does. Nothing else is sent.")
        } else {
          HStack {
            Button("Install from file…") { store.chooseFile() }.disabled(store.busy)
            Button("Install from link…") { store.enteringLink = true }.disabled(store.busy)
          }
        }
      }
      if let phase = store.progress {
        AddonProgressView(phase: phase)
      } else if !store.message.isEmpty {
        note(store.message)
      }
    }
    .onAppear { if settings.visible { store.refresh() } }
    .onChange(of: settings.visible) { _, visible in if visible { store.refresh() } }
  }

  @ViewBuilder private func row(_ addon: OpenAddon) -> some View {
    VStack(alignment: .leading, spacing: 4) {
      Text("\(addon.name) \(addon.version)").font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
      if !addon.publisher.isEmpty {
        note("From \(addon.publisher) · key \(addon.fingerprint) · \(addon.adds)")
      }
      if addon.state == "needs_update" {
        note("Needs a newer MediaViewer, so it is not in use.")
      } else if addon.state != "ok" {
        note("This copy did not pass verification, so it is not in use.")
      }
      if store.confirmingRemove == addon.folder {
        HStack {
          Button("Remove \(addon.name)") { store.remove(addon) }
          Button("Cancel") { store.confirmingRemove = nil }
        }
      } else {
        HStack {
          if !addon.updateURL.isEmpty, addon.state != "invalid" {
            Button("Check for update") { store.checkForUpdate(addon) }.disabled(store.busy)
              .help("Asks \(URL(string: addon.updateURL)?.host ?? "its maker's server") for a newer version. Only when you click.")
          }
          Button("Remove…") { store.confirmingRemove = addon.folder }.disabled(store.busy)
        }
      }
    }
    .padding(.vertical, 4)
  }

  private func note(_ text: String) -> some View {
    Text(text).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      .fixedSize(horizontal: false, vertical: true)
  }
}

/// What a package is, and the question (docs/design/25 "Identity and trust"). Every
/// line but the description comes from the core. Cancel is the default
/// button: Return does not install.
struct OpenAddonSheet: View {
  /// Settings scrolls here when a package is offered.
  static let anchor = "open-addon-sheet"
  let offer: OpenAddonOffer
  @ObservedObject var store = OpenAddonStore.shared

  private var title: String {
    guard let a = offer.addon else { return "This add-on cannot be installed" }
    if !offer.ok { return "“\(a.name)” \(a.version) cannot be installed" }
    switch offer.relation {
    case "update": return "Update “\(a.name)” from \(offer.installedVersion) to \(a.version)?"
    case "repair": return "Install “\(a.name)” \(a.version) again?"
    default: return "Install “\(a.name)” \(a.version)?"
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(title).font(MVTheme.font(16)).fontWeight(.semibold).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      if let a = offer.addon {
        if !a.description.isEmpty {
          Text(a.description).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 16, verticalSpacing: 4) {
          line("From", a.publisherHost.isEmpty ? a.publisher : "\(a.publisher) · \(a.publisherHost)")
          line("Key", a.fingerprint)
          line("Adds", a.adds)
          ForEach(a.can, id: \.self) { line("Can", $0) }
          line("Cannot", a.cannot)
          line("Size", "\(ByteCountFormatter.string(fromByteCount: Int64(a.size), countStyle: .file)) · \(a.licence)")
        }
      }
      if offer.ok {
        Text("MediaViewer has not checked this add-on or who made it.")
          .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
          .fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("Cancel") { store.cancel() }.keyboardShortcut(.defaultAction)
          Button(offer.relation == "update" ? "Update" : "Install") { store.confirm() }
            .disabled(store.busy)
        }
      } else {
        Text(offer.message).font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
          .fixedSize(horizontal: false, vertical: true)
        Button("Close") { store.cancel() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(14)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 8).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 8).stroke(MVTheme.hairline, lineWidth: 1))
    .accessibilityElement(children: .contain)
    .accessibilityLabel(title)
    .id(Self.anchor)
  }

  private func line(_ label: String, _ value: String) -> some View {
    GridRow {
      Text(label).font(MVTheme.font(13)).foregroundStyle(MVTheme.body)
      Text(value).font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
        .textSelection(.enabled)
    }
  }
}
