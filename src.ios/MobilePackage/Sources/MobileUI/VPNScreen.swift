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

  /// Under the switch. The demo never claims to route anything.
  static func detail(_ s: VPNSnapshot) -> String {
    s.isDemo ? VPNCopy.detailDemo : VPNCopy.detail(host: s.host ?? "")
  }

  static func consentTitle(_ s: VPNSnapshot) -> String { s.isDemo ? VPNCopy.consentTitleDemo : VPNCopy.consentTitle }

  static func consentMessage(_ s: VPNSnapshot) -> String {
    s.isDemo ? VPNCopy.consentDemo : VPNCopy.consent(host: s.host ?? "")
  }

  /// The line under the code field: the demo says a code is only checked.
  static func codeNote(_ s: VPNSnapshot) -> String? { s.isDemo ? VPNCopy.codeDemo : nil }

  /// Under Connect automatically. The demo never claims iOS will turn anything on.
  static func autoConnectDetail(_ s: VPNSnapshot) -> String {
    s.isDemo ? VPNCopy.autoConnectDetail + " " + VPNCopy.autoConnectDemo : VPNCopy.autoConnectDetail
  }

  /// The home networks with `name` added (cleaned: a repeat, a blank or a too-long name changes nothing).
  static func adding(_ name: String, to home: [String]) -> [String] { VPNHomeNetworks.cleaned(home + [name]) }
  static func removing(_ name: String, from home: [String]) -> [String] { home.filter { $0 != name } }
}

struct VPNScreen: View {
  @Environment(AppModel.self) private var model
  @State private var enteringCode = false
  @State private var askingConsent = false
  @State private var confirmingRemove = false
  @State private var addingHome = false
  @State private var newHome = ""
  /// A switch-on is still being carried out: another is ignored (no double Turn On). Off always goes.
  @State private var switching = false

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
      // Large only: a sheet that grows for the keyboard moves Connect under the finger.
      VPNCodeSheet(service: service, note: VPNFlow.codeNote(vpn))
        .presentationDetents([.large])
    }
    .sheet(isPresented: $askingConsent) {
      VPNConsentSheet(title: VPNFlow.consentTitle(vpn), message: VPNFlow.consentMessage(vpn)) {
        askingConsent = false
        switchOn {
          await $0.acceptConsent()
          await $0.setOn(true)
        }
      } notNow: {
        askingConsent = false
      }
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
    InfoCard {
      autoConnectToggle
      if vpn.autoConnect { homeNetworkList }
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
    let detail = VPNFlow.detail(vpn)
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

  /// Opt-in, off by default (owner call 4).
  private var autoConnectToggle: some View {
    let detail = VPNFlow.autoConnectDetail(vpn)
    return Toggle(isOn: Binding(get: { vpn.autoConnect }, set: { setAutoConnect($0, vpn.homeNetworks) })) {
      VStack(alignment: .leading, spacing: 2) {
        Text(VPNCopy.autoConnect).font(.subheadline).foregroundStyle(Theme.title)
          .fixedSize(horizontal: false, vertical: true)
        Text(detail)
          .font(.footnote)
          .foregroundStyle(Theme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .tint(Theme.accent)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(VPNCopy.autoConnect)
    .accessibilityValue(vpn.autoConnect ? "On" : "Off")
    .accessibilityHint(detail)
    .accessibilityAddTraits(.isToggle)
    .accessibilityAction { setAutoConnect(!vpn.autoConnect, vpn.homeNetworks) }
    .accessibilityIdentifier(VPNIDs.autoConnect)
  }

  @ViewBuilder
  private var homeNetworkList: some View {
    Text(VPNCopy.homeNetworks)
      .font(.footnote.weight(.semibold))
      .foregroundStyle(Theme.body)
      .frame(maxWidth: .infinity, alignment: .leading)
      .accessibilityAddTraits(.isHeader)
    if vpn.homeNetworks.isEmpty {
      Text(VPNCopy.noHomeNetworks)
        .font(.footnote)
        .foregroundStyle(Theme.body)
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    ForEach(vpn.homeNetworks, id: \.self) { name in
      HStack(spacing: 8) {
        Image(systemName: "wifi").font(.footnote).foregroundStyle(Theme.body).accessibilityHidden(true)
        Text(name).font(.subheadline).foregroundStyle(Theme.title)
          .lineLimit(2)
          .frame(maxWidth: .infinity, alignment: .leading)
          .accessibilityIdentifier(VPNIDs.homeNetwork(name))
        Button { setAutoConnect(true, VPNFlow.removing(name, from: vpn.homeNetworks)) } label: {
          Image(systemName: "xmark").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
        }
        .padding(.trailing, -12)  // the ✕ itself on the switches' edge; the target stays 44 pt
        .accessibilityLabel(VPNCopy.removeHomeNetwork(name))
        .accessibilityIdentifier(VPNIDs.removeHomeNetwork(name))
      }
    }
    Button(VPNCopy.addHomeNetwork) {
      newHome = ""
      addingHome = true
    }
    .buttonStyle(QuietButtonStyle(color: Theme.accent))
    .padding(.leading, -8)  // the style's own inset: the words line up with the card's
    .frame(maxWidth: .infinity, alignment: .leading)
    .accessibilityIdentifier(VPNIDs.addHomeNetwork)
    .alert(VPNCopy.addHomeNetwork, isPresented: $addingHome) {
      // Return adds it, as Add does.
      TextField(VPNCopy.homeNetworkField, text: $newHome)
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .accessibilityIdentifier(VPNIDs.homeNetworkField)
        .onSubmit { addHome() }
      Button(VPNCopy.add) { addHome() }
        .disabled(VPNFlow.adding(newHome, to: []).isEmpty)  // blank or too long for an SSID
        .accessibilityIdentifier(VPNIDs.homeNetworkAddConfirm)
      Button(VPNCopy.cancel, role: .cancel) {}
    } message: {
      Text(VPNCopy.homeNetworkPrompt)
    }
  }

  private func addHome() {
    addingHome = false
    setAutoConnect(true, VPNFlow.adding(newHome, to: vpn.homeNetworks))
  }

  private func setAutoConnect(_ on: Bool, _ home: [String]) {
    let service = service
    Task { await service.setAutoConnect(on, homeNetworks: home) }
  }

  private func switchMoved(to on: Bool) {
    switch VPNFlow.step(switchingTo: on, vpn) {
    case .askConsent: if !switching { askingConsent = true }
    case .turnOn: switchOn { await $0.setOn(true) }
    case .turnOff:
      let service = service
      Task { await service.setOn(false) }
    case .nothing: break
    }
  }

  /// One switch-on at a time.
  private func switchOn(_ work: @escaping @MainActor (VPNService) async -> Void) {
    guard !switching else { return }
    switching = true
    let service = service
    Task { @MainActor in
      await work(service)
      switching = false
    }
  }
}

/// Paste the VPN code from the server. A wrong one says so under the field and stays open.
private struct VPNCodeSheet: View {
  let service: VPNService
  let note: String?
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
          if let note {
            Text(note)
              .font(.footnote.weight(.semibold))
              .foregroundStyle(Theme.title)
              .fixedSize(horizontal: false, vertical: true)
          }
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
  let title: String
  let message: String
  let turnOn: () -> Void
  let notNow: () -> Void
  /// The words and buttons as laid out: the sheet is that tall and no taller. Taller than the
  /// screen (large type), iOS stops at full height and the page scrolls.
  @State private var height: CGFloat = 0

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        Text(title)
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
      .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { height = $0 }
    }
    .scrollBounceBehavior(.basedOnSize)
    .background(Theme.ground)
    .presentationDetents(height > 0 ? [.height(height)] : [.medium])
  }
}
