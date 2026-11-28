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
  static let noPasscode = "Set a passcode for this iPhone in the Settings app to lock MediaViewer."
  static let turnOnReason = "Turn on the lock for MediaViewer."
  static let unlockReason = "Unlock MediaViewer."
  static let locked = "MediaViewer is locked"
  static func unlock(_ kind: OwnerCheckKind?) -> String {
    guard let kind, kind != .passcode else { return "Unlock" }
    return "Unlock with \(kind.name)"
  }
}

@MainActor
@Observable
public final class AppLock {
  /// The person turned the lock on.
  public private(set) var isOn: Bool
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

  /// `hasLibrary` false turns a stored lock off: there is nothing to lock.
  public init(owner: OwnerCheck = DeviceOwnerCheck(), defaults: UserDefaults = .standard, hasLibrary: Bool) {
    self.owner = owner
    self.defaults = defaults
    if !hasLibrary { defaults.removeObject(forKey: Self.key) }
    isOn = hasLibrary && defaults.bool(forKey: Self.key)
    isLocked = isOn
  }

  /// What the check is on this phone, nil when it has no passcode.
  public func kind() -> OwnerCheckKind? { owner.kind() }

  /// Turns the lock on once the owner passes the check, so it is known to work before it is needed.
  @discardableResult
  public func turnOn() async -> Bool {
    guard !isOn, !isChecking, owner.kind() != nil else { return isOn }
    isChecking = true
    let passed = await owner.check(reason: LockCopy.turnOnReason)
    isChecking = false
    guard passed else { return false }
    isOn = true
    defaults.set(true, forKey: Self.key)
    return true
  }

  /// Off, and open. Settings is reached only unlocked; Forget calls it with the library.
  public func turnOff() {
    isOn = false
    isLocked = false
    isHidden = false
    defaults.removeObject(forKey: Self.key)
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
  @Environment(\.scenePhase) private var phase

  var body: some View {
    LockWindowHost(lock: lock, shown: lock.covers)
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
  let shown: Bool

  final class Coordinator {
    var window: UIWindow?
  }

  func makeCoordinator() -> Coordinator { Coordinator() }
  func makeUIView(context: Context) -> UIView { UIView() }

  func updateUIView(_ view: UIView, context: Context) {
    let coordinator = context.coordinator
    let lock = lock
    let shown = shown
    // The view has its window (and scene) only once it is in the hierarchy.
    DispatchQueue.main.async {
      guard let main = view.window, let scene = main.windowScene else { return }
      if shown {
        let window = coordinator.window ?? {
          let window = UIWindow(windowScene: scene)
          window.windowLevel = .alert
          let host = UIHostingController(rootView: LockScreen(lock: lock))
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
  @AppStorage(Preferences.appearance) private var appearance = "dark"
  @State private var kind: OwnerCheckKind?

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
          Button(LockCopy.unlock(kind)) { Task { await lock.unlock() } }
            .buttonStyle(PrimaryButtonStyle())
            .disabled(lock.isChecking)
            .keyboardShortcut(.defaultAction)
            .accessibilityIdentifier("lock.unlock")
            .padding(.top, 8)
        }
      }
      .frame(maxWidth: Theme.readableWidth)
      .padding(Theme.inset * 2)
    }
    .preferredColorScheme(chosenScheme(appearance))
    .tint(Theme.accent)
    .onAppear { kind = lock.kind() }
  }
}
