// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_users.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <span>
#include <thread>

#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/serve_tty.h"
#include "addons/driftwire/cli/update.h"
#include "core/json.h"
#include "wire/net.h"
#include "wire/owner_pass.h"
#include "wire/serve.h"

namespace mv::driftwire::cli {

namespace {

namespace owner = serve::owner;

struct wiped {
  std::string s;
  wiped() { s.reserve(host_smb::kMaxPasswordBytes + owner::kMaxBytes + 2); }  // never reallocated: no copy left unwiped
  ~wiped() {
    if (!s.empty()) sodium_memzero(s.data(), s.size());
  }
  wiped(const wiped&) = delete;
  wiped& operator=(const wiped&) = delete;
};

std::vector<std::string> split_labels(std::string_view text) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= text.size()) {
    const auto comma = text.find(',', at);
    std::string_view part = text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
    while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
    if (!part.empty()) out.emplace_back(part);
    if (comma == std::string_view::npos) break;
    at = comma + 1;
  }
  return out;
}

std::string pad(std::string_view label) {
  std::string out(label);
  if (out.size() < 12) out.resize(12, ' ');
  return out;
}

// The lock the service takes around its own changes of the store (serve-devices.lock).
std::unique_ptr<net::instance_lock> store_lock(const host_layout& l) {
  std::unique_ptr<net::instance_lock> held;
  for (int i = 0; i < 30 && !held; ++i) {
    held = net::try_lock_instance(l.serve + "/serve-devices.lock");
    if (!held) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return held;
}

std::vector<host_smb::folder> lan_folders(const host_layout& l, bool any_port) {
  std::vector<host_smb::folder> out;
  const auto cfg = host_files::config(l, any_port);
  if (!cfg) return out;
  for (const auto& r : cfg->roots) {
    if (r.label == serve::kCatalogueLabel) continue;  // the host library's own: never a network share
    out.push_back({r.label, r.path});
  }
  return out;
}

std::string user_line(const su::user& u) {
  std::string line = pad("user") + u.name + "  " + su::role_words(u.r);
  if (u.folders) {
    line += "  (";
    for (std::size_t i = 0; i < u.folders->size(); ++i) line += (i ? ", " : "") + (*u.folders)[i];
    line += ")";
  }
  if (u.applied == 0) line += "  waiting for an administrator (driftwire host users apply)";
  return line;
}

}  // namespace

std::vector<su::user> desk_users(const std::vector<serve::device_shares::desk>& desks,
                                 const std::vector<known::device>& unlimited,
                                 const std::vector<serve::grants::grant>& limited,
                                 const std::vector<serve::root_config>& roots, std::vector<std::string>& gone) {
  namespace ds = serve::device_shares;
  std::vector<su::user> out;
  for (const auto& d : desks) {
    if (d.gone) {
      gone.push_back(d.account);
      continue;
    }
    const bool full = std::any_of(unlimited.begin(), unlimited.end(), [&](const auto& u) { return u.key == d.key; });
    const auto g = std::find_if(limited.begin(), limited.end(), [&](const auto& x) { return x.key == d.key; });
    if (!full && g == limited.end()) continue;
    std::vector<serve::root_config> shared;
    for (const auto& r : roots) {
      if (r.label != serve::kCatalogueLabel) shared.push_back(r);
    }
    const ds::want w = ds::want_of(full ? nullptr : &*g, shared);
    su::user u;
    u.name = d.account;
    u.r = w.role == "change" ? su::role::change : su::role::view;
    u.folders = w.folders;
    u.applied = ds::waiting(d, w.role, w.folders) ? 0 : d.applied;
    out.push_back(std::move(u));
  }
  return out;
}

// ---- the command's words ---------------------------------------------------------------------

std::string parse_users_command(const args& a, users_command& out) {
  out = users_command{};
  out.input_stdin = a.input_stdin;
  const auto& w = a.host_words;
  const std::string verb = w.empty() ? std::string() : w.front();
  const auto need = [&](std::size_t n) { return w.size() == n; };
  if (w.empty()) {
    out.verb = users_verb::list;
  } else if (verb == "add" && need(2)) {
    out.verb = users_verb::add;
  } else if (verb == "role" && need(3)) {
    out.verb = users_verb::role;
    out.r = su::parse_role(w[2]);
    if (!out.r) return "the role is view, change or admin";
  } else if (verb == "password" && need(2)) {
    out.verb = users_verb::password;
  } else if (verb == "remove" && need(2)) {
    out.verb = users_verb::remove;
  } else if (verb == "apply" && need(1)) {
    out.verb = users_verb::apply;
  } else {
    // A password is never an argument: `host users add anna hunter2` is refused here.
    return "host users takes add NAME, role NAME ROLE, password NAME, remove NAME or apply (a password is "
           "typed, never an argument)";
  }
  if (out.verb != users_verb::list && out.verb != users_verb::apply) out.name = w[1];
  if (!a.role.empty()) {
    if (out.verb != users_verb::add) return "--role goes with host users add (host users role NAME ROLE changes one)";
    out.r = su::parse_role(a.role);
    if (!out.r) return "the role is view, change or admin";
  }
  if (!a.folders.empty()) {
    if (out.verb != users_verb::add && out.verb != users_verb::role) return "--folders goes with host users add or role";
    if (a.folders == "all") {
      out.folders = std::optional<std::vector<std::string>>{};
    } else {
      auto labels = split_labels(a.folders);
      if (labels.empty()) return "--folders takes labels (Photos,Videos) or all";
      out.folders = std::optional<std::vector<std::string>>{std::move(labels)};
    }
  }
  if (out.input_stdin && out.verb == users_verb::list) return "--input-stdin goes with a change";
  if (!out.name.empty() && !su::valid_name(out.name)) {
    return "a user's name is 1 to 20 of a-z, 0-9, '.', '_' or '-', starting with a letter, and not one the system "
           "keeps (admin, guest, root, ...)";
  }
  return {};
}

// ---- the admin sign-in -------------------------------------------------------------------------

admin_answer check_admin(secrets::store& store, std::string_view passphrase, std::int64_t now, std::int64_t& until) {
  until = 0;
  owner::verifier v;
  switch (owner::load(store, v)) {
    case owner::stored::none:
      return admin_answer::none_set;
    case owner::stored::unreadable:
      return admin_answer::unreadable;
    case owner::stored::set:
      break;
  }
  owner::lockout l = owner::load_lockout(store, now);
  if (l.owner.locked(now)) {
    until = l.owner.until;
    return admin_answer::locked;
  }
  // Counted as a miss before it is checked, as the service does (serve.cpp answer()):
  // a store that cannot be written must never mean guesses that are never counted.
  l.owner.miss(now);
  if (!owner::save_lockout(store, l)) return admin_answer::unreadable;
  bool right = false;
  if (owner::acceptable(passphrase) && owner::sane_cost(v.ops, v.mem)) {
    if (auto s = owner::derive(passphrase, v.salt, v.ops, v.mem)) right = sodium_memcmp(s->pk.data(), v.pk.data(), v.pk.size()) == 0;
  }
  if (right) {
    l.owner.success();
    (void)owner::save_lockout(store, l);  // if this fails the miss stays counted: never in the guesser's favour
    return admin_answer::ok;
  }
  until = l.owner.locked(now) ? l.owner.until : 0;
  return until ? admin_answer::locked : admin_answer::wrong;
}

// ---- what the step made ----------------------------------------------------------------------

std::optional<share_state> load_share_state(secrets::store& store) {
  if (!store.exists(kShareStateName)) return std::nullopt;
  const auto bytes = store.get(kShareStateName);
  if (!bytes) return std::nullopt;
  const auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), 3);
  if (!doc || doc->integer("v") != 1) return std::nullopt;
  share_state s;
  const std::string* c = doc->str("computer");
  const std::string* m = doc->str("mdns_host");
  const json::value* list = doc->find("shares");
  if (!c || !m || !list || list->k != json::kind::array) return std::nullopt;
  s.computer = *c;
  s.mdns_host = *m;
  for (const auto& e : list->a) {
    if (e.k == json::kind::string) s.shares.push_back(e.s);
  }
  return s;
}

bool save_share_state(secrets::store& store, const share_state& s) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("computer").string(s.computer).key("mdns_host").string(s.mdns_host);
  w.key("shares").begin_array();
  for (const auto& n : s.shares) w.string(n);
  w.end_array().end_object();
  const std::string text = w.take();
  return store.put(kShareStateName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::vector<std::string> users_status_lines(secrets::store& store) {
  std::vector<std::string> out;
  std::vector<su::user> users;
  if (!su::load(store, users)) {
    out.push_back(pad("LAN share") + "the list of users cannot be read");
    return out;
  }
  const auto state = load_share_state(store);
  if (users.empty()) {
    out.push_back(pad("LAN share") + "off (driftwire host users add NAME)");
    return out;
  }
  if (!state || state->shares.empty()) {
    out.push_back(pad("LAN share") + "not made yet (driftwire host users apply)");
  } else {
    for (const auto& s : state->shares) {
      std::string line = pad("LAN share");
      const std::string unc = su::unc_address(state->computer, s);
      const std::string url = su::smb_url(state->mdns_host, s);
      line += unc.empty() ? s : unc;
      if (!url.empty()) line += "   " + url;
      out.push_back(line);
    }
    out.push_back(pad("") + "encrypted (SMB3), no guests; for your home network only, never on the router");
  }
  std::string names = pad("users");
  std::string waiting;
  for (std::size_t i = 0; i < users.size(); ++i) {
    names += (i ? ", " : "") + users[i].name + " (" + su::role_words(users[i].r) + ")";
    if (users[i].applied == 0) waiting += (waiting.empty() ? "" : ", ") + users[i].name;
  }
  out.push_back(names);
  if (!waiting.empty()) out.push_back(pad("") + "waiting for an administrator: " + waiting + " (driftwire host users apply)");
  return out;
}

// ---- running it ------------------------------------------------------------------------------

namespace {

// A new password, typed twice (or one line of stdin).
bool ask_password(users_env& env, const users_command& c, const std::string& name, wiped& out, std::string& why) {
  if (!env.ask("Password for " + name + " (at least 8 characters; their sign-in from other computers): ", out.s,
               true)) {
    why = "no password given.";
    return false;
  }
  if (!c.input_stdin) {
    wiped again;
    if (!env.ask("Again: ", again.s, true) || again.s.size() != out.s.size() ||
        sodium_memcmp(again.s.data(), out.s.data(), out.s.size()) != 0) {
      why = "the two did not match; nothing changed.";
      return false;
    }
  }
  if (!host_smb::acceptable_password(out.s)) {
    why = "a password needs 8 to 256 characters; nothing changed.";
    return false;
  }
  return true;
}

// The step, for what the list says now; the applied stamps and the share's state kept.
int apply_now(const host_layout& l, secrets::store& store, users_env& env, std::vector<host_smb::password_for>& passwords,
              const std::vector<std::string>& removed) {
  std::vector<su::user> users;
  if (!su::load(store, users)) {
    env.say("driftwire host: the list of users cannot be read; nothing changed.");
    return 1;
  }
  // Each paired computer is a user too (§12): its account, a fresh password for one not made yet.
  namespace ds = serve::device_shares;
  std::vector<ds::desk> desks;
  if (!ds::load(store, desks)) {
    env.say("driftwire host: the list of paired computers cannot be read; nothing changed.");
    return 1;
  }
  std::vector<std::string> desk_removed = removed;
  std::vector<std::string> desk_names;
  {
    std::vector<serve::grants::grant> limited;
    (void)serve::grants::load(store, limited);
    const auto cfg = host_files::config(l, env.any_port);
    auto mine = desk_users(desks, known::load(store), limited,
                           cfg ? cfg->roots : std::vector<serve::root_config>{}, desk_removed);
    for (auto& u : mine) {
      desk_names.push_back(u.name);
      const auto* d = [&]() -> const ds::desk* {
        for (const auto& x : desks) {
          if (x.account == u.name) return &x;
        }
        return nullptr;
      }();
      if (d && (d->password.empty() || d->applied == 0)) {
        // Made (or made again) with the password the service will hand that computer. An
        // account already made keeps its own: a role or folder change sends none.
        host_smb::password_for p;
        p.user = u.name;
        p.password = d->password.empty() ? ds::new_password() : d->password;
        passwords.push_back(std::move(p));
      }
      users.push_back(std::move(u));
    }
  }
  // The passwords as given to the step, by account, kept to be saved once it made the account.
  std::vector<host_smb::password_for> desk_passwords;
  for (const auto& p : passwords) {
    if (std::find(desk_names.begin(), desk_names.end(), p.user) != desk_names.end()) desk_passwords.push_back({p.user, p.password});
  }
  const auto folders = lan_folders(l, env.any_port);
  if (folders.empty() && !users.empty()) {
    env.say("driftwire host: no folder is shared yet (driftwire host share PATH); the users are kept for when one is.");
    return 0;
  }
  host_smb::request r = host_smb::plan(users, folders, passwords, desk_removed);
  if (r.empty()) {
    env.say("Nothing to change.");
    return 0;
  }
  host_smb::facts f;  // what the front end knows: the step says what the OS has
  f.service_on = true;
  env.say("This needs your computer's administrator once. It will:");
  for (const auto& line : host_smb::explain(r, f)) env.say("  " + line);
  env.say("This share is for your home network only. MediaViewer never opens it on your router.");
  if (env.terminal) {
    std::string yes;
    if (!env.ask("Go ahead? [y/N] ", yes, false) || (yes != "y" && yes != "Y" && yes != "yes")) {
      env.say("Nothing changed on this computer; the list is kept (driftwire host users apply makes it so later).");
      return 0;
    }
  }
  std::string request = host_smb::request_json(r);
  const elevated_run ran = env.elevate(request);
  sodium_memzero(request.data(), request.size());
  if (!ran.started) {
    env.say("driftwire host: " + (ran.why.empty() ? std::string("the step did not run") : ran.why) +
            "; the list is kept (driftwire host users apply makes it so later).");
    return ran.declined ? 0 : 1;
  }
  const auto answer = host_smb::parse_answer(ran.answer);
  if (!answer) {
    env.say("driftwire host: the step's answer could not be read.");
    return 1;
  }
  if (!answer->why.empty()) env.say("driftwire host: " + answer->why);
  // The stamps: only users the step took, under the store's lock (the service writes it too).
  const auto held = store_lock(l);
  std::vector<su::user> now_list;
  if (held && su::load(store, now_list)) {
    const auto t = env.now();
    for (const auto& it : answer->users) {
      if (!it.ok) continue;
      for (auto& u : now_list) {
        if (u.name == it.name) u.applied = t;
      }
    }
    (void)su::save(store, now_list);
    // The computers' accounts: what the step made them, and the passwords their sessions get.
    std::vector<ds::desk> now_desks;
    if (ds::load(store, now_desks)) {
      for (const auto& it : answer->users) {
        if (!it.ok) continue;
        for (auto& d : now_desks) {
          if (d.account != it.name || d.gone) continue;
          for (const auto& p : desk_passwords) {
            if (p.user == d.account) d.password = p.password;
          }
          for (const auto& u : r.users) {
            if (u.name != d.account) continue;
            d.role = u.r == su::role::change ? "change" : "view";
          }
          for (const auto& u : users) {
            if (u.name == d.account) d.folders = u.folders;
          }
          d.applied = t;
        }
      }
      // Gone ones whose account the step took away (or never had): dropped.
      std::erase_if(now_desks, [&](const ds::desk& d) {
        if (!d.gone) return false;
        return std::find(r.remove.begin(), r.remove.end(), d.account) != r.remove.end() && answer->ok;
      });
      (void)ds::save(store, now_desks);
    }
  }
  share_state st;
  st.computer = answer->computer;
  st.mdns_host = answer->mdns_host;
  for (const auto& s : answer->shares) {
    const bool opened = std::any_of(r.users.begin(), r.users.end(), [&](const host_smb::user_want& u) {
      return std::find(u.shares.begin(), u.shares.end(), s.name) != u.shares.end();
    });
    if (s.ok && opened) st.shares.push_back(s.name);
  }
  (void)save_share_state(store, st);
  for (const auto& it : answer->users) env.say(std::string(it.ok ? "  done   " : "  failed ") + it.name + (it.ok ? "" : ": " + it.why));
  for (const auto& it : answer->shares) env.say(std::string(it.ok ? "  done   " : "  failed ") + it.name + (it.ok ? "" : ": " + it.why));
  for (const auto& line : users_status_lines(store)) env.say(line);
  return answer->ok ? 0 : 1;
}

}  // namespace

int run_users(const users_command& c, const host_layout& l, users_env& env) {
  if (!make_host_folders(l)) {
    env.say("driftwire host: its folder must be this user's alone.");
    return 1;
  }
  auto store = host_files::store(l.serve + "/secrets");
  if (!store) {
    env.say("driftwire host: the service's store cannot be opened.");
    return 1;
  }
  if (c.verb == users_verb::list) {
    std::vector<su::user> users;
    if (!su::load(*store, users)) {
      env.say("driftwire host: the list of users cannot be read.");
      return 1;
    }
    if (users.empty()) env.say("No users yet (driftwire host users add NAME).");
    for (const auto& u : users) env.say(user_line(u));
    return 0;
  }

  std::vector<host_smb::password_for> passwords;
  passwords.reserve(su::kMaxUsers);
  std::vector<std::string> removed;
  if (c.verb == users_verb::apply) {
    // The app's: NAME<TAB>PASSWORD lines for new users and changed passwords.
    if (c.input_stdin) {
      for (wiped line; passwords.size() < su::kMaxUsers && env.ask("", line.s, true);) {
        const auto tab = line.s.find('\t');
        if (tab == std::string::npos) continue;
        host_smb::password_for p;
        p.user = line.s.substr(0, tab);
        p.password = line.s.substr(tab + 1);
        sodium_memzero(line.s.data(), line.s.size());
        line.s.clear();
        if (!host_smb::acceptable_password(p.password)) {
          env.say("driftwire host: a password needs 8 to 256 characters; nothing changed.");
          return 2;
        }
        passwords.push_back(std::move(p));
      }
    } else if (env.terminal) {
      std::vector<su::user> users;
      if (su::load(*store, users)) {
        for (const auto& u : users) {
          if (u.applied != 0) continue;
          wiped pw;
          std::string why;
          if (!ask_password(env, c, u.name, pw, why)) {
            env.say("driftwire host: " + why);
            return 2;
          }
          passwords.push_back({u.name, pw.s});
        }
      }
    }
    return apply_now(l, *store, env, passwords, removed);
  }

  // A change: the admin passphrase first.
  {
    wiped pass;
    if (!env.ask("Admin passphrase (the owner's): ", pass.s, true)) {
      env.say("driftwire host: no admin passphrase given; nothing changed.");
      return 2;
    }
    std::int64_t until = 0;
    switch (check_admin(*store, pass.s, env.now(), until)) {
      case admin_answer::ok:
        break;
      case admin_answer::wrong:
        env.say("driftwire host: that is not the admin passphrase; nothing changed.");
        return 1;
      case admin_answer::locked:
        env.say("driftwire host: too many wrong passphrases; try again in " +
                std::to_string(std::max<std::int64_t>(1, (until - env.now() + 59) / 60)) + " min.");
        return 1;
      case admin_answer::none_set:
        env.say("driftwire host: no admin passphrase is set yet. Set one when you claim this computer in MediaViewer, "
                "or with driftwire serve passphrase set --data <the host's serve folder>.");
        return 1;
      case admin_answer::unreadable:
        env.say("driftwire host: the admin passphrase cannot be read; nothing changed.");
        return 1;
    }
  }

  wiped pw;
  if (c.verb == users_verb::add || c.verb == users_verb::password) {
    std::string why;
    if (!ask_password(env, c, c.name, pw, why)) {
      env.say("driftwire host: " + why);
      return 2;
    }
  }
  {
    const auto held = store_lock(l);
    if (!held) {
      env.say("driftwire host: the list is being changed by something else; try again.");
      return 1;
    }
    std::vector<su::user> users;
    if (!su::load(*store, users)) {
      env.say("driftwire host: the list of users cannot be read; nothing changed.");
      return 1;
    }
    const su::user* there = su::find(users, c.name);
    if (c.verb == users_verb::add && there) {
      env.say("driftwire host: " + c.name + " is a user already (host users role or password changes them).");
      return 1;
    }
    if (c.verb != users_verb::add && !there) {
      env.say("driftwire host: there is no user " + c.name + ".");
      return 1;
    }
    if (c.verb == users_verb::remove) {
      (void)su::remove(users, c.name);
      removed.push_back(c.name);
    } else if (c.verb == users_verb::add || c.verb == users_verb::role) {
      su::user u = there ? *there : su::user{};
      u.name = c.name;
      if (c.r) u.r = *c.r;
      if (c.folders) u.folders = *c.folders;
      const std::string why = su::upsert(users, std::move(u), env.now(), env.self);
      if (!why.empty()) {
        env.say(why == "too-many" ? "driftwire host: 32 users at most." : why == "bad-name" ? "driftwire host: that name cannot be used." : "driftwire host: those folders cannot be used.");
        return 1;
      }
    }
    if (!su::save(*store, users)) {
      env.say("driftwire host: the list could not be written; nothing changed.");
      return 1;
    }
  }
  if (!pw.s.empty()) passwords.push_back({c.name, pw.s});
  return apply_now(l, *store, env, passwords, removed);
}

namespace {

bool read_terminal_line(const std::string& prompt, std::string& out, bool hidden, bool terminal) {
  out.clear();
  if (terminal && !prompt.empty()) print(prompt);
  bool console = false, got = false;
  {
    const serve_echo_off quiet(hidden && terminal);
    console = quiet.active();
    for (int ch; (ch = std::fgetc(stdin)) != EOF;) {
      got = true;
      if (ch == '\n') break;
      if (out.size() < host_smb::kMaxPasswordBytes + owner::kMaxBytes + 1) out.push_back(static_cast<char>(ch));
    }
    if (!out.empty() && out.back() == '\r') out.pop_back();
  }
  if (console) print("\n");
  return got;
}

std::string os_user() {
#if defined(_WIN32)
  const char* u = std::getenv("USERNAME");
#else
  const char* u = std::getenv("USER");
#endif
  return u ? std::string(u) : std::string();
}

}  // namespace

int run_host_users(const args& a, const host_layout& l, bool test_hooks) {
  users_command c;
  if (const std::string why = parse_users_command(a, c); !why.empty()) {
    print("driftwire: " + why + "\n" + usage_line() + "\n", true);
    return 2;
  }
  const bool terminal = serve_stdin_is_terminal() && !c.input_stdin;
  if (c.verb != users_verb::list && c.verb != users_verb::apply && !terminal && !c.input_stdin) {
    print("driftwire host: no terminal to ask for the admin passphrase (the app gives --input-stdin).\n", true);
    return 2;
  }
  users_env env;
  env.terminal = terminal;
  env.any_port = test_hooks;
  env.self = os_user();
  env.now = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
  env.say = [](const std::string& line) { print(line + "\n"); };
  env.ask = [terminal](const std::string& prompt, std::string& out, bool hidden) {
    return read_terminal_line(prompt, out, hidden, terminal);
  };
  const std::string program = self_path();
  env.elevate = [program, terminal, test_hooks](std::string_view request) {
    if (test_hooks) {
      // The test build never elevates: it says what it would have done.
      elevated_run r;
      r.why = "the test build never asks for an administrator";
      return r;
    }
    return run_elevated(program, request, terminal);
  };
  return run_users(c, l, env);
}

int run_smb_apply(const args& a) {
  if (!serve_is_privileged()) {
    print("driftwire host: smb-apply is the elevated step that driftwire host users starts; it is not typed.\n", true);
    return 2;
  }
  host_smb::note_invoker();
  if (sodium_init() < 0) return 1;
  const std::string channel = a.host_words.empty() ? std::string() : a.host_words.front();
  std::string line;
  host_smb::answer out;
  if (!helper_read(channel, line)) {
    out.why = "the step was given nothing to do";
  } else {
    std::string why;
    auto r = host_smb::parse_request(line, why);
    sodium_memzero(line.data(), line.size());
    if (r) {
      // Never a system folder, a folder reached through a link, or one that is not
      // the person's: the request came from a process anyone logged in as them can run.
      for (const auto& s : r->shares) {
        if (std::string refused = host_smb::check_share_folder(s.path); !refused.empty()) {
          why = s.name + ": " + refused;
          r.reset();
          break;
        }
      }
    }
    if (!r) {
      out.why = why;
    } else {
      auto port = host_smb::os_port();
      out = host_smb::apply(*r, *port);
    }
  }
  return helper_write(channel, host_smb::answer_json(out)) && out.ok ? 0 : 1;
}

}  // namespace mv::driftwire::cli
