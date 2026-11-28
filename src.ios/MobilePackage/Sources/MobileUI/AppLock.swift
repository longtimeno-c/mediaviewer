// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §12 "Lost phone": the optional lock. With it on, the app locks each time
// it leaves the screen and opens again only for the phone's owner: Face ID, Touch ID or Optic ID,
// else the phone's own passcode (iOS offers it when biometrics fail). It covers the app while it is
// not in front, so the app switcher shows no photos. It is offered only once a library is set up,
// and Forget turns it off with the library.
import LocalAuthentication
import SwiftUI

/// How this phone proves it is its owner.
public enum OwnerCheckKind: Equatable, Sendable {
  case faceID, touchID, opticID, passcode

  var name: String {
    switch self {
    case .faceID: return "Face ID"
    case .touchID: return "Touch ID"
    case .opticID: return "Optic ID"
    case .passcode: return "Passcode"
    }
  }
}

/// Asks iOS whether the person holding the phone is its owner.
public protocol OwnerCheck: Sendable {
  /// What the check is on this phone; nil when the phone has no passcode, so nothing can lock it.
  func kind() -> OwnerCheckKind?
  /// Shows the system's check. False when it failed or the person cancelled.
  func check(reason: String) async -> Bool
}

/// The real check: biometrics, falling back to the phone's passcode.
public struct DeviceOwnerCheck: OwnerCheck {
  public init() {}

  public func kind() -> OwnerCheckKind? {
    let context = LAContext()
    guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: nil) else { return nil }
    // biometryType is set only by a successful biometric check of the policy; none enrolled is the passcode.
    guard context.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: nil) else { return .passcode }
    switch context.biometryType {
    case .faceID: return .faceID
    case .touchID: return .touchID
    case .opticID: return .opticID
    default: return .passcode
    }
  }

  public func check(reason: String) async -> Bool {
    (try? await LAContext().evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)) ?? false
  }
}

/// The lock's words, shared with the tests.
enum LockCopy {
  static let section = "Lock"
  static func toggle(_ kind: OwnerCheckKind) -> String { kind == .passcode ? "Require passcode" : "Require \(kind.name)" }
  static let detail = "MediaViewer locks each time you leave it, and hides your photos in the app switcher."
  static let required = "The owner of this library requires it. MediaViewer locks each time you leave it."
  static let noPasscode = "Set a passcode for this iPhone in the Settings app to lock MediaViewer."
  static let requiredNoPasscode =
    "The owner of this library requires a passcode on this iPhone. Set one in the Settings app, then try again."
  static let turnOnReason = "Turn on the lock for MediaViewer."
  static let pairReason = "This library's owner requires the lock."
  static func pairing(_ kind: OwnerCheckKind?) -> String {
    guard let kind else { return requiredNoPasscode }
    let check = kind == .passcode ? "your passcode" : kind.name
    return "The owner of this library requires \(check) on this iPhone. You'll be asked for it now, and each time you open MediaViewer."
  }
  static let unlockReason = "Unlock MediaViewer."
  static let locked = "MediaViewer is locked"
  static let tryAgain = "Try again"
  static let forget = "Forget this library"
  static func unlock(_ kind: OwnerCheckKind?) -> String {
    guard let kind, kind != .passcode else { return "Unlock" }
    return "Unlock with \(kind.name)"
  }
}

@MainActor
@Observable
public final class AppLock {
  /// The person turned the lock on (Settings), for a library that does not require it.
  public private(set) var isChosen: Bool
  /// The library's owner requires it (the service's caps, kept with the pairing). Not the
  /// person's to turn off.
  public private(set) var isRequired: Bool
  /// The app locks when it leaves the screen.
  public var isOn: Bool { isChosen || isRequired }
  /// The app needs the owner before it shows anything.
  public private(set) var isLocked: Bool
  /// Not in front: covered so the app switcher's picture shows nothing, though not locked yet.
  public private(set) var isHidden = false
  /// The system's check is on screen.
  public private(set) var isChecking = false
  /// Covered, by the lock or because the app is not in front.
  public var covers: Bool { isLocked || isHidden }

  /// The check has been offered since the app locked. It is offered once by itself, so a person
  /// who cancels is not asked again and again; the lock screen's button asks after that.
  @ObservationIgnored private var offered = false
  private let owner: OwnerCheck
  private let defaults: UserDefaults
  static let key = "appLock"

  /// `hasLibrary` false turns a stored lock off: there is nothing to lock. `required`: the owner's
  /// word as the pairing kept it, so a phone kept offline still locks at launch.
  public init(owner: OwnerCheck = DeviceOwnerCheck(), defaults: UserDefaults = .standard, hasLibrary: Bool,
              required: Bool = false) {
    self.owner = owner
    self.defaults = defaults
    if !hasLibrary { defaults.removeObject(forKey: Self.key) }
    isChosen = hasLibrary && defaults.bool(forKey: Self.key)
    isRequired = hasLibrary && required
    isLocked = isChosen || isRequired
  }

  /// What the check is on this phone, nil when it has no passcode.
  public func kind() -> OwnerCheckKind? { owner.kind() }

  /// The owner's word, as each connect hears it. Newly required, the app locks the next time it
  /// leaves the screen, not under the person using it; no longer required, it opens unless the
  /// person chose the lock themselves.
  public func setRequired(_ required: Bool) {
    guard required != isRequired else { return }
    isRequired = required
    if !isOn {
      isLocked = false
      isHidden = false
    }
  }

  /// Turns the lock on once the owner passes the check, so it is known to work before it is needed.
  @discardableResult
  public func turnOn() async -> Bool {
    guard !isChosen, !isChecking, owner.kind() != nil else { return isChosen }
    isChecking = true
    let passed = await owner.check(reason: LockCopy.turnOnReason)
    isChecking = false
    guard passed else { return false }
    isChosen = true
    defaults.set(true, forKey: Self.key)
    return true
  }

  /// Pairing with a library whose owner requires the lock: the check first, so the person knows
  /// what opening it will take. Nothing changes here; the pairing brings the owner's word.
  public func checkBeforePairing() async -> Bool {
    guard !isChecking, owner.kind() != nil else { return false }
    isChecking = true
    let passed = await owner.check(reason: LockCopy.pairReason)
    isChecking = false
    return passed
  }

  /// The person's own lock off. Settings is reached only unlocked; one the owner requires stays.
  public func turnOff() {
    isChosen = false
    defaults.removeObject(forKey: Self.key)
    if !isOn {
      isLocked = false
      isHidden = false
    }
  }

  /// The library is gone (Forget): nothing left to lock, the owner's word with it.
  public func forget() {
    isRequired = false
    turnOff()
  }

  /// Asks the owner, then opens.
  public func unlock() async {
    guard isLocked, !isChecking else { return }
    offered = true
    isChecking = true
    let passed = await owner.check(reason: LockCopy.unlockReason)
    isChecking = false
    if passed { isLocked = false }
  }

  /// Follows the scene. Returns the check it offered by itself on coming to the front, if any.
  @discardableResult
  public func sceneChanged(to phase: ScenePhase) -> Task<Void, Never>? {
    guard isOn else { return nil }
    switch phase {
    case .background:
      isLocked = true
      isHidden = true
      offered = false
    case .inactive:
      // The system's own check makes the app inactive; covering then would flash over Settings.
      if !isChecking { isHidden = true }
    case .active:
      isHidden = false
      if isLocked, !offered, !isChecking {
        offered = true
        return Task { await unlock() }
      }
    @unknown default:
      break
    }
    return nil
  }
}

/// Puts the lock screen over the whole app, sheets and full-screen covers included, in a window
/// of its own above the app's. Reads the lock itself, so only this view redraws with it.
struct LockCover: View {
  let lock: AppLock
  /// Forget, from the lock screen of a library whose owner requires a passcode this phone lacks.
  let forget: @MainActor () -> Void
  @Environment(\.scenePhase) private var phase

  var body: some View {
    LockWindowHost(lock: lock, forget: forget, shown: lock.covers)
      .frame(width: 0, height: 0)
      .onChange(of: phase, initial: true) { _, new in _ = lock.sceneChanged(to: new) }
  }
}

/// The app's own window cannot show anything before the lock window is up, at launch: it is covered
/// in place too.
struct LockGround: View {
  let lock: AppLock

  var body: some View {
    if lock.covers { Theme.ground.ignoresSafeArea().accessibilityHidden(true) }
  }
}

private struct LockWindowHost: UIViewRepresentable {
  let lock: AppLock
  let forget: @MainActor () -> Void
  let shown: Bool

  final class Coordinator {
    var window: UIWindow?
  }

  func makeCoordinator() -> Coordinator { Coordinator() }
  func makeUIView(context: Context) -> UIView { UIView() }

  func updateUIView(_ view: UIView, context: Context) {
    let coordinator = context.coordinator
    let lock = lock
    let forget = forget
    let shown = shown
    // The view has its window (and scene) only once it is in the hierarchy.
    DispatchQueue.main.async {
      guard let main = view.window, let scene = main.windowScene else { return }
      if shown {
        let window = coordinator.window ?? {
          let window = UIWindow(windowScene: scene)
          window.windowLevel = .alert
          let host = UIHostingController(rootView: LockScreen(lock: lock, forget: forget))
          host.view.backgroundColor = .clear
          host.view.accessibilityViewIsModal = true
          window.rootViewController = host
          coordinator.window = window
          return window
        }()
        guard window.isHidden else { return }
        // A keyboard would stay up over the lock.
        main.endEditing(true)
        window.makeKeyAndVisible()
        UIAccessibility.post(notification: .screenChanged, argument: window.rootViewController?.view)
      } else if let window = coordinator.window, !window.isHidden {
        window.isHidden = true
        main.makeKey()
        UIAccessibility.post(notification: .screenChanged, argument: nil)
      }
    }
  }
}

struct LockScreen: View {
  let lock: AppLock
  let forget: @MainActor () -> Void
  @AppStorage(Preferences.appearance) private var appearance = "dark"
  @State private var kind: OwnerCheckKind?
  @State private var confirmingForget = false

  /// The owner requires a lock this phone cannot give: it has no passcode.
  private var needsPasscode: Bool { lock.isRequired && kind == nil }

  var body: some View {
    ZStack {
      Theme.ground.ignoresSafeArea()
      VStack(spacing: 16) {
        Image(systemName: "lock.fill")
          .font(.system(size: 44, weight: .semibold))
          .foregroundStyle(Theme.accent)
          .accessibilityHidden(true)
        // Only the cover, while the app is not in front: nothing to read or press.
        if lock.isLocked {
          Text(LockCopy.locked)
            .font(.title3.weight(.semibold))
            .foregroundStyle(Theme.title)
            .multilineTextAlignment(.center)
            .accessibilityAddTraits(.isHeader)
          if needsPasscode {
            Text(LockCopy.requiredNoPasscode)
              .font(.footnote)
              .foregroundStyle(Theme.body)
              .multilineTextAlignment(.center)
              .fixedSize(horizontal: false, vertical: true)
          }
          Button(needsPasscode ? LockCopy.tryAgain : LockCopy.unlock(kind)) {
            kind = lock.kind()
            Task { await lock.unlock() }
          }
            .buttonStyle(PrimaryButtonStyle())
            .disabled(lock.isChecking)
            .keyboardShortcut(.defaultAction)
            .accessibilityIdentifier("lock.unlock")
            .padding(.top, 8)
          if needsPasscode {
            Button(LockCopy.forget, role: .destructive) { confirmingForget = true }
              .buttonStyle(QuietButtonStyle(color: Theme.danger))
              .confirmationDialog("Forget this library?", isPresented: $confirmingForget, titleVisibility: .visible) {
                Button("Forget", role: .destructive) { forget() }
              } message: {
                Text("Everything MediaViewer keeps on this phone is removed. Nothing on your server changes. You'll need a new code from MediaViewer to connect again.")
              }
          }
        }
      }
      .frame(maxWidth: Theme.readableWidth)
      .padding(Theme.inset * 2)
    }
    .preferredColorScheme(chosenScheme(appearance))
    .tint(Theme.accent)
    .onAppear { kind = lock.kind() }
    // A passcode set in the Settings app meanwhile.
    .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in kind = lock.kind() }
  }
}
