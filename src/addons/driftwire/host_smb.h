// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The self-hosted computer's LAN SMB share (docs/plans/self-host-users-smb.md §5):
// the OS's own file sharing, a sharing-only account per user, each share's
// folder given exactly its users' rights. Made only by the elevated step
// (`driftwire host smb-apply`), which the person approves each time; never by
// the always-running host, which refuses elevation.
//
//   front end (`host users …`, unprivileged)       elevated step (`host smb-apply`)
//   plan(users, folders, passwords) -> request  ->  parse_request -> apply(request, os_port())
//   parse_answer <- answer_json                 <-  answer (no password, no path)
//
// What the step never does: change or delete an account or a share it did not
// make (each OS marks its own); turn on SMB1 or guest access; touch a file in a
// shared folder (rule 5); forward a port (host_ports refuses 139/445). The
// passwords live in a request only, are wiped when it goes, and are never in an
// answer, a log or a crash report (rule 6).
//
// One interface, one file per OS (D9): host_smb_mac.cpp (Open Directory,
// `sharing`, ACLs), host_smb_win.cpp (NetUserAdd, NetShareAdd, NTFS ACLs),
// host_smb_none.cpp (Linux: Samba comes with the Linux host). The tests give a
// stand-in port; nothing in a test makes a real account or share.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wire/share_users.h"

namespace mv::driftwire::host_smb {

using role = serve::share_users::role;

inline constexpr std::size_t kMaxPasswordBytes = 256;
inline constexpr std::size_t kMaxRequestBytes = 64 * 1024;

struct share_want {
  std::string name;  // share_users::share_name(label)
  std::string path;  // the folder, absolute
};
struct member {
  std::string user;
  role r = role::view;
};
struct user_want {
  std::string name;
  role r = role::view;
  std::vector<std::string> shares;  // share names it may open
  std::string password;             // "" keeps the one it has (an account already made)
};

// What the elevated step is asked to do. Holds passwords: never copied, wiped when it goes.
struct request {
  std::vector<share_want> shares;
  std::vector<user_want> users;
  std::vector<std::string> remove;  // users to take away (their accounts, if MediaViewer's)
  request() = default;
  request(request&&) = default;
  request& operator=(request&&) = default;
  request(const request&) = delete;
  request& operator=(const request&) = delete;
  ~request();
  [[nodiscard]] bool empty() const noexcept { return shares.empty() && users.empty() && remove.empty(); }
};
// The request as it goes down the pipe (with its passwords: never written anywhere else).
[[nodiscard]] std::string request_json(const request& r);
// Every field checked again (the step trusts nothing an unprivileged process
// sent): names valid, paths absolute, roles known, shares named in `shares`,
// no name twice. nullopt with `why`.
[[nodiscard]] std::optional<request> parse_request(std::string_view json, std::string& why);

struct item {
  std::string name;
  bool ok = false;
  std::string why;  // plain words; never a path or a password
};
struct answer {
  bool ok = false;   // everything asked was done
  std::string why;   // when nothing could be done at all
  std::vector<item> users, shares;
  std::string computer;   // "STUDIO": \\STUDIO\Photos
  std::string mdns_host;  // "studio.local": smb://studio.local/Photos
};
[[nodiscard]] std::string answer_json(const answer& a);
[[nodiscard]] std::optional<answer> parse_answer(std::string_view json);

// What this computer offers.
struct facts {
  bool supported = false;
  std::string why_not;    // when not supported, in words
  std::string computer;   // the NetBIOS / computer name
  std::string mdns_host;  // the Bonjour name with ".local"
  bool smb1_on = false;   // the SMB1 server is on (Windows): the step refuses
  bool service_on = true; // file sharing is running
};

// The OS's side. Every call is the elevated step's, on its own thread, and
// may block (accounts, shares, ACLs).
class port {
 public:
  enum class owner : std::uint8_t { none, ours, theirs };
  virtual ~port() = default;
  [[nodiscard]] virtual facts probe() = 0;
  // An account of this name: none, one MediaViewer made, or anyone else's.
  [[nodiscard]] virtual owner account(const std::string& name) = 0;
  // "" when done, else why (plain words). A sharing-only account, marked as MediaViewer's.
  [[nodiscard]] virtual std::string make_account(const std::string& name, std::string_view password) = 0;
  [[nodiscard]] virtual std::string set_password(const std::string& name, std::string_view password) = 0;
  [[nodiscard]] virtual std::string delete_account(const std::string& name) = 0;
  [[nodiscard]] virtual owner share(const std::string& name) = 0;
  // Makes the share (or changes one of ours) for `s.path`, encrypted, no guest,
  // with exactly `members` on it and on the folder: the entries any of
  // `ours` had there before go first.
  [[nodiscard]] virtual std::string put_share(const share_want& s, const std::vector<member>& members,
                                              const std::vector<std::string>& ours) = 0;
  // Removes a share of ours and `ours`' entries on its folder (the files stay).
  [[nodiscard]] virtual std::string remove_share(const std::string& name, const std::vector<std::string>& ours) = 0;
  // The shares MediaViewer made, by name.
  [[nodiscard]] virtual std::vector<std::string> our_shares() = 0;
  // The accounts MediaViewer made (marked as its own), by name: any the request
  // does not list is taken away, so a removal the step never heard of still happens.
  [[nodiscard]] virtual std::vector<std::string> our_accounts() = 0;
  // Turns file sharing on (said before the person approved).
  [[nodiscard]] virtual std::string start_service() = 0;
};
[[nodiscard]] std::unique_ptr<port> os_port();

// ---- the step's checks of a share's folder (before anything changes) ----------------------
// A folder the step never shares, whoever asks: the system's root, its volume
// root on Windows, the system's own trees, the folder holding every account's
// home. "" when it is not one, else why. Pure: every OS's tests run it.
[[nodiscard]] std::string refuse_system_folder(std::string_view path, bool windows);
// Per OS: refuse_system_folder, then the folder as given is the folder itself
// (no link or reparse point anywhere in it) and belongs to the person who started
// the step (or is the root of a drive that is not the system's). "" when good.
[[nodiscard]] std::string check_share_folder(const std::string& path);
// Per OS, first thing in the step: who started it, before it acts as the administrator.
void note_invoker();

// The elevated step's work, on any port: accounts made or updated, then shares
// (so a removed user's entries go while its account still resolves), then
// accounts taken away (ours only: those asked, and any of ours the request does
// not list), the service started if a share was made and it is off. Only
// accounts that are MediaViewer's have their entries removed from a folder.
[[nodiscard]] answer apply(const request& r, port& p);

// ---- the front end (pure) -------------------------------------------------------------------
struct folder {
  std::string label, path;
};
struct password_for {
  std::string user, password;
  ~password_for();
};
// What to ask of the step: every share some user may open (a share no one
// may open is left out, and the step removes it if it was ours), every user
// with its shares, the passwords given, and the users taken away.
[[nodiscard]] request plan(const std::vector<serve::share_users::user>& users, const std::vector<folder>& folders,
                           std::vector<password_for>& passwords, const std::vector<std::string>& removed);
// What the step will do, in plain lines for the person before the OS asks
// (no password, no path).
[[nodiscard]] std::vector<std::string> explain(const request& r, const facts& f);
// A password the step accepts: 8 to kMaxPasswordBytes bytes, no control characters
// (a pipe line carries it; the OS has its own rules besides).
[[nodiscard]] bool acceptable_password(std::string_view p) noexcept;

}  // namespace mv::driftwire::host_smb
