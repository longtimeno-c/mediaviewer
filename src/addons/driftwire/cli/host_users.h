// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host users` (docs/plans/self-host-users-smb.md): who may open the
// self-hosted computer's folders from other computers at home, as a LAN SMB
// share, and with what role.
//
//   driftwire host users                       the list: names and roles (no sign-in)
//   driftwire host users add NAME [--role R] [--folders L1,L2|all]
//   driftwire host users role NAME ROLE [--folders L1,L2|all]
//   driftwire host users password NAME
//   driftwire host users remove NAME
//   driftwire host users apply [--input-stdin] what the list says, made so on this
//                                              computer (the app's: NAME<TAB>PASSWORD lines)
//   driftwire host smb-apply [CHANNEL]         the elevated step itself: never typed
//
// add, role, password and remove ask for the admin passphrase first (the owner's,
// owner_pass.h; misses count on the owner's lockout); the passwords are typed
// with echo off, or lines on stdin with --input-stdin, never arguments. The
// list is kept in the host service's store (share_users.h); every change to
// this computer's accounts and shares goes through one elevated step the
// person approves (elevate.h, host_smb.h). The always-running host never
// elevates: this command starts the step and waits for it.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/elevate.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/host_smb.h"
#include "wire/secrets.h"
#include "wire/share_users.h"

namespace mv::driftwire::cli {

namespace su = serve::share_users;

enum class users_verb : std::uint8_t { list, add, role, password, remove, apply };
struct users_command {
  users_verb verb = users_verb::list;
  std::string name;
  std::optional<su::role> r;  // add: --role (view when none); role: the role
  // --folders: nullopt not given; a nullopt inside: "all".
  std::optional<std::optional<std::vector<std::string>>> folders;
  bool input_stdin = false;
};
// The words after `host users` and their options; "" when good, else the usage error.
[[nodiscard]] std::string parse_users_command(const args& a, users_command& out);

// ---- the admin sign-in: the owner's passphrase ------------------------------------------------
enum class admin_answer : std::uint8_t { ok, wrong, locked, none_set, unreadable };
// Checks `passphrase` against the verifier in `store` (Argon2id at its cost);
// a miss counts on the owner's lockout, as over the wire. `until`: when locked.
[[nodiscard]] admin_answer check_admin(secrets::store& store, std::string_view passphrase, std::int64_t now,
                                       std::int64_t& until);

// ---- what the step last made, for `host status` ------------------------------------------------
inline constexpr const char* kShareStateName = "share_state";
struct share_state {
  std::string computer, mdns_host;
  std::vector<std::string> shares;  // the shares made, by name
};
[[nodiscard]] std::optional<share_state> load_share_state(secrets::store& store);
[[nodiscard]] bool save_share_state(secrets::store& store, const share_state& s);
// `host status`'s lines: the share's addresses and the users, names and roles only.
[[nodiscard]] std::vector<std::string> users_status_lines(secrets::store& store);

// ---- running it (the tests give their own env) ----------------------------------------------
struct users_env {
  // One line: shown `prompt` at a terminal, echo off when `hidden`. False at the end of input.
  std::function<bool(const std::string& prompt, std::string& out, bool hidden)> ask;
  std::function<void(const std::string& line)> say;
  // The elevated step, given its request (the passwords in it).
  std::function<elevated_run(std::string_view request)> elevate;
  std::function<std::int64_t()> now;
  std::string self;       // the OS account running this (never a user's name)
  bool terminal = true;   // a person is here: ask before the OS does
  bool any_port = false;  // serve.json's port 0 (the test build)
};
[[nodiscard]] int run_users(const users_command& c, const host_layout& l, users_env& env);

// Runs a.host == users with the real terminal, sudo / the dialog / UAC.
[[nodiscard]] int run_host_users(const args& a, const host_layout& l, bool test_hooks);
// Runs a.host == smb_apply: the elevated step (refuses when not elevated).
[[nodiscard]] int run_smb_apply(const args& a);

}  // namespace mv::driftwire::cli
