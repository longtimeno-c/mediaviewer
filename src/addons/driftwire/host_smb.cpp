// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/host_smb.h"

#include <sodium.h>

#include <algorithm>

#include "core/json.h"

namespace mv::driftwire::host_smb {

namespace su = serve::share_users;

namespace {

void wipe(std::string& s) {
  if (!s.empty()) sodium_memzero(s.data(), s.size());
  s.clear();
}

bool absolute_path(std::string_view p) {
  if (p.empty() || p.size() > 4096) return false;
  if (p.find('\0') != std::string_view::npos) return false;
  if (p.front() == '/') return true;
  return p.size() > 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
         (p[2] == '\\' || p[2] == '/');
}

bool has(const std::vector<std::string>& v, std::string_view s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::string folder_words(std::size_t n) { return n == 1 ? "1 folder" : std::to_string(n) + " folders"; }

}  // namespace

request::~request() {
  for (auto& u : users) wipe(u.password);
}

password_for::~password_for() { wipe(password); }

bool acceptable_password(std::string_view p) noexcept {
  if (p.size() < 8 || p.size() > kMaxPasswordBytes) return false;
  // Valid UTF-8 (Windows turns anything else into an empty password), no control characters.
  for (std::size_t i = 0; i < p.size();) {
    const auto u = static_cast<unsigned char>(p[i]);
    if (u < 0x20 || u == 0x7f) return false;
    std::size_t n = u < 0x80 ? 1 : (u >> 5) == 0x6 ? 2 : (u >> 4) == 0xE ? 3 : (u >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > p.size() || (n == 2 && u < 0xC2) || (n == 4 && u > 0xF4)) return false;
    for (std::size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(p[i + k]) & 0xC0) != 0x80) return false;
    }
    if (n == 3) {
      const auto v = static_cast<unsigned char>(p[i + 1]);
      if ((u == 0xE0 && v < 0xA0) || (u == 0xED && v >= 0xA0)) return false;  // overlong, surrogates
    }
    if (n == 4) {
      const auto v = static_cast<unsigned char>(p[i + 1]);
      if ((u == 0xF0 && v < 0x90) || (u == 0xF4 && v >= 0x90)) return false;
    }
    i += n;
  }
  return true;
}

std::string refuse_system_folder(std::string_view path, bool windows) {
  std::string p(path);
  if (windows) {
    for (auto& c : p) {
      if (c == '/') c = '\\';
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
  }
  const char sep = windows ? '\\' : '/';
  while (p.size() > (windows ? 3u : 1u) && p.back() == sep) p.pop_back();
  const auto under = [&](std::string_view top) {
    return p == top || (p.size() > top.size() && p.compare(0, top.size(), top) == 0 && p[top.size()] == sep);
  };
  if (p.find(std::string(1, sep) + "..") != std::string::npos || p.find(std::string(1, sep) + "." + sep) != std::string::npos) {
    return "that folder is not a plain path";
  }
  if (windows) {
    // The system's drive: its root and its own trees (the drive letter is the one the system has; C: here).
    if (p.size() <= 3) return p.rfind("c:", 0) == 0 ? "the system's drive is never shared whole" : std::string();
    for (const char* top : {"c:\\windows", "c:\\program files", "c:\\program files (x86)", "c:\\programdata",
                            "c:\\$recycle.bin", "c:\\system volume information", "c:\\recovery"}) {
      if (under(top)) return "a folder of the system's own is never shared";
    }
    if (p == "c:\\users" || p == "c:\\users\\public" || p == "c:\\users\\default") return "the folder of everyone's accounts is never shared";
    return {};
  }
  if (p == "/") return "the system's root is never shared";
  for (const char* top : {"/System", "/Library", "/private", "/usr", "/bin", "/sbin", "/etc", "/var", "/tmp", "/cores",
                          "/dev", "/opt", "/Applications", "/boot", "/proc", "/sys", "/root", "/lib", "/lib64", "/srv", "/run"}) {
    if (under(top)) return "a folder of the system's own is never shared";
  }
  if (p == "/Users" || p == "/home" || p == "/Volumes" || p == "/Users/Shared" || p == "/mnt" || p == "/media") {
    return "the folder of everyone's accounts or drives is never shared whole";
  }
  return {};
}

std::string request_json(const request& r) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("shares").begin_array();
  for (const auto& s : r.shares) w.begin_object().key("name").string(s.name).key("path").string(s.path).end_object();
  w.end_array().key("users").begin_array();
  for (const auto& u : r.users) {
    w.begin_object().key("name").string(u.name).key("role").string(su::role_name(u.r)).key("shares").begin_array();
    for (const auto& s : u.shares) w.string(s);
    w.end_array();
    if (!u.password.empty()) w.key("password").string(u.password);
    w.end_object();
  }
  w.end_array().key("remove").begin_array();
  for (const auto& n : r.remove) w.string(n);
  w.end_array().end_object();
  return w.take();
}

std::optional<request> parse_request(std::string_view text, std::string& why) {
  why.clear();
  if (text.size() > kMaxRequestBytes) {
    why = "the request is too long";
    return std::nullopt;
  }
  auto doc = json::parse(text, 5);
  const auto fail = [&](const char* w) {
    why = w;
    // The parsed copy holds the passwords too.
    if (doc) {
      if (json::value* users = const_cast<json::value*>(doc->find("users")); users && users->k == json::kind::array) {
        for (auto& e : users->a) {
          if (json::value* p = const_cast<json::value*>(e.find("password")); p && p->k == json::kind::string) wipe(p->s);
        }
      }
    }
    return std::optional<request>{};
  };
  if (!doc || doc->integer("v") != 1) return fail("the request is not one this program reads");
  const json::value* shares = doc->find("shares");
  const json::value* users = doc->find("users");
  const json::value* remove = doc->find("remove");
  if (!shares || shares->k != json::kind::array || !users || users->k != json::kind::array || !remove ||
      remove->k != json::kind::array || shares->a.size() > su::kMaxFolders || users->a.size() > su::kMaxUsers ||
      remove->a.size() > su::kMaxUsers) {
    return fail("the request is not one this program reads");
  }
  request r;
  r.users.reserve(users->a.size());  // never reallocated: no unwiped copy of a password left behind
  for (const auto& e : shares->a) {
    const std::string* name = e.str("name");
    const std::string* path = e.str("path");
    if (!name || !path || su::share_name(*name) != *name || !absolute_path(*path)) return fail("a share's name or folder is not right");
    if (std::any_of(r.shares.begin(), r.shares.end(), [&](const share_want& s) { return s.name == *name; })) {
      return fail("a share is named twice");
    }
    r.shares.push_back({*name, *path});
  }
  std::vector<std::string> seen;
  for (const auto& e : users->a) {
    const std::string* name = e.str("name");
    const std::string* rn = e.str("role");
    const json::value* us = e.find("shares");
    const auto ro = rn ? su::parse_role(*rn) : std::nullopt;
    if (!name || !su::valid_name(*name) || !ro || !us || us->k != json::kind::array || has(seen, *name)) {
      return fail("a user's name or role is not right");
    }
    user_want u;
    u.name = *name;
    u.r = *ro;
    for (const auto& s : us->a) {
      if (s.k != json::kind::string ||
          std::none_of(r.shares.begin(), r.shares.end(), [&](const share_want& x) { return x.name == s.s; })) {
        return fail("a user's share is not in the request");
      }
      if (!has(u.shares, s.s)) u.shares.push_back(s.s);
    }
    if (const json::value* p = e.find("password")) {
      if (p->k != json::kind::string || !acceptable_password(p->s)) return fail("a password is not one the step takes");
      u.password = p->s;
    }
    seen.push_back(*name);
    r.users.push_back(std::move(u));
  }
  for (const auto& e : remove->a) {
    if (e.k != json::kind::string || !su::valid_name(e.s) || has(seen, e.s) || has(r.remove, e.s)) {
      return fail("a user to remove is not right");
    }
    r.remove.push_back(e.s);
  }
  (void)fail("");  // wipes the parsed copy's passwords
  why.clear();
  return r;
}

std::string answer_json(const answer& a) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("ok").boolean(a.ok).key("why").string(a.why);
  w.key("computer").string(a.computer).key("mdns_host").string(a.mdns_host);
  const auto items = [&](const char* k, const std::vector<item>& list) {
    w.key(k).begin_array();
    for (const auto& i : list) w.begin_object().key("name").string(i.name).key("ok").boolean(i.ok).key("why").string(i.why).end_object();
    w.end_array();
  };
  items("users", a.users);
  items("shares", a.shares);
  w.end_object();
  return w.take();
}

std::optional<answer> parse_answer(std::string_view text) {
  if (text.size() > kMaxRequestBytes) return std::nullopt;
  const auto doc = json::parse(text, 4);
  if (!doc || doc->integer("v") != 1) return std::nullopt;
  answer a;
  const auto ok = doc->boolean("ok");
  const std::string* why = doc->str("why");
  const std::string* computer = doc->str("computer");
  const std::string* mdns = doc->str("mdns_host");
  if (!ok || !why || !computer || !mdns) return std::nullopt;
  a.ok = *ok;
  a.why = *why;
  a.computer = *computer;
  a.mdns_host = *mdns;
  const auto items = [&](const char* k, std::vector<item>& out) {
    const json::value* list = doc->find(k);
    if (!list || list->k != json::kind::array) return false;
    for (const auto& e : list->a) {
      const std::string* n = e.str("name");
      const auto o = e.boolean("ok");
      const std::string* y = e.str("why");
      if (!n || !o || !y) return false;
      out.push_back({*n, *o, *y});
    }
    return true;
  };
  if (!items("users", a.users) || !items("shares", a.shares)) return std::nullopt;
  return a;
}

answer apply(const request& r, port& p) {
  answer out;
  const facts f = p.probe();
  out.computer = f.computer;
  out.mdns_host = f.mdns_host;
  if (!f.supported) {
    out.why = f.why_not.empty() ? "this computer cannot share folders this way yet" : f.why_not;
    return out;
  }
  if (f.smb1_on) {
    out.why = "the old SMB1 file sharing is turned on on this computer; turn it off first (MediaViewer never "
              "shares over SMB1)";
    return out;
  }
  bool all = true;
  // Taken away: those asked, and any account of ours the request does not list (a
  // removal whose step was declined, or made where the step was never told).
  std::vector<std::string> removing = r.remove;
  for (const auto& n : p.our_accounts()) {
    const bool listed = std::any_of(r.users.begin(), r.users.end(), [&](const user_want& u) { return u.name == n; });
    if (!listed && !has(removing, n) && serve::share_users::valid_name(n)) removing.push_back(n);
  }

  // Accounts first: a share lists only users whose account is there and ours.
  std::vector<std::string> ready;
  for (const auto& u : r.users) {
    item it{u.name, false, {}};
    switch (p.account(u.name)) {
      case port::owner::none:
        if (u.password.empty()) {
          it.why = "needs a password";
        } else {
          it.why = p.make_account(u.name, u.password);
        }
        break;
      case port::owner::ours:
        if (!u.password.empty()) it.why = p.set_password(u.name, u.password);
        break;
      case port::owner::theirs:
        it.why = "an account of that name is already on this computer and is not MediaViewer's; it was left alone";
        break;
    }
    it.ok = it.why.empty();
    if (it.ok) ready.push_back(u.name);
    all = all && it.ok;
    out.users.push_back(std::move(it));
  }

  // Whose earlier entries on a folder go before the new ones are written: MediaViewer's
  // accounts only (made or kept above, or about to be taken away), never anyone else's.
  std::vector<std::string> ours;
  for (const auto& u : r.users) {
    if (p.account(u.name) == port::owner::ours) ours.push_back(u.name);
  }
  for (const auto& n : removing) {
    if (p.account(n) == port::owner::ours && !has(ours, n)) ours.push_back(n);
  }

  bool made_any = false;
  std::vector<std::string> wanted;
  for (const auto& s : r.shares) {
    std::vector<member> members;
    for (const auto& u : r.users) {
      if (has(ready, u.name) && has(u.shares, s.name)) members.push_back({u.name, u.r});
    }
    item it{s.name, false, {}};
    const auto there = p.share(s.name);
    if (there == port::owner::theirs) {
      it.why = "a share of that name is already on this computer and is not MediaViewer's; it was left alone";
    } else if (members.empty()) {
      if (there == port::owner::ours) it.why = p.remove_share(s.name, ours);
    } else {
      it.why = p.put_share(s, members, ours);
      made_any = made_any || it.why.empty();
      wanted.push_back(s.name);
    }
    it.ok = it.why.empty();
    all = all && it.ok;
    out.shares.push_back(std::move(it));
  }
  // Shares of ours the request no longer names (a folder unshared, its last user gone).
  for (const auto& name : p.our_shares()) {
    if (has(wanted, name)) continue;
    if (std::any_of(r.shares.begin(), r.shares.end(), [&](const share_want& s) { return s.name == name; })) continue;
    item it{name, false, p.remove_share(name, ours)};
    it.ok = it.why.empty();
    all = all && it.ok;
    out.shares.push_back(std::move(it));
  }

  for (const auto& n : removing) {
    item it{n, false, {}};
    switch (p.account(n)) {
      case port::owner::none:
        break;
      case port::owner::ours:
        it.why = p.delete_account(n);
        break;
      case port::owner::theirs:
        it.why = "that account is not MediaViewer's; it was left alone";
        break;
    }
    it.ok = it.why.empty();
    all = all && it.ok;
    out.users.push_back(std::move(it));
  }

  if (made_any && !f.service_on) {
    if (std::string why = p.start_service(); !why.empty()) {
      out.why = why;
      all = false;
    }
  }
  out.ok = all;
  return out;
}

request plan(const std::vector<su::user>& users, const std::vector<folder>& folders, std::vector<password_for>& passwords,
             const std::vector<std::string>& removed) {
  request r;
  r.users.reserve(users.size());
  for (const auto& f : folders) {
    const std::string name = su::share_name(f.label);
    if (std::any_of(r.shares.begin(), r.shares.end(), [&](const share_want& s) { return s.name == name; })) continue;
    // A folder no one may open is still named: the step removes its share if it made one.
    r.shares.push_back({name, f.path});
  }
  for (const auto& u : users) {
    user_want w;
    w.name = u.name;
    w.r = u.r;
    for (const auto& f : folders) {
      if (!u.sees(f.label)) continue;
      const std::string name = su::share_name(f.label);
      if (!has(w.shares, name)) w.shares.push_back(name);
    }
    for (auto& p : passwords) {
      if (p.user == u.name) {
        w.password.assign(p.password);  // copied, then the source wiped (a move leaves short ones behind)
        wipe(p.password);
      }
    }
    r.users.push_back(std::move(w));
  }
  for (const auto& n : removed) {
    if (!su::find(users, n) && !has(r.remove, n)) r.remove.push_back(n);
  }
  return r;
}

std::vector<std::string> explain(const request& r, const facts& f) {
  std::vector<std::string> out;
  for (const auto& u : r.users) {
    const std::string what = u.password.empty() ? "Keep" : "Make or update";
    out.push_back(what + " the sharing-only account " + u.name + " (" + su::role_words(u.r) + ", " +
                  folder_words(u.shares.size()) + ").");
  }
  for (const auto& n : r.remove) out.push_back("Remove the account " + n + " if MediaViewer made it.");
  std::size_t opened = 0;
  for (const auto& s : r.shares) {
    if (std::any_of(r.users.begin(), r.users.end(), [&](const user_want& u) { return has(u.shares, s.name); })) ++opened;
  }
  if (opened > 0) {
    out.push_back("Share " + folder_words(opened) +
                  " on this network only: encrypted (SMB3), no guests, never SMB1, never opened on the router.");
  }
  if (opened > 0 && !f.service_on) out.push_back("Turn on file sharing on this computer (it is off).");
  out.push_back("Nothing in the folders is changed; only who may open them.");
  return out;
}

}  // namespace mv::driftwire::host_smb
