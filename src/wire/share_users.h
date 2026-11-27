// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The self-hosted computer's users and their roles (docs/plans/self-host-users-smb.md):
// who may open its folders as a LAN SMB share, and what a phone paired for
// them may do. Kept in the host service's own store beside the owner's
// passphrase (`share_users`), edited by whoever signed in as admin (that
// passphrase) from the `driftwire` CLI or over the desktop's owner session.
//
// No password is ever here: an SMB password is the OS account's, typed and
// handed to the OS once by the elevated step (addons/driftwire/host_smb.h).
// Names and roles are shown on the person's own screen only, never logged
// (rule 6). Portable (D9); no I/O beyond the store.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/grants.h"
#include "wire/secrets.h"

namespace mv::driftwire::serve::share_users {

inline constexpr const char* kUsersName = "share_users";
inline constexpr std::size_t kMaxUsers = 32;
inline constexpr std::size_t kMaxNameBytes = 20;   // a Windows local account's most
inline constexpr std::size_t kMaxLabelBytes = 64;
inline constexpr std::size_t kMaxFolders = 32;

// What a user may do with the files (plan §2). The order is "more": view < change < admin.
enum class role : std::uint8_t { view, change, admin };
[[nodiscard]] const char* role_name(role r) noexcept;   // "view", "change", "admin"
[[nodiscard]] const char* role_words(role r) noexcept;  // "can view files", "can change files", "admin"
[[nodiscard]] std::optional<role> parse_role(std::string_view s) noexcept;  // the names above

struct user {
  std::string name;
  role r = role::view;
  // Labels of the host's folders it may open; nullopt: every folder. Never empty.
  std::optional<std::vector<std::string>> folders;
  std::int64_t created = 0;  // unix seconds
  std::int64_t applied = 0;  // when the OS last took it (0: waiting for the elevated step)
  [[nodiscard]] bool sees(std::string_view label) const;
};

// 1 to kMaxNameBytes of [a-z0-9._-], a letter first, and none the OS keeps for
// itself (admin, guest, root, everyone, ...; anything starting with '_').
// `self`: the account running MediaViewer, refused too ("" for none known).
[[nodiscard]] bool valid_name(std::string_view name, std::string_view self = {});
// A folder label as `host share --label` keeps it: 1 to kMaxLabelBytes, no control characters.
[[nodiscard]] bool valid_label(std::string_view label);

[[nodiscard]] std::string encode(const std::vector<user>& list);
// nullopt: not that shape, a name twice, a bad name or role. Never partial.
[[nodiscard]] std::optional<std::vector<user>> decode(std::string_view json);

// false: there and unreadable (never replaced: a broken file is not an empty list).
// Missing: an empty list, true.
[[nodiscard]] bool load(secrets::store& store, std::vector<user>& out);
[[nodiscard]] bool save(secrets::store& store, const std::vector<user>& list);

// Adds or changes `u` by name (its `created` kept, its `applied` cleared when
// the role or folders changed). "" when done, else the reason ("bad-name",
// "bad-folders", "too-many").
[[nodiscard]] std::string upsert(std::vector<user>& list, user u, std::int64_t now, std::string_view self = {});
// True when one went.
bool remove(std::vector<user>& list, std::string_view name);
[[nodiscard]] const user* find(const std::vector<user>& list, std::string_view name);

// ---- what each role is on each OS (plan §2) -------------------------------------------------
// Windows: the share's permission and the folder's NTFS rights, as access masks.
[[nodiscard]] std::uint32_t windows_share_mask(role r) noexcept;
[[nodiscard]] std::uint32_t windows_ntfs_mask(role r) noexcept;
// macOS: the folder's ACL entry, as `chmod +a` writes its permissions.
[[nodiscard]] std::string mac_acl_perms(role r);
// A phone paired for this user (plan §9): view backs nothing up, change may,
// admin has no limit; its folders become root ids (`ids_of(label)`, nullopt
// for a label not shared: dropped).
template <typename IdOf>
[[nodiscard]] grants::limit phone_limit(const user& u, IdOf ids_of) {
  grants::limit l;
  if (u.r == role::admin) return l;
  l.backup = u.r == role::change;
  if (u.folders) {
    std::vector<std::string> ids;
    for (const auto& f : *u.folders) {
      if (auto id = ids_of(f)) ids.push_back(*id);
    }
    l.roots = std::move(ids);
  }
  return l;
}

// ---- the share's names and addresses (plan §7) ----------------------------------------------
// An SMB share name for a folder's label: none of "/\[]:|<>+=;,?*, no control
// characters, at most 80, no leading or trailing space or dot; "Share" when nothing is left.
[[nodiscard]] std::string share_name(std::string_view label);
// "\\PC\Photos"; "" without a computer name.
[[nodiscard]] std::string unc_address(std::string_view computer, std::string_view share);
// "smb://pc.local/My%20Photos"; `host` as Bonjour has it ("pc.local"); "" without one.
[[nodiscard]] std::string smb_url(std::string_view host, std::string_view share);

}  // namespace mv::driftwire::serve::share_users
