// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → VPN through your server (docs/plans/phone-vpn.md). Built in but opt-in: nothing is set
// up until the person pastes their server's VPN code and switches it on, and Remove VPN takes it all
// away. The first switch-on asks first (App Review 5.4), always before iOS's own alert. A build with
// no tunnel says so plainly; the sample library's demo says, for as long as it is shown, that
// nothing is routed.
import MobileKit
import SwiftUI

/// What the screen shows and what a tap on the switch does, apart from SwiftUI so tests can hold it.
enum VPNFlow {
  enum Page: Equatable {
    /// This build has no tunnel: the words, what's coming, nothing to switch.
    case notInThisBuild
    /// No code yet: how to get one, and the button to paste it.
    case notSetUp
    /// A code is in: the host, the switch, the status, another code, Remove.
    case setUp
  }

  enum SwitchStep: Equatable {
    /// The consent comes first; nothing is switched until it is accepted.
    case askConsent
    case turnOn
    case turnOff
    /// The switch is already where it was moved to, or there is nothing to switch.
    case nothing
  }

  static func page(_ s: VPNSnapshot) -> Page {
    switch s.status {
    case .notInThisBuild: return .notInThisBuild
    case .notSetUp: return .notSetUp
    default: return .setUp
    }
  }

  /// The switch was moved to `on`.
  static func step(switchingTo on: Bool, _ s: VPNSnapshot) -> SwitchStep {
    guard s.isSetUp else { return .nothing }
    if !on { return s.isOn ? .turnOff : .nothing }
    if s.isOn { return .nothing }
    return s.consented ? .turnOn : .askConsent
  }

  static func toggleTitle(_ s: VPNSnapshot) -> String { s.isDemo ? VPNCopy.toggleDemo : VPNCopy.toggle }

  static func consentMessage(_ s: VPNSnapshot) -> String {
    s.isDemo ? VPNCopy.consentDemo : VPNCopy.consent(host: s.host ?? "")
  }
}

struct VPNScreen: View {
  @Environment(AppModel.self) private var model
  @State private var enteringCode = false
  @State private var askingConsent = false
  @State private var confirmingRemove = false

  private var vpn: VPNSnapshot { model.vpn }
  private var service: VPNService { model.vpnService }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 12) {
        if vpn.isDemo {
          NoticeCard(message: VPNCopy.demoBanner)
            .accessibilityIdentifier(VPNIDs.demoBanner)
        }
        switch VPNFlow.page(vpn) {
        case .notInThisBuild: notInThisBuild
        case .notSetUp: notSetUp
        case .setUp: setUp
        }
      }
      .frame(maxWidth: Theme.readableWidth)
      .padding(Theme.inset)
      .frame(maxWidth: .infinity)
    }
    .background(Theme.ground)
    .navigationTitle(VPNCopy.section)
    .navigationBarTitleDisplayMode(.inline)
    .sheet(isPresented: $enteringCode) {
      VPNCodeSheet(service: service)
        .presentationDetents([.medium, .large])
    }
    .sheet(isPresented: $askingConsent) {
      VPNConsentSheet(message: VPNFlow.consentMessage(vpn)) {
        askingConsent = false
        let service = service
        Task {
          await service.acceptConsent()
          await service.setOn(true)
        }
      } notNow: {
        askingConsent = false
      }
      .presentationDetents([.medium, .large])
    }
  }

  // MARK: The pages

  private var notInThisBuild: some View {
    InfoCard {
      VStack(alignment: .leading, spacing: 8) {
        Text(VPNCopy.notInThisBuild)
          .font(.subheadline)
          .foregroundStyle(Theme.title)
          .fixedSize(horizontal: false, vertical: true)
        Text(VPNCopy.howTo)
          .font(.footnote)
          .foregroundStyle(Theme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .accessibilityElement(children: .combine)
      .accessibilityIdentifier(VPNIDs.notInThisBuild)
    }
  }

  private var notSetUp: some View {
    InfoCard {
      Text(VPNCopy.howTo)
        .font(.subheadline)
        .foregroundStyle(Theme.title)
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, alignment: .leading)
      Button(VPNCopy.connectHost) { enteringCode = true }
        .buttonStyle(PrimaryButtonStyle())
        .accessibilityIdentifier(VPNIDs.connectHost)
    }
  }

  @ViewBuilder
  private var setUp: some View {
    InfoCard {
      InfoRow(label: VPNCopy.host, value: vpn.host ?? "")
      vpnToggle
      InfoRow(label: VPNCopy.status, value: VPNCopy.status(vpn.status))
        .accessibilityIdentifier(VPNIDs.status)
    }
    Button(VPNCopy.connectAnother) { enteringCode = true }
      .buttonStyle(SecondaryButtonStyle())
      .accessibilityIdentifier(VPNIDs.connectHost)
    Button(VPNCopy.remove, role: .destructive) { confirmingRemove = true }
      .buttonStyle(QuietButtonStyle(color: Theme.danger))
      .frame(maxWidth: .infinity)
      .accessibilityIdentifier(VPNIDs.remove)
      .confirmationDialog(VPNCopy.removeTitle, isPresented: $confirmingRemove, titleVisibility: .visible) {
        Button(VPNCopy.remove, role: .destructive) {
          let service = service
          Task { await service.remove() }
        }
        .accessibilityIdentifier(VPNIDs.removeConfirm)
      } message: {
        Text(VPNCopy.removeMessage)
      }
  }

  /// The same switch and VoiceOver treatment as Settings' "Faster direct connection on mobile data".
  private var vpnToggle: some View {
    let title = VPNFlow.toggleTitle(vpn)
    let detail = VPNCopy.detail(host: vpn.host ?? "")
    return Toggle(isOn: Binding(get: { vpn.isOn }, set: { switchMoved(to: $0) })) {
      VStack(alignment: .leading, spacing: 2) {
        Text(title).font(.subheadline).foregroundStyle(Theme.title)
        Text(detail)
          .font(.footnote)
          .foregroundStyle(Theme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .tint(Theme.accent)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(title)
    .accessibilityValue(vpn.isOn ? "On" : "Off")
    .accessibilityHint(detail)
    .accessibilityAddTraits(.isToggle)
    .accessibilityAction { switchMoved(to: !vpn.isOn) }
    .accessibilityIdentifier(VPNIDs.toggle)
  }

  private func switchMoved(to on: Bool) {
    let service = service
    switch VPNFlow.step(switchingTo: on, vpn) {
    case .askConsent: askingConsent = true
    case .turnOn: Task { await service.setOn(true) }
    case .turnOff: Task { await service.setOn(false) }
    case .nothing: break
    }
  }
}

/// Paste the VPN code from the server. A wrong one says so under the field and stays open.
private struct VPNCodeSheet: View {
  let service: VPNService
  @Environment(\.dismiss) private var dismiss
  @State private var code = ""
  @State private var notACode = false
  @State private var working = false

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          Text(VPNCopy.howTo)
            .font(.footnote)
            .foregroundStyle(Theme.body)
            .fixedSize(horizontal: false, vertical: true)
          SecureField(VPNCopy.codeField, text: $code)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .textContentType(.none)
            .submitLabel(.go)
            .onSubmit(connect)
            .font(.body)
            .foregroundStyle(Theme.title)
            .padding(.horizontal, Theme.inset)
            .frame(minHeight: 44)
            .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
            .accessibilityIdentifier(VPNIDs.codeField)
          if notACode {
            Text(VPNCopy.notACode)
              .font(.footnote)
              .foregroundStyle(Theme.danger)
              .fixedSize(horizontal: false, vertical: true)
              .accessibilityIdentifier(VPNIDs.codeError)
          }
          Button(VPNCopy.connect, action: connect)
            .buttonStyle(PrimaryButtonStyle())
            .disabled(working)
            .accessibilityIdentifier(VPNIDs.connect)
        }
        .frame(maxWidth: Theme.readableWidth)
        .padding(Theme.inset)
        .frame(maxWidth: .infinity)
      }
      .background(Theme.ground)
      .navigationTitle(VPNCopy.connectHost)
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
      }
    }
  }

  private func connect() {
    guard !working else { return }
    working = true
    let text = code.trimmingCharacters(in: .whitespacesAndNewlines)
    Task {
      do {
        try await service.connect(code: text)
        dismiss()
      } catch {
        notACode = true
      }
      working = false
    }
  }
}

/// App Review 5.4: what the VPN does with the person's traffic, before the first switch-on and so
/// before iOS's own alert. Not Now, or a swipe down, leaves it off.
private struct VPNConsentSheet: View {
  let message: String
  let turnOn: () -> Void
  let notNow: () -> Void

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        Text(VPNCopy.consentTitle)
          .font(.title3.weight(.semibold))
          .foregroundStyle(Theme.title)
          .fixedSize(horizontal: false, vertical: true)
          .accessibilityAddTraits(.isHeader)
        Text(message)
          .font(.subheadline)
          .foregroundStyle(Theme.body)
          .fixedSize(horizontal: false, vertical: true)
        VStack(spacing: 8) {
          Button(VPNCopy.turnOn, action: turnOn)
            .buttonStyle(PrimaryButtonStyle())
            .accessibilityIdentifier(VPNIDs.consentTurnOn)
          Button(VPNCopy.notNow, action: notNow)
            .buttonStyle(SecondaryButtonStyle())
            .accessibilityIdentifier(VPNIDs.consentNotNow)
        }
        .padding(.top, 4)
      }
      .frame(maxWidth: Theme.readableWidth, alignment: .leading)
      .padding(24)
      .frame(maxWidth: .infinity)
    }
    .scrollBounceBehavior(.basedOnSize)
    .background(Theme.ground)
  }
}
