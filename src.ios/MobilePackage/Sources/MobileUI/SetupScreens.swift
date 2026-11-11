// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Welcome, Connect, First copy, and the scan between them. Setup is one path: the code.
import AVFoundation
import MobileKit
import PhotosUI
import SwiftUI
import Vision

struct WelcomeScreen: View {
  @Environment(AppModel.self) private var model

  var body: some View {
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
    } footer: {
      Button {
        model.isScanning = true
      } label: {
        Label("Scan code", systemImage: "qrcode.viewfinder")
      }
      .buttonStyle(PrimaryButtonStyle())
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
  @State private var picked: PhotosPickerItem?
  @State private var problem: String?
  @State private var reading = false
  /// How tall the buttons at the bottom are, so the camera's own messages stop above them.
  @State private var controlsHeight: CGFloat = 0

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()
      model.scanner.surface { code in
        model.scanned(code)
      }
      .environment(\.scanControlsHeight, controlsHeight)
      .ignoresSafeArea()
      VStack(spacing: 12) {
        HStack {
          Button { model.isScanning = false } label: { Image(systemName: "xmark") }
            .buttonStyle(ChromeIconStyle())
            .accessibilityLabel("Close")
          Spacer()
        }
        Spacer()
        otherWays
          .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { controlsHeight = $0 }
      }
      .padding(16)
      .frame(maxWidth: Theme.readableWidth)
    }
    .onChange(of: picked) { _, item in
      guard let item else { return }
      Task { await read(item) }
    }
  }

  /// For when the camera cannot see the code: a screenshot or photo of it, or its link.
  private var otherWays: some View {
    VStack(spacing: 8) {
      if let problem {
        Text(problem)
          .font(.footnote)
          .foregroundStyle(.white)
          .multilineTextAlignment(.center)
          .fixedSize(horizontal: false, vertical: true)
          .padding(.horizontal, 12)
          .padding(.vertical, 8)
          .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(.black.opacity(0.6)))
      }
      ViewThatFits(in: .horizontal) {
        HStack(spacing: 8) { pickButton; pasteButton }
        VStack(spacing: 8) { pickButton; pasteButton }
      }
    }
  }

  private var pickButton: some View {
    PhotosPicker(selection: $picked, matching: .images, photoLibrary: .shared()) {
      Label(reading ? "Reading…" : "Choose a picture", systemImage: "photo")
    }
    .buttonStyle(SecondaryButtonStyle())
    .disabled(reading)
    .accessibilityHint("Reads the code from a screenshot or photo of it")
  }

  /// The same button as "Choose a picture". It reads the clipboard only when tapped; iOS asks the
  /// person once whether MediaViewer may paste.
  private var pasteButton: some View {
    Button {
      let text = UIPasteboard.general.string?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
      if text.hasPrefix("mediaviewer://pair") {
        problem = nil
        model.scanned(text)
      } else {
        problem = FailureCopy.pasteNotCode
      }
    } label: {
      Label("Paste link", systemImage: "doc.on.clipboard")
    }
    .buttonStyle(SecondaryButtonStyle())
    .accessibilityHint("Pastes the link MediaViewer shows next to its code")
  }

  private func read(_ item: PhotosPickerItem) async {
    reading = true
    defer {
      reading = false
      picked = nil
    }
    guard let data = try? await item.loadTransferable(type: Data.self),
          let code = await QRReader.code(in: data) else {
      problem = FailureCopy.noCodeInPicture
      return
    }
    problem = nil
    model.scanned(code)
  }
}

/// The QR code in a still picture, off the main thread. Vision first; Core Image's detector when
/// Vision finds nothing, which also covers the Simulator, where Vision's reader cannot run.
enum QRReader {
  static func code(in data: Data) async -> String? {
    await Task.detached(priority: .userInitiated) {
      guard let image = CIImage(data: data, options: [.applyOrientationProperty: true]) else { return nil }
      let request = VNDetectBarcodesRequest()
      request.symbologies = [.qr]
      try? VNImageRequestHandler(ciImage: image, options: [:]).perform([request])
      if let found = request.results?.compactMap(\.payloadStringValue).first { return found }
      let detector = CIDetector(ofType: CIDetectorTypeQRCode, context: nil, options: [CIDetectorAccuracy: CIDetectorAccuracyHigh])
      return detector?.features(in: image).compactMap { ($0 as? CIQRCodeFeature)?.messageString }.first
    }.value
  }
}

extension EnvironmentValues {
  /// The scan screen's buttons, measured; what the camera surface keeps clear at the bottom.
  @Entry var scanControlsHeight: CGFloat = 0
}

private struct CameraSurface: View {
  let onCode: (String) -> Void
  @Environment(\.scanControlsHeight) private var controlsHeight
  @State private var access = AVCaptureDevice.authorizationStatus(for: .video)
  @Environment(\.openURL) private var openURL
  private let hasCamera = AVCaptureDevice.default(for: .video) != nil

  var body: some View {
    Group {
      if !hasCamera {
        message(FailureCopy.noCamera, settings: false)
      } else if access == .authorized {
        CameraPreview(onCode: onCode)
          // At the top, under Close: the buttons at the bottom grow with the text size.
          .overlay(alignment: .top) {
            Text("Point at the code in MediaViewer")
              .font(.subheadline)
              .foregroundStyle(.white)
              .multilineTextAlignment(.center)
              .padding(.horizontal, Theme.inset)
              .padding(.vertical, 8)
              .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(.black.opacity(0.5)))
              .padding(.horizontal, 16)
              .safeAreaPadding(.top, 76)
          }
      } else if access == .notDetermined {
        Color.black.task {
          _ = await AVCaptureDevice.requestAccess(for: .video)
          access = AVCaptureDevice.authorizationStatus(for: .video)
        }
      } else {
        message(FailureCopy.cameraNotAllowed, settings: true)
      }
    }
  }

  /// Between Close and the buttons, scrolling when the text is too large for that space.
  private func message(_ text: String, settings: Bool) -> some View {
    GeometryReader { geo in
      let top = geo.safeAreaInsets.top + 76
      let bottom = geo.safeAreaInsets.bottom + controlsHeight + 32
      ScrollView {
        messageContent(text, settings: settings)
          .frame(maxWidth: .infinity, minHeight: max(0, geo.size.height - top - bottom))
          .padding(.top, top)
          .padding(.bottom, bottom)
      }
      .scrollBounceBehavior(.basedOnSize)
    }
    .ignoresSafeArea()
  }

  private func messageContent(_ text: String, settings: Bool) -> some View {
    VStack(spacing: 12) {
      Image(systemName: settings ? "camera" : "camera.badge.ellipsis")
        .font(.system(size: 28))
        .foregroundStyle(.white.opacity(0.8))
        .accessibilityHidden(true)
      Text(text)
        .font(.headline)
        .foregroundStyle(.white)
        .multilineTextAlignment(.center)
      if settings {
        Button("Open Settings") {
          if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
        }
        .buttonStyle(SecondaryButtonStyle())
        .frame(maxWidth: 240)
      }
    }
    .padding(24)
    .frame(maxWidth: Theme.readableWidth)
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
  private enum StepState: Equatable { case waiting, working, passed, warned, failed, skipped }
  private enum Ending: Equatable {
    case failure(PairingFailure)
    case warning(String)
  }

  /// The step icons' column grows with the text, so a large icon never sits on the words.
  @ScaledMetric(relativeTo: .body) private var iconWidth: CGFloat = 24
  @State private var phase: Phase = .reading
  @State private var steps: [PairingStep: StepState] = [:]
  @State private var ending: Ending?
  /// What the person said matches the desktop. Pairing cannot start without it.
  @State private var confirmed: LibraryIdentity?
  /// Each change runs the next step; Try again and They match both land here.
  @State private var attempt = 0

  var body: some View {
    SetupPage {
      switch phase {
      case .check(let identity): checkPage(identity)
      case .notMatched: notMatchedPage
      case .reading, .pairing: stepsPage
      }
    } footer: {
      actions
    }
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: ending)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: phase)
    .task(id: attempt) {
      if attempt > 0, let confirmed { await pair(confirmed) } else { await begin() }
    }
  }

  // §7.4: the phone shows the library's name and a short key fingerprint, the desktop shows the same.
  private func checkPage(_ identity: LibraryIdentity) -> some View {
    Group {
      Text("Does this match your computer?")
        .font(.largeTitle.bold())
        .foregroundStyle(Theme.title)
        .padding(.top, 32)
      Text("MediaViewer on your computer shows the same name and check code. Pair only if they match.")
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
    Group {
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
    Group {
      Text("Connecting")
        .font(.largeTitle.bold())
        .foregroundStyle(Theme.title)
        .padding(.top, 32)
      VStack(alignment: .leading, spacing: 16) {
        // The computer's half waits for nearby sync (M4b); a step that did not run is not shown.
        if steps[.mac] != .skipped { row(.mac, "Pairing with your computer") }
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
    return HStack(alignment: .firstTextBaseline, spacing: 12) {
      ZStack {
        switch state {
        case .waiting: Image(systemName: "circle").foregroundStyle(Theme.body.opacity(0.5))
        case .working: Image(systemName: "circle.inset.filled").foregroundStyle(Theme.accent)
        case .passed: Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.accent)
        case .warned: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(Theme.warm)
        case .failed: Image(systemName: "xmark.circle.fill").foregroundStyle(Theme.danger)
        case .skipped: Image(systemName: "minus.circle").foregroundStyle(Theme.body)
        }
      }
      .font(.body)
      .frame(width: iconWidth)
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
    case .failed: return "Failed"
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
      // The step that was running is the one that failed, and says so.
      steps = steps.mapValues { $0 == .working ? .failed : $0 }
      ending = .failure(failure)
    } catch {
      steps = steps.mapValues { $0 == .working ? .failed : $0 }
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
