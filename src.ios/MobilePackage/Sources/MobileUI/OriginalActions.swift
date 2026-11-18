// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The original of a photo or a clip, two ways: saved to Photos, or handed to the share sheet
// (AirDrop, Messages, Files, "Save Video"...). One transfer at a time, its progress in a line; the
// viewer and the clip screen both use it.
import MobileKit
import SwiftUI
import UIKit

@MainActor
@Observable
final class OriginalTransfer {
  enum Kind { case save, share }
  enum State: Equatable {
    case running(TransferProgress?)
    case outcome(ProgressOutcome)
  }

  /// The item the transfer is about: swiping away leaves it running, out of sight.
  private(set) var item: MediaItem?
  private(set) var kind: Kind = .save
  private(set) var state: State?
  /// The last save failed because Photos access was refused: the line offers Settings.
  private(set) var photosRefused = false
  /// Set once the file is on the phone: the share sheet shows it.
  var shared: SharedFile?

  @ObservationIgnored private var token = UUID()

  func state(for item: MediaItem) -> State? { self.item?.id == item.id ? state : nil }

  func isRunning(for item: MediaItem) -> Bool {
    if case .running = state(for: item) { return true }
    return false
  }

  var verb: String { kind == .save ? "Saving to Photos" : "Getting it ready to share" }

  func save(_ item: MediaItem, saver: PhotoSaver, failureLine: @escaping (MediaFailure) -> String) {
    let token = begin(item, .save)
    Task {
      do {
        for try await progress in saver.save(item) where token == self.token { state = .running(progress) }
        guard token == self.token else { return }
        state = .outcome(.done("Saved to Photos"))
        try? await Task.sleep(for: .seconds(2))
        if token == self.token, case .outcome(.done) = state { state = nil }
      } catch let failure as MediaFailure {
        guard token == self.token else { return }
        photosRefused = failure == .photosNotAllowed
        state = .outcome(.failed(failureLine(failure), canRetry: false))
      } catch {
        if token == self.token { state = .outcome(.failed("Couldn't save this to Photos.", canRetry: true)) }
      }
    }
  }

  func share(_ item: MediaItem, saver: PhotoSaver, failureLine: @escaping (MediaFailure) -> String) {
    let token = begin(item, .share)
    Task {
      do {
        for try await event in saver.shareable(item) where token == self.token {
          switch event {
          case .progress(let progress): state = .running(progress)
          case .finished(let url):
            state = nil
            shared = SharedFile(url: url)
          }
        }
      } catch is CancellationError {
        if token == self.token { state = nil }
      } catch let failure as MediaFailure {
        if token == self.token { state = .outcome(.failed(failureLine(failure), canRetry: true)) }
      } catch {
        if token == self.token { state = .outcome(.failed("Couldn't get this ready to share.", canRetry: true)) }
      }
    }
  }

  func retry(saver: PhotoSaver, failureLine: @escaping (MediaFailure) -> String) {
    guard let item else { return }
    kind == .save ? save(item, saver: saver, failureLine: failureLine) : share(item, saver: saver, failureLine: failureLine)
  }

  private func begin(_ item: MediaItem, _ kind: Kind) -> UUID {
    let token = UUID()
    self.token = token
    self.item = item
    self.kind = kind
    photosRefused = false
    state = .running(nil)
    return token
  }
}

struct SharedFile: Identifiable {
  let id = UUID()
  let url: URL
}

/// Share and Save to Photos, as the clip screen's top bar buttons.
struct OriginalButtons: View {
  let item: MediaItem
  let transfer: OriginalTransfer

  var body: some View {
    ShareOriginalButton(item: item, transfer: transfer)
    SaveOriginalButton(item: item, transfer: transfer)
  }
}

struct ShareOriginalButton: View {
  let item: MediaItem
  let transfer: OriginalTransfer
  @Environment(AppModel.self) private var model

  var body: some View {
    Button {
      transfer.share(item, saver: model.services.saver) { FailureCopy.line(for: $0, sample: model.connection.isSample) }
    } label: {
      Image(systemName: "square.and.arrow.up")
    }
    .buttonStyle(ChromeIconStyle())
    .accessibilityLabel(item.kind == .clip ? "Share the clip" : "Share the photo")
    .disabled(transfer.isRunning(for: item))
  }
}

struct SaveOriginalButton: View {
  let item: MediaItem
  let transfer: OriginalTransfer
  @Environment(AppModel.self) private var model

  var body: some View {
    Button {
      transfer.save(item, saver: model.services.saver) { FailureCopy.line(for: $0, sample: model.connection.isSample) }
    } label: {
      Image(systemName: "square.and.arrow.down")
    }
    .buttonStyle(ChromeIconStyle())
    .accessibilityLabel("Save to Photos")
    .disabled(transfer.isRunning(for: item))
  }
}

/// The transfer's line for the item on screen; nothing when it is about another one.
struct OriginalProgress: View {
  let item: MediaItem
  let transfer: OriginalTransfer
  @Environment(AppModel.self) private var model
  @Environment(\.openURL) private var openURL

  var body: some View {
    switch transfer.state(for: item) {
    case .running(let progress):
      ProgressLine(verb: transfer.verb,
                   fraction: progress.map { $0.totalBytes > 0 ? Double($0.doneBytes) / Double($0.totalBytes) : 0 },
                   detail: progress.map { ProgressWords.amounts($0.doneBytes, of: $0.totalBytes) },
                   key: "original-transfer-\(item.id.raw)")
    case .outcome(let outcome):
      ProgressLine(verb: transfer.verb, fraction: nil, outcome: outcome,
                   onRetry: { transfer.retry(saver: model.services.saver, failureLine: { FailureCopy.line(for: $0, sample: model.connection.isSample) }) },
                   extra: transfer.photosRefused ? (title: "Open Settings", action: {
                     if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                   }) : nil)
    case nil:
      EmptyView()
    }
  }
}

/// The system share sheet for one file.
struct ShareSheet: UIViewControllerRepresentable {
  let url: URL

  func makeUIViewController(context: Context) -> UIActivityViewController {
    UIActivityViewController(activityItems: [url], applicationActivities: nil)
  }

  func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

extension View {
  /// Shows the share sheet once `transfer` has the file.
  func sharesOriginals(_ transfer: OriginalTransfer) -> some View {
    sheet(item: Binding(get: { transfer.shared }, set: { transfer.shared = $0 })) { ShareSheet(url: $0.url) }
  }
}
