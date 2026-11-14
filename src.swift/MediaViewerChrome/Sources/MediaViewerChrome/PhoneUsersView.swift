// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access → This computer → Users (docs/plans/self-host-users-smb.md §8):
// who may open this Mac's shared folders from other computers at home, and
// with what role. Admin unlock first (the server passphrase, the card's own
// unlock); then each change goes to the host's service over the owner session
// and to this Mac's file sharing through one administrator approval. A
// password is typed here, handed on once to the driftwire program's stdin and
// cleared: nothing on this Mac keeps it.
import SwiftUI

extension PhoneStore {
  /// One of the host's users, as its service last said (names and roles only).
  struct User: Equatable, Identifiable {
    var name: String
    var role: String  // "view" | "change" | "admin"
    var folders: [String]?  // nil: every folder
    var applied = false
    var id: String { name }
  }

  nonisolated static let roles: [(value: String, label: String)] = [
    ("view", "Can view files"), ("change", "Can change files"), ("admin", "Admin"),
  ]

  nonisolated static func roleLabel(_ role: String) -> String {
    roles.first { $0.value == role }?.label ?? "Can view files"
  }

  /// A name the service takes (share_users::valid_name): a-z first, then a-z 0-9 . _ -, 20 at most.
  nonisolated static func validUserName(_ name: String) -> Bool {
    let reserved: Set<String> = ["admin", "administrator", "administrators", "guest", "guests", "root", "nobody",
                                 "daemon", "everyone", "system", "users", "user", "wheel", "staff", "operator"]
    guard let first = name.unicodeScalars.first, ("a"..."z").contains(first), name.count <= 20,
          !name.hasSuffix("."), !reserved.contains(name) else { return false }
    return name.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || "._-".unicodeScalars.contains($0) }
  }

  nonisolated static func userDetail(_ u: User) -> String {
    var parts = [roleLabel(u.role)]
    if let f = u.folders, !f.isEmpty { parts.append(f.joined(separator: ", ")) }
    if !u.applied { parts.append("Waiting for your Mac's administrator") }
    return parts.joined(separator: " · ")
  }

  nonisolated static func parseUsers(_ o: [String: Any]) -> [User]? {
    guard let list = o["users"] as? [[String: Any]] else { return nil }
    return list.compactMap {
      guard let n = $0["name"] as? String, let r = $0["role"] as? String else { return nil }
      return User(name: n, role: r, folders: $0["folders"] as? [String], applied: $0["applied"] as? Bool ?? false)
    }
  }
}

struct PhoneUsersSection: View {
  @ObservedObject private var model = PhoneStore.shared
  @State private var unlockWith = ""
  @State private var adding = false
  @State private var newName = ""
  @State private var newRole = "view"
  @State private var newPassword = ""
  @State private var newPasswordAgain = ""
  private var s: PhoneStore.State { model.state }
  private var locked: Bool { s.nasPassphrase == "set" && s.unlockedSeconds == 0 }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      SettingsRow(title: "Users",
                  detail: "People who can open your shared folders from other computers at home, with their own name and password. For your home network only: MediaViewer never opens it on your router.",
                  labelsContent: false) {
        if locked {
          EmptyView()
        } else if !adding {
          SettingsButton(title: "Add user…") { adding = true }
            .disabled(!s.busy.isEmpty)
            .accessibilityLabel("Add a user")
        }
      }
      if locked {
        unlockRow
      } else {
        ForEach(s.users ?? []) { u in userRow(u) }
        if adding { addRows }
        if (s.users ?? []).contains(where: { !$0.applied }) {
          SettingsRow(title: "Waiting for your Mac's administrator",
                      detail: "Your Mac asks for an administrator to make these accounts and share the folders.",
                      labelsContent: false) {
            SettingsButton(title: "Finish…") { model.send(["do": "users_apply"]) }
              .disabled(!s.busy.isEmpty)
              .accessibilityLabel("Finish with your Mac's administrator")
          }
        }
      }
      ForEach(s.shareAddresses, id: \.self) { a in
        SettingsRow(title: a, detail: "", labelsContent: false) {
          SettingsButton(title: "Copy") {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(a, forType: .string)
          }
          .accessibilityLabel("Copy \(a)")
        }
      }
    }
    .onAppear { model.send(["do": "users_refresh"]) }
    .onDisappear { forget() }
  }

  // Admin first: the server passphrase, as the card's own unlock.
  private var unlockRow: some View {
    SettingsRow(title: "Unlock as admin",
                detail: "Enter the server passphrase to add or change users. This Mac keeps the key for 10 minutes and never the passphrase.",
                labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        SecureField("Server passphrase", text: $unlockWith)
          .onSubmit(unlock)
          .textFieldStyle(.roundedBorder).frame(width: 200).font(MVTheme.font(12))
          .accessibilityLabel("Server passphrase")
        SettingsButton(title: "Unlock", action: unlock)
          .disabled(unlockWith.isEmpty || !s.busy.isEmpty)
      }
    }
  }

  private func unlock() {
    guard !unlockWith.isEmpty else { return }
    model.send(["do": "unlock", "passphrase": unlockWith])
    unlockWith = ""
  }

  private func userRow(_ u: PhoneStore.User) -> some View {
    SettingsRow(title: u.name, detail: PhoneStore.userDetail(u), labelsContent: false) {
      HStack(spacing: MVTheme.space(8)) {
        Picker("Role", selection: Binding(get: { u.role }, set: { role in
          var r: [String: Any] = ["do": "user_set", "name": u.name, "role": role]
          if let f = u.folders { r["folders"] = f }
          model.send(r)
        })) {
          ForEach(PhoneStore.roles, id: \.value) { Text($0.label).tag($0.value) }
        }
        .pickerStyle(.menu).frame(width: 160)
        .disabled(!s.busy.isEmpty)
        .accessibilityLabel("Role for \(u.name)")
        SettingsButton(title: "Remove") { model.send(["do": "user_remove", "name": u.name]) }
          .disabled(!s.busy.isEmpty)
          .accessibilityLabel("Remove \(u.name)")
      }
    }
  }

  @ViewBuilder private var addRows: some View {
    SettingsRow(title: "Name", detail: "Lowercase letters and digits, starting with a letter. Their sign-in from other computers.") {
      TextField("anna", text: $newName).textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
    }
    SettingsRow(title: "Role", detail: "") {
      Picker("Role", selection: $newRole) {
        ForEach(PhoneStore.roles, id: \.value) { Text($0.label).tag($0.value) }
      }
      .pickerStyle(.menu).frame(width: 240)
    }
    SettingsRow(title: "Password", detail: "At least 8 characters. Nothing on this Mac keeps it.") {
      SecureField("", text: $newPassword).textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
    }
    SettingsRow(title: "Again", detail: "") {
      SecureField("", text: $newPasswordAgain).textFieldStyle(.roundedBorder).frame(width: 240).font(MVTheme.font(12))
    }
    HStack(spacing: MVTheme.space(8)) {
      Spacer()
      SettingsButton(title: "Cancel") { forget() }
      SettingsButton(title: "Add") {
        model.send(["do": "user_set", "name": newName, "role": newRole, "password": newPassword])
        forget()
      }
      .disabled(!s.busy.isEmpty || !PhoneStore.validUserName(newName) || newPassword.utf8.count < 8 ||
                newPassword != newPasswordAgain)
      .accessibilityLabel("Add the user; your Mac asks for its administrator")
    }
  }

  private func forget() {
    adding = false
    newName = ""
    newRole = "view"
    newPassword = ""
    newPasswordAgain = ""
    unlockWith = ""
  }
}
