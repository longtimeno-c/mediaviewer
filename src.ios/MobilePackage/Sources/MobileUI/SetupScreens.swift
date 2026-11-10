// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Welcome, Connect, First copy, and the scan between them. Setup is one path: the code.
import AVFoundation
import MobileKit
import SwiftUI

struct WelcomeScreen: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    VStack(spacing: 0) {
      SetupPage {
        Text("Your NAS library, searched from your phone")
          .font(.largeTitle.bold())
          .foregroundStyle(Theme.title)
          .padding(.top, 32)
        Text("Open MediaViewer on your Mac or Windows PC, show its code, and scan it here.")
          .font(.body)
          .foregroundStyle(Theme.body)
          .padding(.top, 12)
        VStack(alignment: .leading, spacing: 8) {
          Text("You'll need").font(.footnote.weight(.semibold)).foregroundStyle(Theme.title)
          need("MediaViewer on a Mac or Windows PC, with Local search and Driftwire installed.")
          need("A Synology NAS.")
          need("A public IPv4 address with one forwarded port, or a public IPv6 address with that port open.")
        }
        .padding(.top, 32)
      }
      Button {
        model.isScanning = true
      } label: {
        Label("Scan code", systemImage: "qrcode.viewfinder")
      }
      .buttonStyle(PrimaryButtonStyle())
      .frame(maxWidth: Theme.readableWidth)
      .padding(.horizontal, 24)
      .padding(.vertical, 16)
    }
  }

  private func need(_ text: String) -> some View {
    Text(text).font(.footnote).foregroundStyle(Theme.body).fixedSize(horizontal: false, vertical: true)
  }
}

// MARK: Scan

/// What fills the scan screen: the camera in the app, a stand-in where there is no camera.
@MainActor
public protocol ScanSurfaceProviding {
  func surface(onCode: @escaping (String) -> Void) -> AnyView
}

public struct CameraScanSurface: ScanSurfaceProviding {
  public init() {}
  public func surface(onCode: @escaping (String) -> Void) -> AnyView {
    AnyView(CameraSurface(onCode: onCode))
  }
}

struct ScanScreen: View {
  @Environment(AppModel.self) private var model
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()
      model.scanner.surface { code in
        model.scanned(code)
      }
      .ignoresSafeArea()
      VStack {
        HStack {
          Button { model.isScanning = false } label: { Image(systemName: "xmark") }
            .buttonStyle(ChromeIconStyle())
            .accessibilityLabel("Close")
          Spacer()
        }
        Spacer()
        Text("Point at the code in MediaViewer")
          .font(.subheadline)
          .foregroundStyle(.white)
          .multilineTextAlignment(.center)
          .padding(.horizontal, Theme.inset)
          .padding(.vertical, 8)
          .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(.black.opacity(0.5)))
      }
      .padding(16)
    }
  }
}

private struct CameraSurface: View {
  let onCode: (String) -> Void
  @State private var access = AVCaptureDevice.authorizationStatus(for: .video)
  @Environment(\.openURL) private var openURL

  var body: some View {
    Group {
      if access == .authorized, AVCaptureDevice.default(for: .video) != nil {
        CameraPreview(onCode: onCode)
      } else if access == .notDetermined {
        Color.black.task {
          _ = await AVCaptureDevice.requestAccess(for: .video)
          access = AVCaptureDevice.authorizationStatus(for: .video)
        }
      } else {
        VStack(spacing: 12) {
          Text("Allow the camera to scan the code")
            .font(.headline).foregroundStyle(Theme.title)
          Button("Open Settings") {
            if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
          }
          .buttonStyle(SecondaryButtonStyle())
          .frame(maxWidth: 240)
        }
        .padding(24)
      }
    }
  }
}

private struct CameraPreview: UIViewRepresentable {
  let onCode: (String) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(onCode: onCode) }

  func makeUIView(context: Context) -> PreviewView {
    let view = PreviewView()
    view.previewLayer.session = context.coordinator.session
    view.previewLayer.videoGravity = .resizeAspectFill
    context.coordinator.start(previewLayer: view.previewLayer)
    return view
  }

  func updateUIView(_ view: PreviewView, context: Context) {}

  static func dismantleUIView(_ view: PreviewView, coordinator: Coordinator) { coordinator.stop() }

  final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
  }

  final class Coordinator: NSObject, AVCaptureMetadataOutputObjectsDelegate {
    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "scan.session")
    private let onCode: (String) -> Void
    private var delivered = false

    init(onCode: @escaping (String) -> Void) { self.onCode = onCode }

    /// Keeps the preview upright when the device turns.
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?

    private func followRotation(of device: AVCaptureDevice, previewLayer: AVCaptureVideoPreviewLayer) {
      let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: previewLayer)
      rotation = coordinator
      rotationObservation = coordinator.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.initial, .new]) {
        [weak previewLayer] coordinator, _ in
        let angle = coordinator.videoRotationAngleForHorizonLevelPreview
        DispatchQueue.main.async {
          guard let connection = previewLayer?.connection, connection.isVideoRotationAngleSupported(angle) else { return }
          connection.videoRotationAngle = angle
        }
      }
    }

    func start(previewLayer: AVCaptureVideoPreviewLayer) {
      queue.async { [self] in
        guard let device = AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input)
        else { return }
        session.addInput(input)
        let output = AVCaptureMetadataOutput()
        guard session.canAddOutput(output) else { return }
        session.addOutput(output)
        output.setMetadataObjectsDelegate(self, queue: .main)
        output.metadataObjectTypes = [.qr]
        session.startRunning()
        DispatchQueue.main.async { self.followRotation(of: device, previewLayer: previewLayer) }
      }
    }

    func stop() { queue.async { [session] in session.stopRunning() } }

    func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject],
                        from connection: AVCaptureConnection) {
      guard !delivered, let code = objects.compactMap({ $0 as? AVMetadataMachineReadableCodeObject })
        .first?.stringValue else { return }
      delivered = true
      onCode(code)
    }
  }
}

// MARK: Connect

struct ConnectScreen: View {
  let code: String
  let start: AppModel.ConnectStart
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private enum Phase: Equatable {
    case reading
    case check(LibraryIdentity)
    case notMatched
    case pairing
  }
  private enum StepState: Equatable { case waiting, working, passed, warned, skipped }
  private enum Ending: Equatable {
    case failure(PairingFailure)
    case warning(String)
  }

  @State private var phase: Phase = .reading
  @State private var steps: [PairingStep: StepState] = [:]
  @State private var ending: Ending?
  /// What the person said matches the desktop. Pairing cannot start without it.
  @State private var confirmed: LibraryIdentity?
  /// Each change runs the next step; Try again and They match both land here.
  @State private var attempt = 0

  var body: some View {
    VStack(spacing: 0) {
      switch phase {
      case .check(let identity): checkPage(identity)
      case .notMatched: notMatchedPage
      case .reading, .pairing: stepsPage
      }
      actions
        .frame(maxWidth: Theme.readableWidth)
        .padding(.horizontal, 24)
        .padding(.vertical, 16)
    }
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: ending)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: phase)
    .task(id: attempt) {
      if attempt > 0, let confirmed { await pair(confirmed) } else { await begin() }
    }
  }

  // §7.4: the phone shows the library's name and a short key fingerprint, the desktop shows the same.
  private func checkPage(_ identity: LibraryIdentity) -> some View {
    SetupPage {
      Text("Does this match your computer?")
        .font(.largeTitle.bold())
        .foregroundStyle(Theme.title)
        .padding(.top, 32)
      Text("MediaViewer on your computer shows the same name and code.")
        .font(.body)
        .foregroundStyle(Theme.body)
        .padding(.top, 12)
      VStack(alignment: .leading, spacing: 8) {
        Text(identity.name)
          .font(.title3.weight(.semibold))
          .foregroundStyle(Theme.title)
        Text(identity.fingerprint)
          .font(.title.monospaced())
          .foregroundStyle(Theme.title)
          .accessibilityLabel(identity.fingerprint.map(String.init).joined(separator: " "))
      }
      .padding(Theme.inset)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
      .padding(.top, 24)
    }
  }

  private var notMatchedPage: some View {
    SetupPage {
      Text("Not paired")
        .font(.largeTitle.bold())
        .foregroundStyle(Theme.title)
        .padding(.top, 32)
      Text(FailureCopy.notMatched)
        .font(.body)
        .foregroundStyle(Theme.body)
        .padding(.top, 12)
    }
  }

  private var stepsPage: some View {
    SetupPage {
      Text("Connecting")
        .font(.largeTitle.bold())
        .foregroundStyle(Theme.title)
        .padding(.top, 32)
      VStack(alignment: .leading, spacing: 16) {
        row(.mac, "Pairing with your computer")
        row(.synology, "Reaching your Synology")
        row(.outsideRoute, "Testing from outside, over mobile data")
      }
      .padding(.top, 24)
      // One sweep for however long the steps take; a spinner is for waits under a second.
      if ending == nil, steps.values.contains(.working) {
        ProgressTrack(fraction: nil).padding(.top, 24)
      }
      if let ending {
        endingWords(ending)
          .padding(.top, 24)
          .arrives(reduceMotion: reduceMotion)
      }
    }
  }

  private func row(_ step: PairingStep, _ title: String) -> some View {
    let state = steps[step] ?? .waiting
    return HStack(spacing: 12) {
      ZStack {
        switch state {
        case .waiting: Image(systemName: "circle").foregroundStyle(Theme.body.opacity(0.5))
        case .working: Image(systemName: "circle.inset.filled").foregroundStyle(Theme.accent)
        case .passed: Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.accent)
        case .warned: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(Theme.warm)
        case .skipped: Image(systemName: "minus.circle").foregroundStyle(Theme.body)
        }
      }
      .font(.body)
      .frame(width: 24)
      Text(title)
        .font(.body)
        .foregroundStyle(state == .waiting ? Theme.body : Theme.title)
    }
    .accessibilityElement(children: .combine)
    .accessibilityValue(stateName(state))
  }

  private func stateName(_ state: StepState) -> String {
    switch state {
    case .waiting: return "Not started"
    case .working: return "In progress"
    case .passed: return "Done"
    case .warned: return "Needs attention"
    case .skipped: return "Skipped"
    }
  }

  @ViewBuilder
  private func endingWords(_ ending: Ending) -> some View {
    switch ending {
    case .warning(let text):
      Text(text).font(.subheadline).foregroundStyle(Theme.title)
    case .failure(let failure):
      switch failure {
      case .codeExpired: Text(FailureCopy.codeExpired).font(.subheadline).foregroundStyle(Theme.title)
      case .codeNotReadable: Text(FailureCopy.codeNotReadable).font(.subheadline).foregroundStyle(Theme.title)
      case .serviceTooOld: Text(FailureCopy.serviceTooOld).font(.subheadline).foregroundStyle(Theme.title)
      case .synologyUnreachable(let tried):
        VStack(alignment: .leading, spacing: 8) {
          Text("Can't reach your Synology.").font(.subheadline).foregroundStyle(Theme.title)
          ForEach(tried) { failure in
            AddressFailureRow(failure: failure)
          }
        }
      }
    }
  }

  @ViewBuilder
  private var actions: some View {
    switch (phase, ending) {
    case (.check(let identity), _):
      VStack(spacing: 8) {
        Button("They match") {
          confirmed = identity
          phase = .pairing
          attempt += 1
        }
        .buttonStyle(PrimaryButtonStyle())
        Button("They don't match") { phase = .notMatched }
          .buttonStyle(SecondaryButtonStyle())
      }
    case (.notMatched, _), (_, .failure(.codeExpired)), (_, .failure(.codeNotReadable)):
      VStack(spacing: 8) {
        scanAgain
        backToLibrary
      }
    case (_, .failure):
      VStack(spacing: 8) {
        Button("Try again") { restart() }.buttonStyle(PrimaryButtonStyle())
        Button("Scan a new code") { model.isScanning = true }.buttonStyle(SecondaryButtonStyle())
        backToLibrary
      }
    case (_, .warning):
      Button("Continue") { model.connected() }.buttonStyle(PrimaryButtonStyle())
    default:
      EmptyView()
    }
  }

  private var scanAgain: some View {
    Button { model.isScanning = true } label: { Label("Scan again", systemImage: "qrcode.viewfinder") }
      .buttonStyle(PrimaryButtonStyle())
  }

  /// Only when this scan began from a library that is already paired.
  @ViewBuilder
  private var backToLibrary: some View {
    if model.connection.hasLibrary {
      Button("Back to library") { model.returnToLibrary() }.buttonStyle(QuietButtonStyle())
    }
  }

  private func restart() {
    ending = nil
    steps = [:]
    attempt += 1
  }

  private func begin() async {
    switch start {
    #if DEBUG
    case .matched(let identity):
      confirmed = identity
      phase = .pairing
      await pair(identity)
    case .notMatched:
      phase = .notMatched
    #endif
    case .checkMatch:
      do {
        phase = .check(try await model.services.connection.identify(code: code))
      } catch let failure as PairingFailure {
        phase = .pairing
        ending = .failure(failure)
      } catch {
        phase = .pairing
        ending = .failure(.codeNotReadable)
      }
    }
  }

  private func pair(_ identity: LibraryIdentity) async {
    do {
      var warning: String?
      for try await event in model.services.connection.pair(code: code, confirmed: identity) {
        switch event {
        case .working(let step): steps[step] = .working
        case .passed(let step): steps[step] = .passed
        case .skipped(let step): steps[step] = .skipped
        case .outsideRoute(let result):
          switch result {
          case .reachable:
            steps[.outsideRoute] = .passed
          case .reachableOnlyOnItsOwnNetwork:
            steps[.outsideRoute] = .warned
            warning = FailureCopy.ownNetworkOnly
          case .skippedNoMobileData:
            steps[.outsideRoute] = .skipped
            warning = FailureCopy.noMobileData
          }
        }
      }
      // A cancelled stream ends as if finished, and that is not a pairing.
      guard !Task.isCancelled else { return }
      if let warning { ending = .warning(warning) } else { model.connected() }
    } catch let failure as PairingFailure {
      steps = steps.mapValues { $0 == .working ? .waiting : $0 }
      ending = .failure(failure)
    } catch {
      ending = .failure(.codeNotReadable)
    }
  }
}

/// "nas.example.com: timed out": names the address that failed.
struct AddressFailureRow: View {
  let failure: AddressFailure

  var body: some View {
    (Text(failure.address).foregroundStyle(Theme.title) + Text(": \(failure.reason)").foregroundStyle(Theme.body))
      .font(.footnote)
      .fixedSize(horizontal: false, vertical: true)
  }
}

// MARK: First copy

struct FirstCopyScreen: View {
  @Environment(AppModel.self) private var model

  private enum State: Equatable {
    case starting
    case waiting(totalBytes: Int64)
    case copying(done: Int64, total: Int64)
    case resuming(done: Int64, total: Int64)
    case failed(String)
  }

  @SwiftUI.State private var state: State = .starting
  @SwiftUI.State private var overMobileData = false
  @SwiftUI.State private var attempt = 0

  private let barKey = "first-copy"

  var body: some View {
    // §11.1: this screen is one progress line.
    VStack(alignment: .leading, spacing: 12) {
      line
      if case .waiting(let bytes) = state {
        Button("Copy \(ProgressWords.size(bytes)) over mobile data") {
          overMobileData = true
          attempt += 1
        }
        .buttonStyle(SecondaryButtonStyle())
      }
      if let leaving = leavingWords {
        Text(leaving).font(.footnote).foregroundStyle(Theme.body)
      }
    }
    .frame(maxWidth: Theme.readableWidth)
    .padding(.horizontal, 24)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(Theme.ground)
    .task(id: attempt) { await copy() }
  }

  /// Said only while leaving the app is a choice someone could make.
  private var leavingWords: String? {
    switch state {
    case .copying, .resuming:
      return model.services.sync.continuesAfterLeaving
        ? "You can leave the app. The copy carries on."
        : "Keep MediaViewer open until this finishes."
    default:
      return nil
    }
  }

  @ViewBuilder
  private var line: some View {
    switch state {
    case .starting:
      ProgressLine(verb: "Copying library", fraction: nil, onCancel: cancel)
    case .waiting(let bytes):
      ProgressLine(verb: "Waiting for Wi-Fi", fraction: nil, detail: ProgressWords.size(bytes), onCancel: cancel)
    case .copying(let done, let total):
      ProgressLine(verb: "Copying library", fraction: fraction(done, total),
                   detail: ProgressWords.amounts(done, of: total), key: barKey, onCancel: cancel)
    case .resuming(let done, let total):
      ProgressLine(verb: "Picking up where it left off", fraction: fraction(done, total),
                   detail: ProgressWords.amounts(done, of: total), onCancel: cancel)
    case .failed(let reason):
      ProgressLine(verb: "Copying library", fraction: nil, outcome: .failed(reason, canRetry: true),
                   onCancel: cancel, onRetry: { attempt += 1 })
    }
  }

  private func fraction(_ done: Int64, _ total: Int64) -> Double? {
    total > 0 ? Double(done) / Double(total) : nil
  }

  private func cancel() { model.forgetLibrary() }

  private func copy() async {
    state = .starting
    ProgressMemory.shared.forget(barKey)
    do {
      for try await event in model.services.sync.firstCopy(overMobileData: overMobileData) {
        switch event {
        case .waitingForWiFi(let bytes): state = .waiting(totalBytes: bytes)
        case .copying(let done, let total): state = .copying(done: done, total: total)
        case .resuming(let done, let total): state = .resuming(done: done, total: total)
        }
      }
      guard !Task.isCancelled else { return }
      model.copied()
    } catch is CancellationError {
    } catch let failure as CopyFailure {
      guard !Task.isCancelled else { return }
      state = .failed(FailureCopy.line(for: failure))
    } catch {
      guard !Task.isCancelled else { return }
      state = .failed(FailureCopy.line(for: CopyFailure.synologyUnreachable))
    }
  }
}
