// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The card's rows and words follow PhoneAccessView.swift as it stood on
// 2026-11-14 (its Windows twin had the same words, Shared/PhoneWords.cs). Two
// things a view tree has no control for are drawn another way: a confirmation
// dialog is a row that asks again in place, and a menu is a choice.
#include "addons/phone/screens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>

#include "core/json.h"

namespace mv::phone::card {

namespace {

using json::writer;

constexpr std::uint32_t kClick = 1, kChange = 2, kSubmit = 3;  // MV_SLOT_EVENT_*

struct phone_row {
  std::string name, key;
  bool limited = false, expired = false, guest = false, backup = true;
  std::int64_t expires = 0;
};
struct folder_row {
  std::string id, label, path;
  bool backup = false, indexed = false;
  std::int64_t limit = 0, reserve = 0, used = 0, room = 0;
  [[nodiscard]] bool catalogue() const { return label == "Catalogue"; }
};
struct host_row {
  bool installed = false, set_up = false, running = false, at_login = false;
  std::int64_t service_port = 0, offline = 0;
  std::string name, duckdns;
  std::vector<std::string> keepers;
};
struct catalogue_row {
  bool running = false;
  std::string phase, error;
  double fraction = 0;
  std::int64_t published = 0, assets = 0;
};

// The link's state (link::state_json), as the cards read it.
struct view {
  bool starting = false, secondary = false, driftwire = true;
  std::string busy, error, folder, library_name;
  bool local_search = false;
  std::optional<catalogue_row> catalogue;
  bool claimed = false, reachable = true;
  std::string local, public_name, warning;
  std::vector<phone_row> phones;
  std::vector<folder_row> folders;
  std::string qr, qr_library, qr_fingerprint;
  std::int64_t qr_expires = 0, qr_for_s = 0;
  std::vector<std::string> added;
  bool other_publisher = false, mismatch = false;
  std::string other_fingerprint, asking_name, asking_key, asking_fingerprint;
  std::string nas_passphrase;
  std::vector<std::string> nas_features;
  std::int64_t unlocked_s = 0, locked_until = 0;
  std::string relay;
  std::optional<host_row> host;
  std::string host_step, host_said;
  [[nodiscard]] bool has(std::string_view f) const {
    return std::find(nas_features.begin(), nas_features.end(), f) != nas_features.end();
  }
};

std::string s_of(const json::value& o, std::string_view k) {
  const std::string* s = o.str(k);
  return s ? *s : std::string();
}
std::int64_t i_of(const json::value& o, std::string_view k) {
  const json::value* v = o.find(k);
  if (!v || v->k != json::kind::number) return 0;
  return v->is_integer ? v->i : static_cast<std::int64_t>(v->d);
}
double d_of(const json::value& o, std::string_view k) {
  const json::value* v = o.find(k);
  if (!v || v->k != json::kind::number) return 0;
  return v->is_integer ? static_cast<double>(v->i) : v->d;
}
bool b_of(const json::value& o, std::string_view k, bool fallback = false) { return o.boolean(k).value_or(fallback); }
std::vector<std::string> strings_of(const json::value& o, std::string_view k) {
  std::vector<std::string> out;
  if (const json::value* v = o.find(k); v && v->k == json::kind::array) {
    for (const json::value& e : v->a) {
      if (e.k == json::kind::string) out.push_back(e.s);
    }
  }
  return out;
}

view parse(std::string_view text) {
  view s;
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) {
    s.starting = true;
    return s;
  }
  const json::value& o = *doc;
  s.starting = b_of(o, "starting");
  s.secondary = b_of(o, "secondary");
  s.driftwire = b_of(o, "driftwire", true);
  s.busy = s_of(o, "busy");
  s.error = s_of(o, "error");
  s.folder = s_of(o, "folder");
  s.library_name = s_of(o, "library_name");
  s.local_search = b_of(o, "local_search");
  if (const json::value* c = o.find("catalogue"); c && c->k == json::kind::object) {
    catalogue_row r;
    r.running = b_of(*c, "running");
    r.phase = s_of(*c, "phase");
    r.fraction = d_of(*c, "fraction");
    r.error = s_of(*c, "error");
    r.published = i_of(*c, "published");
    r.assets = i_of(*c, "assets");
    s.catalogue = r;
  }
  s.claimed = b_of(o, "claimed");
  s.reachable = b_of(o, "reachable", true);
  s.local = s_of(o, "local");
  s.public_name = s_of(o, "public");
  s.warning = s_of(o, "warning");
  if (const json::value* v = o.find("devices"); v && v->k == json::kind::array) {
    for (const json::value& d : v->a) {
      if (!d.str("name") || !d.str("key")) continue;
      phone_row p;
      p.name = *d.str("name");
      p.key = *d.str("key");
      p.guest = b_of(d, "guest");
      p.limited = b_of(d, "limited");
      p.expires = i_of(d, "expires");
      p.expired = b_of(d, "expired");
      p.backup = b_of(d, "backup", !p.guest);
      s.phones.push_back(std::move(p));
    }
  }
  if (const json::value* v = o.find("folders"); v && v->k == json::kind::array) {
    for (const json::value& d : v->a) {
      if (!d.str("id") || !d.str("label") || !d.str("path")) continue;
      folder_row f;
      f.id = *d.str("id");
      f.label = *d.str("label");
      f.path = *d.str("path");
      f.backup = b_of(d, "backup");
      f.limit = i_of(d, "limit");
      f.reserve = i_of(d, "reserve");
      f.used = i_of(d, "used");
      f.room = i_of(d, "room");
      f.indexed = b_of(d, "indexed");
      s.folders.push_back(std::move(f));
    }
  }
  s.qr = s_of(o, "qr");
  s.qr_library = s_of(o, "qr_library");
  s.qr_fingerprint = s_of(o, "qr_fingerprint");
  s.qr_expires = i_of(o, "qr_expires");
  s.qr_for_s = i_of(o, "qr_for_s");
  s.added = strings_of(o, "added");
  s.other_publisher = b_of(o, "other_publisher");
  s.mismatch = b_of(o, "mismatch");
  s.other_fingerprint = s_of(o, "other_fingerprint");
  s.asking_name = s_of(o, "asking_name");
  s.asking_key = s_of(o, "asking_key");
  s.asking_fingerprint = s_of(o, "asking_fingerprint");
  s.nas_passphrase = s_of(o, "nas_passphrase");
  s.nas_features = strings_of(o, "nas_features");
  s.unlocked_s = i_of(o, "unlocked_s");
  s.locked_until = i_of(o, "locked_until");
  s.relay = s_of(o, "relay");
  if (const json::value* h = o.find("host"); h && h->k == json::kind::object) {
    host_row r;
    r.installed = b_of(*h, "installed");
    r.set_up = b_of(*h, "set_up");
    r.running = b_of(*h, "running");
    r.at_login = b_of(*h, "at_login");
    r.service_port = i_of(*h, "service_port");
    r.offline = i_of(*h, "offline");
    r.name = s_of(*h, "name");
    r.duckdns = s_of(*h, "duckdns");
    r.keepers = strings_of(*h, "keepers");
    s.host = r;
  }
  s.host_step = s_of(o, "host_step");
  s.host_said = s_of(o, "host_said");
  return s;
}

std::string trim(std::string_view t) {
  std::size_t a = 0, b = t.size();
  while (a < b && (t[a] == ' ' || t[a] == '\t' || t[a] == '\n' || t[a] == '\r')) ++a;
  while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '\n' || t[b - 1] == '\r')) --b;
  return std::string(t.substr(a, b - a));
}

std::string clock_text(std::int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%H:%M", &tm);
  return buf;
}

std::string weekday_clock(std::int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[48];
  std::strftime(buf, sizeof buf, "%a %H:%M", &tm);
  return buf;
}

std::string with_commas(std::int64_t n) {
  std::string digits = std::to_string(n < 0 ? -n : n), out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return n < 0 ? "-" + out : out;
}

// ---- the tree's pieces --------------------------------------------------------------------

void text(writer& w, const std::string& id, const std::string& s, const char* style = "body") {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).end_object();
}
void copyable(writer& w, const std::string& id, const std::string& s, const char* style, const std::string& label) {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).key("copyable").boolean(true).key("label").string(label).end_object();
}
void button(writer& w, const std::string& id, const std::string& label, bool disabled = false,
            const char* style = "plain", const std::string& name = {}) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("style").string(style);
  if (!name.empty()) w.key("help").string(name);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}
void field(writer& w, const std::string& id, const std::string& label, const std::string& value,
           const std::string& placeholder, bool disabled = false) {
  w.begin_object().key("type").string("text_field").key("id").string(id).key("label").string(label);
  w.key("value").string(value).key("placeholder").string(placeholder);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}
// Never a value: what is typed goes to the add-on and nowhere else.
void secure(writer& w, const std::string& id, const std::string& label, const std::string& placeholder = {}) {
  w.begin_object().key("type").string("text_field").key("id").string(id).key("label").string(label);
  w.key("value").string("").key("secure").boolean(true);
  if (!placeholder.empty()) w.key("placeholder").string(placeholder);
  w.end_object();
}
void pick_folder(writer& w, const std::string& id, const std::string& label, const std::string& value,
                 const std::string& placeholder, bool disabled) {
  w.begin_object().key("type").string("text_field").key("id").string(id).key("label").string(label);
  w.key("value").string(value).key("placeholder").string(placeholder).key("pick").string("folder");
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}
void toggle(writer& w, const std::string& id, const std::string& label, bool value, bool disabled = false) {
  w.begin_object().key("type").string("toggle").key("id").string(id).key("label").string(label);
  w.key("value").boolean(value);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}
struct option {
  std::string id, text;
};
void choice(writer& w, const std::string& id, const std::string& label, const std::vector<option>& options,
            const std::string& value, bool disabled = false) {
  w.begin_object().key("type").string("choice").key("id").string(id).key("label").string(label);
  w.key("options").begin_array();
  for (const option& o : options) w.begin_object().key("id").string(o.id).key("text").string(o.text).end_object();
  w.end_array().key("value").string(value);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}
void progress(writer& w, const std::string& id, const std::string& label, double value) {
  w.begin_object().key("type").string("progress").key("id").string(id).key("label").string(label);
  w.key("value").number(value).key("detail").string(label).end_object();
}
void stack(writer& w, const std::string& id, int spacing, const char* axis = "vertical", const char* look = nullptr) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string(axis);
  w.key("spacing").integer(spacing).key("align").string(axis[0] == 'h' ? "center" : "fill");
  if (look) w.key("look").string(look);
  w.key("children").begin_array();
}
void end(writer& w) { w.end_array().end_object(); }

// A Settings row: the title and its line, then the row's controls (call end()).
void row(writer& w, const std::string& id, const std::string& title, const std::string& detail) {
  stack(w, id, 4, "vertical");
  text(w, id + "_t", title, "body");
  if (!detail.empty()) text(w, id + "_d", detail, "secondary");
  stack(w, id + "_c", 8, "horizontal");
}
void end_row(writer& w) {
  end(w);  // controls
  end(w);  // row
}

std::string short_key(const std::string& key) { return key.substr(0, std::min<std::size_t>(8, key.size())); }

const std::vector<std::pair<const char*, std::int64_t>>& durations() {
  static const std::vector<std::pair<const char*, std::int64_t>> d{
      {"1 hour", 3600}, {"24 hours", 86400}, {"7 days", 7 * 86400}, {"30 days", 30 * 86400}};
  return d;
}
constexpr const char* kUnlimited = "Until I remove it";
constexpr std::size_t kRelayKeyMinimum = 16;  // as driftwire-relay takes it

}  // namespace

// ---- words ---------------------------------------------------------------------------------

std::string screen::share_label(std::string_view path) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  if (parts.size() >= 2 && parts[0].rfind("volume", 0) == 0 && parts[0].size() > 6 &&
      parts[0].find_first_not_of("0123456789", 6) == std::string::npos) {
    return parts[1];  // a Synology's share is the folder right under /volumeN
  }
  return parts.empty() ? std::string() : parts.back();
}

std::string screen::gigabytes(std::int64_t bytes) {
  const double gb = static_cast<double>(bytes) / 1e9;
  if (gb >= 10) return std::to_string(static_cast<long long>(std::llround(gb)));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", gb);
  std::string t = buf;
  while (!t.empty() && t.back() == '0') t.pop_back();
  if (!t.empty() && t.back() == '.') t.pop_back();
  return t.empty() ? std::string("0") : t;
}

std::int64_t screen::bytes(std::string_view gigabytes_text) {
  std::string t = trim(gigabytes_text);
  std::replace(t.begin(), t.end(), ',', '.');
  if (t.empty() || t.find_first_not_of("0123456789.") != std::string::npos ||
      std::count(t.begin(), t.end(), '.') > 1 || t == ".") {
    return -1;
  }
  const double v = std::strtod(t.c_str(), nullptr);
  if (!(v >= 0) || v >= 1e6) return -1;
  return static_cast<std::int64_t>(std::llround(v * 1e9));
}

std::string screen::duration_text(std::int64_t seconds) {
  if (seconds == 3600) return "1 hour";
  if (seconds == 86400) return "24 hours";
  if (seconds > 0 && seconds % 86400 == 0) return std::to_string(seconds / 86400) + " days";
  if (seconds > 0 && seconds % 3600 == 0) return std::to_string(seconds / 3600) + " hours";
  return std::to_string(std::max<std::int64_t>(1, seconds / 60)) + " minutes";
}

std::string screen::relative(std::int64_t then, std::int64_t now) {
  const std::int64_t d = then - now, a = d < 0 ? -d : d;
  if (a < 60) return d < 0 ? "just now" : "in a moment";
  std::string amount;
  if (a < 3600) {
    const std::int64_t m = a / 60;
    amount = std::to_string(m) + (m == 1 ? " minute" : " minutes");
  } else if (a < 86400) {
    const std::int64_t h = a / 3600;
    amount = std::to_string(h) + (h == 1 ? " hour" : " hours");
  } else {
    const std::int64_t days = a / 86400;
    if (days == 1) return d < 0 ? "yesterday" : "tomorrow";
    amount = std::to_string(days) + " days";
  }
  return d < 0 ? amount + " ago" : "in " + amount;
}

const char* screen::here() const noexcept {
  switch (computer_) {
    case computer::mac: return "this Mac";
    case computer::pc: return "this PC";
    default: return "this computer";
  }
}
const char* screen::Here() const noexcept {
  switch (computer_) {
    case computer::mac: return "This Mac";
    case computer::pc: return "This PC";
    default: return "This computer";
  }
}

// ---- the tree -------------------------------------------------------------------------------

std::string screen::build(std::string_view state_json, std::int64_t now) {
  const view s = parse(state_json);
  const std::string me = here(), Me = Here();
  const bool busy = !s.busy.empty();
  busy_ = busy;
  claimed_ = s.claimed;
  qr_ = s.qr;
  asking_key_ = s.asking_key;
  relay_now_ = s.relay;
  // A refused action asked for the passphrase: the field stays until an unlock lands.
  if (s.error == "needs_unlock" && last_error_ != "needs_unlock") unlock_wanted_ = true;
  last_error_ = s.error;
  if (s.unlocked_s > 0 && unlock_wanted_) {
    unlock_wanted_ = false;
    unlock_with_.clear();
  }
  // The addresses start as the link's, once; then they are what was typed.
  if (!seeded_local_ && !s.local.empty()) {
    if (local_.empty()) local_ = s.local;
    seeded_local_ = true;
  }
  if (!seeded_public_ && !s.public_name.empty()) {
    if (public_.empty()) public_ = s.public_name;
    seeded_public_ = true;
  }
  phone_keys_.clear();
  phone_ends_.clear();
  phone_limited_.clear();
  phone_backup_.clear();
  for (const phone_row& p : s.phones) {
    phone_keys_.push_back(p.key);
    phone_ends_.push_back(p.limited ? p.expires : 0);
    phone_limited_.push_back(p.limited);
    phone_backup_.push_back(p.backup);
  }
  folder_ids_.clear();
  folder_backup_.clear();
  folder_limit_.clear();
  folder_reserve_.clear();
  for (const folder_row& f : s.folders) {
    folder_ids_.push_back(f.id);
    folder_backup_.push_back(f.backup);
    folder_limit_.push_back(f.limit);
    folder_reserve_.push_back(f.reserve);
  }

  // A secure field's id carries a generation: when what was typed is sent or
  // forgotten here, the host draws a new, empty field instead of keeping dots
  // that no longer stand for anything.
  const auto sid = [this](const char* base) { return std::string(base) + "." + std::to_string(secret_gen_); };

  writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("axis").string("vertical");
  w.key("spacing").integer(12).key("align").string("fill").key("children").begin_array();

  if (s.secondary) {
    text(w, "note", "Set up in the first MediaViewer window.", "secondary");
    end(w);
    return w.take();
  }
  if (s.starting) {
    text(w, "note", "Starting\xE2\x80\xA6", "secondary");
    end(w);
    return w.take();
  }
  if (!s.driftwire) {
    text(w, "note", "This copy of MediaViewer can't keep phone keys.", "secondary");
    end(w);
    return w.take();
  }

  // What is under way, as one line.
  std::string busy_text;
  if (s.busy == "claim") busy_text = "Claiming the service";
  else if (s.busy == "pair") busy_text = "Asking your server for a code";
  else if (s.busy == "refresh") busy_text = "Checking your server";
  else if (s.busy == "revoke") busy_text = "Removing the phone";
  else if (s.busy == "add_folder" || s.busy == "remove_folder") busy_text = "Changing the shared folders";
  else if (s.busy == "set_addresses") busy_text = "Checking the address";
  else if (s.busy == "folder") busy_text = "Setting up the folder";
  else if (s.busy == "takeover_ask" || s.busy == "takeover_allow" || s.busy == "takeover_restart")
    busy_text = "Updating the library";
  else if (s.busy == "unlock") busy_text = "Unlocking";
  else if (s.busy == "set_passphrase") busy_text = "Setting the passphrase";
  else if (s.busy == "grant") busy_text = "Changing the phone's access";
  else if (s.busy == "host_start") busy_text = "Starting the server";
  else if (s.busy == "host_setup") {
    if (s.host_step == "share") busy_text = "Sharing the folders";
    else if (s.host_step == "name") busy_text = "Naming it on DuckDNS";
    else if (s.host_step == "start") busy_text = "Starting the server";
    else if (s.host_step == "claim") busy_text = "Claiming it";
    else busy_text = "Setting up " + me;
  }
  if (!busy_text.empty()) progress(w, "busy", busy_text, -1);

  // This computer is the server: its state, and Start when it isn't running.
  if (s.host && s.host->set_up) {
    const host_row& h = *s.host;
    std::string line;
    if (!h.running) {
      line = h.at_login ? "Not running. It starts when you log in; Start runs it now."
                        : "Not running, and not set to start at login. Start runs it and keeps it starting at login.";
    } else {
      line = "Running";
      line += h.service_port > 0 ? " \xC2\xB7 port " + std::to_string(h.service_port) : " \xC2\xB7 not listening yet";
      if (!h.duckdns.empty()) line += " \xC2\xB7 " + h.duckdns + ".duckdns.org";
      if (h.offline > 0) {
        line += h.offline == 1 ? std::string(" \xC2\xB7 1 drive not connected")
                               : " \xC2\xB7 " + std::to_string(h.offline) + " drives not connected";
      }
      for (const std::string& k : h.keepers) line += "\n" + k;
    }
    row(w, "host", h.name.empty() ? Me + " is your server" : Me + " is your server \xC2\xB7 " + h.name, line);
    if (!h.running) button(w, "host_start", "Start", busy, "plain", "Start the server on " + me);
    end_row(w);
  }

  const auto search_rows = [&] {
    std::string detail;
    if (!s.local_search || !s.catalogue) {
      detail = "Your phone searches file names and tags. Install Local search and index this library for words, faces, places and sounds.";
    } else if (s.catalogue->running) {
      detail = s.catalogue->phase == "sending" ? "Sending to your server" : "Building the catalogue";
    } else {
      const std::string& e = s.catalogue->error;
      if (e == "unreachable") detail = "Couldn't reach that folder. It will need to be mounted.";
      else if (e == "other_library") detail = "That folder holds another library's catalogue.";
      else if (e == "no_space") detail = "Not enough space on the share.";
      else if (e == "no_index") detail = "Nothing is indexed yet. Your phone searches file names and tags until it is.";
      else if (e == "no_folders") detail = "None of your search folders are on that server.";
      else if (e == "no_key_store") detail = "This copy of MediaViewer can't keep a signing key, so it can't publish.";
      else if (e == "failed") detail = "The catalogue could not be built.";
      else if (e == "cancelled") detail = "Stopped.";
      else if (s.catalogue->published <= 0) detail = "Smart search starts once the catalogue is published.";
      else
        detail = "Smart search is on. Published " + relative(s.catalogue->published, now) + " \xC2\xB7 " +
                 with_commas(s.catalogue->assets) + " files";
    }
    row(w, "search", "Search on your phone", detail);
    if (!s.local_search) {
      text(w, "search_get", "Local search is in Settings \xE2\x86\x92 Add-ons.", "caption");
    } else if (!(s.catalogue && s.catalogue->running)) {
      button(w, "publish", "Publish now", busy);
    }
    end_row(w);
    if (s.catalogue && s.catalogue->running) {
      stack(w, "publishing", 8, "horizontal");
      progress(w, "publish_progress", s.catalogue->phase == "sending" ? "Sending to your server" : "Building the catalogue",
               std::clamp(s.catalogue->fraction, 0.0, 1.0));
      button(w, "publish_cancel", "Cancel", false, "plain", "Stop publishing");
      end(w);
    }
  };

  const auto public_rows = [&](bool claimed_now) {
    field(w, "public", "Address from outside", public_, "photos.example.com:7878", busy && claimed_now);
    std::string warn;
    if (s.warning == "private") warn = "This name points to a private address, so your phone can't reach it from outside.";
    else if (s.warning == "carrier")
      warn = "This name points to a carrier-grade address, so a forwarded port won't work. Ask your provider for a public address.";
    else if (s.warning == "unresolved") warn = "This name doesn't resolve yet.";
    if (!warn.empty()) text(w, "warning", warn, "secondary");
  };

  const auto claim_rows = [&] {
    stack(w, "claim_box", 8);
    row(w, "claim", "Service on your server",
        "Claim it from your server's own network, once. " + std::string(me == "this Mac" ? "Your Mac" : "This computer") +
            " writes a one-time token to the share to prove it can.");
    button(w, "claim_go", "Claim", busy, "primary", "Claim the service");
    end_row(w);
    field(w, "local", "Address on your network", local_, "server.local:7878");
    public_rows(false);
    text(w, "pass_t",
         "Server passphrase: the one you set on the server with driftwire serve init. If it has none, choose one here: enter it twice, at least 10 characters.",
         "secondary");
    secure(w, sid("pass"), "Server passphrase");
    secure(w, sid("pass_again"), "Again, to choose it", "Leave empty when the server already has one");
    end(w);
  };

  const auto passphrase_rows = [&] {
    std::string detail;
    if (s.nas_passphrase == "set") {
      detail = s.unlocked_s > 0 ? "Unlocked \xC2\xB7 " + std::to_string(std::max<std::int64_t>(1, (s.unlocked_s + 59) / 60)) + " min"
                                : "Locked";
    } else if (s.nas_passphrase == "none") {
      detail = "None. Choose one so nobody else on your network can claim or change this service.";
    } else if (s.nas_passphrase == "broken") {
      detail = "Your server can't read it. Reset it on the server with driftwire serve passphrase reset.";
    } else {
      detail = "Update the service on your server to use this.";
    }
    row(w, "passphrase", "Server passphrase", detail);
    if (s.nas_passphrase == "set" && s.unlocked_s > 0) {
      button(w, "lock", "Lock", busy, "plain", "Lock: forget the server's key on " + me);
    } else if (s.nas_passphrase == "set" && !unlock_wanted_) {
      button(w, "unlock_open", "Unlock\xE2\x80\xA6", busy, "plain", "Unlock with the server passphrase");
    }
    if (s.nas_passphrase == "set" || s.nas_passphrase == "none") {
      button(w, "change_open", s.nas_passphrase == "set" ? "Change\xE2\x80\xA6" : "Choose\xE2\x80\xA6", busy, "plain",
             s.nas_passphrase == "set" ? "Change the server passphrase" : "Choose a server passphrase");
    }
    end_row(w);
    if (changing_) {
      stack(w, "change_box", 8);
      text(w, "change_note", "At least 10 characters. Nothing on " + me + " keeps it.", "secondary");
      secure(w, sid("new_pass"), "New passphrase");
      secure(w, sid("new_pass_again"), "Again");
      stack(w, "change_buttons", 8, "horizontal");
      button(w, "change_cancel", "Cancel");
      button(w, "change_set", "Set passphrase", busy, "primary");
      end(w);
      end(w);
    }
    if (unlock_wanted_ && s.nas_passphrase == "set") {
      row(w, "unlock", "Unlock",
          "Enter the server passphrase, then do that again. " + Me + " keeps the key for 10 minutes and never the passphrase.");
      secure(w, sid("unlock_pass"), "Server passphrase");
      button(w, "unlock_go", "Unlock", busy, "primary");
      button(w, "unlock_cancel", "Cancel", false, "plain", "Cancel the unlock");
      end_row(w);
    }
  };

  const auto code_rows = [&] {
    if (s.qr.empty()) {
      row(w, "add_phone", "Add a phone",
          s.added.empty() ? "One scan pairs a phone with " + me + " and your server." : "Added " + s.added.back() + ".");
      std::vector<option> opts{{"0", kUnlimited}};
      for (const auto& [label, secs] : durations()) opts.push_back({std::to_string(secs), label});
      choice(w, "code_for", "How long the phone may use this library", opts,
             s.has("passphrase") ? std::to_string(code_for_) : "0", !s.has("passphrase"));
      button(w, "pair", "Show code", busy, "primary");
      end_row(w);
      return;
    }
    stack(w, "code", 16, "horizontal", "card");
    w.begin_object().key("type").string("surface").key("id").string("qr_picture").key("surface").string(kQrSurface);
    w.key("width").integer(240).key("height").integer(240).key("label").string("Code for your phone to scan").end_object();
    stack(w, "code_words", 6);
    text(w, "code_title", "Scan with MediaViewer on your phone", "heading");
    if (s.qr_for_s > 0) {
      text(w, "code_guest", "Guest code \xC2\xB7 access for " + duration_text(s.qr_for_s) + " from when it is scanned", "body");
    }
    text(w, "code_check", "Your phone will show " + s.qr_library + " and this code. Check they match before you confirm.",
         "secondary");
    copyable(w, "code_fingerprint", s.qr_fingerprint, "mono", "Check code " + s.qr_fingerprint);
    const std::int64_t left = std::max<std::int64_t>(0, s.qr_expires - now);
    char good[48];
    std::snprintf(good, sizeof good, "Good for %lld:%02lld", static_cast<long long>(left / 60),
                  static_cast<long long>(left % 60));
    text(w, "code_left", good, "secondary");
    button(w, "pair_stop", "Cancel", false, "plain", "Cancel the code");
    end(w);
    end(w);
  };

  const auto phone_rows = [&] {
    for (std::size_t i = 0; i < s.phones.size(); ++i) {
      const phone_row& p = s.phones[i];
      const std::string id = "ph" + std::to_string(i);
      std::string lead = p.guest ? (p.backup ? "Guest \xC2\xB7 Backs up \xC2\xB7 " : "Guest \xC2\xB7 ")
                                 : (p.backup ? "" : "No backup \xC2\xB7 ");
      std::string detail;
      if (!p.limited || p.expires == 0) {
        detail = lead + "Key " + short_key(p.key);
      } else if (p.expired || p.expires <= now) {
        detail = lead + "Expired " + relative(p.expires, now);
      } else {
        detail = lead + "Until " + weekday_clock(p.expires);
      }
      row(w, id, p.name, detail);
      // Extend offers only lengths that end later than the phone's end now.
      std::vector<option> opts{{"", p.limited ? "Extend\xE2\x80\xA6" : "Limit\xE2\x80\xA6"}};
      for (const auto& [label, secs] : durations()) {
        if (!p.limited || p.expires == 0 || now + secs > p.expires) opts.push_back({std::to_string(secs), std::string(label) + " from now"});
      }
      if (p.limited) opts.push_back({"unlimited", kUnlimited});
      choice(w, id + "_access", p.limited ? "Extend " + p.name + "'s access" : "Limit " + p.name + "'s access", opts, "",
             busy || !s.has("passphrase"));
      if (s.has("backup")) {
        toggle(w, id + "_backup", "Backs up", p.backup, busy);
      }
      button(w, id + "_remove", "Remove", busy, "plain", "Remove " + p.name);
      end_row(w);
    }
    if (!s.reachable) text(w, "unreachable", "Couldn't reach your server. These are the phones it knew about last.", "secondary");
  };

  const auto relay_rows = [&] {
    const bool available = s.has("relay");
    std::string detail;
    if (!available) detail = "Update the service on your server to reach it away from home without a forwarded port.";
    else if (s.relay.empty())
      detail = "Away from home without a forwarded port, phones need a relay you run yourself (driftwire-relay on your own server). There is no public one.";
    else
      detail = "Phones away from home go through " + s.relay +
               " when they can't reach your server directly. It passes sealed bytes only.";
    row(w, "relay", "Relay (your own server)", detail);
    if (!s.relay.empty()) button(w, "relay_remove", "Remove", busy, "plain", "Stop using the relay");
    button(w, "relay_open", s.relay.empty() ? "Set up\xE2\x80\xA6" : "Change\xE2\x80\xA6", busy || !available, "plain",
           s.relay.empty() ? "Set up your own relay" : "Change the relay");
    end_row(w);
    if (relay_editing_) {
      stack(w, "relay_box", 8);
      field(w, "relay_address", "Relay address", relay_address_, "relay.example.com:7879");
      text(w, "relay_key_note", "The key your driftwire-relay was started with. Your server keeps it; " + me + " doesn't.",
           "secondary");
      secure(w, sid("relay_key"), "Relay key");
      stack(w, "relay_buttons", 8, "horizontal");
      button(w, "relay_cancel", "Cancel");
      button(w, "relay_save", "Save relay", busy, "primary");
      end(w);
      end(w);
    }
  };

  const auto folder_rows = [&] {
    for (std::size_t i = 0; i < s.folders.size(); ++i) {
      const folder_row& f = s.folders[i];
      const std::string id = "fo" + std::to_string(i);
      std::string detail = f.path;
      if (f.backup) {
        detail += " \xC2\xB7 " + gigabytes(f.used) + " of " + gigabytes(f.limit) + " GB used, keeps " + gigabytes(f.reserve) +
                  " GB free. ";
        detail += f.indexed ? "Indexed by " + me + ": what phones send reaches search after the next scan."
                            : "Not in Local search on " + me + " yet: add this folder there so what phones send reaches search.";
      }
      row(w, id, f.backup ? f.label + " \xC2\xB7 phone backup" : f.label, detail);
      if (f.backup) button(w, id + "_limit", "Change limit", busy, "plain", "Change the storage limit of " + f.label);
      if (!f.catalogue()) button(w, id + "_stop", "Stop sharing", busy, "plain", "Stop sharing " + f.label);
      end_row(w);
      if (editing_ == f.id) {
        stack(w, id + "_edit", 8, "horizontal");
        field(w, "edit_limit", "Storage limit in gigabytes", edit_limit_, "GB");
        field(w, "edit_reserve", "Free space to keep on the server, in gigabytes", edit_reserve_, "GB");
        button(w, "edit_save", "Save", busy, "primary");
        button(w, "edit_cancel", "Cancel");
        end(w);
      }
    }
    stack(w, "new_folder", 8, "vertical", "card");
    text(w, "new_title", "Share another folder", "body");
    field(w, "new_label", "Name of the new shared folder", new_label_,
          new_path_.empty() ? "Label" : share_label(new_path_));
    field(w, "new_path", "Path of the new shared folder on the server", new_path_,
          "Folder on the server, like /volume1/Video or /Volumes/Drive/Video");
    toggle(w, "new_backup", "Phones back up to this folder", new_backup_, !s.has("backup"));
    if (new_backup_) {
      stack(w, "new_sizes", 8, "horizontal");
      field(w, "new_limit", "Storage limit in gigabytes", new_limit_, "GB");
      field(w, "new_reserve", "Free space to keep on the server, in gigabytes", new_reserve_, "GB");
      end(w);
    }
    button(w, "new_add", "Add", busy, "plain", "Add the shared folder");
    end(w);
  };

  const auto other_publisher_rows = [&] {
    row(w, "other", "Published from another computer",
        "Key " + s.other_fingerprint +
            ". Ask it to hand over, and phones follow by themselves. If it's gone, start over and scan each phone again.");
    button(w, "takeover_ask", "Ask to hand over", busy);
    if (!confirm_restart_) button(w, "restart_open", "Start over\xE2\x80\xA6", busy);
    end_row(w);
    if (confirm_restart_) {
      row(w, "restart", "Start over from " + me + "?", "Every phone will have to scan a new code.");
      button(w, "restart_cancel", "Cancel");
      button(w, "restart_go", "Start over", busy, "destructive");
      end_row(w);
    }
  };

  const auto asking_rows = [&] {
    row(w, "asking", s.asking_name + " wants to publish this library",
        "Its key is " + s.asking_fingerprint + ". Check it matches what that computer shows.");
    if (!confirm_hand_over_) button(w, "handover_open", "Hand over\xE2\x80\xA6", busy);
    end_row(w);
    if (confirm_hand_over_) {
      row(w, "handover", "Hand this library to " + s.asking_name + "?",
          Me + " will stop publishing it. Phones follow by themselves.");
      button(w, "handover_cancel", "Cancel");
      button(w, "handover_go", "Hand over", busy, "primary");
      end_row(w);
    }
  };

  const auto library_rows = [&] {
    row(w, "folder", "Folder on your server",
        s.folder.empty() ? "Where your phone reads your library. Nothing is written there until you choose it." : s.folder);
    pick_folder(w, "folder_pick", "Choose the folder on your server", "", s.folder.empty() ? "Choose\xE2\x80\xA6" : "Change\xE2\x80\xA6",
                busy || (s.catalogue && s.catalogue->running));
    end_row(w);
    if (s.folder.empty()) return;
    search_rows();
    if (s.mismatch) {
      row(w, "mismatch", "The share doesn't match " + me,
          "Its library.json names another library or an older epoch, perhaps after a restore. " + Me +
              " goes on publishing its own; phones will refuse what doesn't match until it is fixed.");
      end_row(w);
    }
    if (s.other_publisher) other_publisher_rows();
    if (!s.asking_name.empty()) asking_rows();
    if (!s.claimed) {
      claim_rows();
      return;
    }
    passphrase_rows();
    code_rows();
    phone_rows();
    field(w, "local", "Address on your network", local_, "server.local:7878", busy);
    public_rows(true);
    relay_rows();
    folder_rows();
  };

  const auto this_computer_rows = [&] {
    if (!(s.host && s.host->installed)) {
      std::string install;
      if (computer_ == computer::mac) {
        install = "This Mac needs the driftwire program to be the server. Install it once with brew install longtimeno-c/tap/driftwire (no administrator), then choose Check again.";
      } else if (computer_ == computer::pc) {
        install = "This PC needs the driftwire program to be the server. Install it once in PowerShell with irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex (no administrator), then choose Check again.";
      } else {
        install = "This computer needs the driftwire program to be the server. Install it from your package manager, then choose Check again.";
      }
      row(w, "program", "The driftwire program", install);
      button(w, "host_check", "Check again", busy);
      end_row(w);
      return;
    }
    for (std::size_t i = 0; i < host_folders_.size(); ++i) {
      const host_folder& f = host_folders_[i];
      const std::string label = f.backup ? std::string("Phone backup") : share_label(f.path);
      const std::string id = "hf" + std::to_string(i);
      row(w, id, f.backup ? label + " \xC2\xB7 phones back up here" : label, f.path);
      button(w, id + "_remove", "Remove", busy, "plain", "Don't share " + label);
      end_row(w);
    }
    row(w, "host_folders", "Folders to share",
        host_folders_.empty() ? "A folder or a whole drive your phone reads, and a folder phones back up into if you want one."
                              : "Your phone reads these. The first one holds the library.");
    pick_folder(w, "host_add", "Add a folder or drive to share", "", "Add folder\xE2\x80\xA6", busy);
    pick_folder(w, "host_add_backup", "Add a folder for phones to back up into", "", "Add backup folder\xE2\x80\xA6", busy);
    end_row(w);
    row(w, "duck", "DuckDNS name",
        "Optional, free at duckdns.org: how your phone finds " + me +
            " away from home. DuckDNS learns your home's address, nothing about your files.");
    end_row(w);
    field(w, "duck_name", "DuckDNS name", duck_name_, "home.duckdns.org", busy);
    if (!trim(duck_name_).empty()) {
      text(w, "duck_token_note", "DuckDNS token: from your duckdns.org page. Kept by the server on " + me + ", never shown again.",
           "secondary");
      secure(w, sid("duck_token"), "DuckDNS token");
    }
    row(w, "host_go_row", "Set up " + me,
        "It runs while you're logged in, starts by itself when you log in, and keeps " + me +
            " awake while it serves (the display may still sleep). No administrator.");
    button(w, "host_setup", "Set up", busy, "primary", "Set up " + me + " as your server");
    end_row(w);
  };

  // Until a service is claimed; and again after a setup on this computer that stopped part way.
  if (!s.claimed && (s.folder.empty() || (s.host && s.host->set_up))) {
    row(w, "server", "Your server",
        "Where your phone reads your library: " + me +
            ", a Synology, or another computer you run. Each works the same way.");
    if (!setup_open_) button(w, "setup_open", "Set up the service\xE2\x80\xA6", false, "primary");
    end_row(w);
    if (setup_open_) {
      choice(w, "setup_kind", "Where the service runs",
             {{"this", "This computer"}, {"synology", "A Synology"}, {"other", "Something else"}}, setup_kind_);
      if (setup_kind_ == "this") {
        this_computer_rows();
      } else if (setup_kind_ == "synology") {
        text(w, "synology_note",
             "Install the MediaViewer package on your Synology: Package Center \xE2\x86\x92 Manual Install, choose the file, accept the warning about a package not from Synology, and pick the shared folder. Then choose that folder below and claim it.",
             "secondary");
        library_rows();
      } else {
        text(w, "other_note",
             "Run driftwire serve on the computer that keeps your library (driftwire serve init, then driftwire serve). Then choose its folder below and claim it.",
             "secondary");
        library_rows();
      }
    }
  } else {
    library_rows();
  }

  // The link's last refusal, or this screen's own line.
  std::string e;
  const std::string& r = s.error;
  if (r.empty()) e = note_;
  else if (r == "refused") e = "The service said no. It takes a claim only from its own network, and only once.";
  else if (r == "unreachable") e = "Couldn't reach the service.";
  else if (r == "bad_address") e = "That isn't an address. Use name:port.";
  else if (r == "too_many") e = "Too many phones are paired. Remove one first.";
  else if (r == "no_folder" || r == "not_published") e = "Choose the folder on your server first.";
  else if (r == "other_library") e = "That folder holds another library's catalogue.";
  else if (r == "no_key_store") e = "This copy of MediaViewer can't keep a signing key.";
  else if (r == "revoke_waits") e = "Removed here. Your server will be told when it can be reached.";
  else if (r == "no_driftwire") e = "This copy of MediaViewer can't keep phone keys.";
  else if (r == "no_local_search") e = "Install Local search to publish a catalogue.";
  else if (r == "needs_unlock") e = "Enter the server passphrase to do that.";
  else if (r == "passphrase") e = "That is not the server passphrase.";
  else if (r == "passphrase_short") e = "A passphrase needs at least 10 characters.";
  else if (r == "no_passphrase") e = "This server has no passphrase yet. Enter a new one twice to choose it.";
  else if (r == "slow_down") e = "Try again in a moment.";
  else if (r == "wrong_service") e = "That is a different server from the one this computer claimed.";
  else if (r == "no_driftwire_program") e = "Install the driftwire program first, then choose Check again.";
  else if (r == "host_no_folders") e = "Choose a folder for your phone to read.";
  else if (r == "host_no_library") e = "Choose a folder for your phone to read, not only a backup folder.";
  else if (r == "host_twice") e = "That folder is in the list twice.";
  else if (r == "host_bad_name") e = "That isn't a DuckDNS name. Use the name you chose there, like home or home.duckdns.org.";
  else if (r == "host_bad_token") e = "That isn't a DuckDNS token. Copy it from duckdns.org; it looks like 1a2b3c4d-\xE2\x80\xA6";
  else if (r == "host_share") e = s.host_said.empty() ? "The folder couldn't be shared." : "Couldn't share it: " + s.host_said;
  else if (r == "host_name") e = s.host_said.empty() ? "DuckDNS didn't take the name." : "DuckDNS: " + s.host_said;
  else if (r == "host_start") e = s.host_said.empty() ? "The server didn't start." : "The server didn't start: " + s.host_said;
  else if (r == "host_not_listening") e = "The server started but isn't answering yet. Another program may be using its port.";
  else if (r == "host_no_network") e = "Claimed, but " + me + " isn't on a network, so a phone can't reach it yet.";
  else if (r == "host_failed") e = "The driftwire program couldn't be run.";
  else if (r == "locked_out")
    e = "Too many tries. Try again at " + clock_text(s.locked_until) +
        ", or reset it on the server with driftwire serve passphrase reset.";
  else if (r == "nas_too_old")
    e = s.claimed && (s.nas_passphrase.empty() || s.nas_passphrase == "none")
            ? "Claimed. Update the service on your server to protect it with a passphrase."
            : "Update the service on your server to use this.";
  else e = "That didn't work.";
  if (!e.empty()) text(w, "error", e, "secondary");

  end(w);
  return w.take();
}

// ---- events ---------------------------------------------------------------------------------

namespace {

std::string value_string(std::string_view value_json) {
  const auto v = json::parse(value_json, 2);
  return v && v->k == json::kind::string ? v->s : std::string();
}
bool value_bool(std::string_view value_json) {
  const auto v = json::parse(value_json, 2);
  return v && v->k == json::kind::boolean && v->b;
}
// The index in "ph3_remove" after `prefix`, and the rest ("_remove").
bool split_row(std::string_view node, std::string_view prefix, std::size_t& index, std::string_view& rest) {
  if (node.substr(0, prefix.size()) != prefix) return false;
  std::size_t i = prefix.size(), n = 0;
  bool any = false;
  while (i < node.size() && node[i] >= '0' && node[i] <= '9') {
    n = n * 10 + static_cast<std::size_t>(node[i] - '0');
    ++i;
    any = true;
  }
  if (!any) return false;
  index = n;
  rest = node.substr(i);
  return true;
}

struct req {
  writer w;
  req(const char* what) { w.begin_object().key("do").string(what); }
  req& s(const char* k, const std::string& v) {
    w.key(k).string(v);
    return *this;
  }
  req& i(const char* k, std::int64_t v) {
    w.key(k).integer(v);
    return *this;
  }
  req& b(const char* k, bool v) {
    w.key(k).boolean(v);
    return *this;
  }
  std::string done() { return w.end_object().take(); }
};

}  // namespace

bool screen::holds_secret() const noexcept {
  return !passphrase_.empty() || !passphrase_again_.empty() || !unlock_with_.empty() || !relay_key_.empty() ||
         !duck_token_.empty();
}

std::vector<std::string> screen::on_event(std::string_view node, std::uint32_t kind, std::string_view value_json,
                                          std::int64_t now) {
  // "pass.3" is the field "pass" (its generation, build()); an older one's typing is dropped.
  std::string_view base = node;
  if (const auto dot = node.find('.'); dot != std::string_view::npos) {
    if (node.substr(dot + 1) != std::to_string(secret_gen_)) return {};
    base = node.substr(0, dot);
  }
  const bool before = holds_secret();
  auto out = handle(base, kind, value_json, now);
  if (before && !holds_secret()) ++secret_gen_;  // sent or dropped: the host draws empty fields
  return out;
}

std::vector<std::string> screen::handle(std::string_view node, std::uint32_t kind, std::string_view value_json,
                                        std::int64_t /*now*/) {
  std::vector<std::string> out;
  note_.clear();
  const std::string v = kind == kChange || kind == kSubmit ? value_string(value_json) : std::string();

  // ---- typed (the tree keeps plain ones; secure ones stay here only) ----
  if (kind == kChange || kind == kSubmit) {
    if (node == "local") {
      local_ = v;
      if (claimed_ && kind == kSubmit) out.push_back(req("set_addresses").s("local", local_).s("public", public_).done());
    } else if (node == "public") {
      public_ = v;
      if (claimed_) out.push_back(req("set_addresses").s("local", local_).s("public", public_).done());
    } else if (node == "pass") {
      passphrase_ = v;
    } else if (node == "pass_again") {
      passphrase_again_ = v;
    } else if (node == "new_pass") {
      passphrase_ = v;
    } else if (node == "new_pass_again") {
      passphrase_again_ = v;
    } else if (node == "unlock_pass") {
      unlock_with_ = v;
      if (kind == kSubmit && !unlock_with_.empty()) {
        out.push_back(req("unlock").s("passphrase", unlock_with_).done());
        unlock_with_.clear();
      }
    } else if (node == "relay_address") {
      relay_address_ = v;
    } else if (node == "relay_key") {
      relay_key_ = v;
    } else if (node == "duck_name") {
      duck_name_ = v;
    } else if (node == "duck_token") {
      duck_token_ = v;
    } else if (node == "new_label") {
      new_label_ = v;
    } else if (node == "new_path") {
      new_path_ = v;
    } else if (node == "new_limit") {
      new_limit_ = v;
    } else if (node == "new_reserve") {
      new_reserve_ = v;
    } else if (node == "edit_limit") {
      edit_limit_ = v;
    } else if (node == "edit_reserve") {
      edit_reserve_ = v;
    } else if (node == "new_backup") {
      new_backup_ = value_bool(value_json);
      // A suggestion the owner changes; the service refuses past what is really free.
      if (new_backup_ && new_limit_.empty()) new_limit_ = "1000";
    } else if (node == "code_for") {
      code_for_ = std::strtoll(v.c_str(), nullptr, 10);
    } else if (node == "setup_kind") {
      if (v == "this" || v == "synology" || v == "other") setup_kind_ = v;
      if (setup_kind_ == "this") out.push_back(req("host_status").done());
    } else if (node == "folder_pick") {
      if (!v.empty()) out.push_back(req("folder").s("path", v).done());
    } else if (node == "host_add" || node == "host_add_backup") {
      const bool backup = node == "host_add_backup";
      const bool there = std::any_of(host_folders_.begin(), host_folders_.end(),
                                     [&](const host_folder& f) { return f.path == v; });
      if (!v.empty() && !there) host_folders_.push_back({v, backup});
    } else {
      std::size_t i = 0;
      std::string_view rest;
      if (split_row(node, "ph", i, rest) && i < phone_keys_.size()) {
        if (rest == "_access" && !v.empty()) {
          const std::int64_t secs = v == "unlimited" ? 0 : std::strtoll(v.c_str(), nullptr, 10);
          out.push_back(req("grant").s("key", phone_keys_[i]).i("for_s", secs).done());
        } else if (rest == "_backup") {
          out.push_back(req("phone_backup").s("key", phone_keys_[i]).b("on", value_bool(value_json)).done());
        }
      }
    }
    return out;
  }
  if (kind != kClick) return out;

  // ---- clicks ----
  if (node == "setup_open") {
    setup_open_ = true;
    out.push_back(req("host_status").done());
  } else if (node == "host_check") {
    out.push_back(req("host_status").done());
  } else if (node == "host_start") {
    out.push_back(req("host_start").done());
  } else if (node == "host_setup") {
    const bool any_library = std::any_of(host_folders_.begin(), host_folders_.end(),
                                         [](const host_folder& f) { return !f.backup; });
    const std::string name = trim(duck_name_);
    if (!any_library) {
      note_ = "Choose a folder for your phone to read.";
    } else if (!name.empty() && duck_token_.empty()) {
      note_ = "Paste the DuckDNS token too, or leave the name empty.";
    } else {
      // The folders phones read first: the first of them holds the library.
      writer w;
      w.begin_object().key("do").string("host_setup").key("folders").begin_array();
      for (int pass = 0; pass < 2; ++pass) {
        for (const host_folder& f : host_folders_) {
          if (f.backup != (pass == 1)) continue;
          w.begin_object().key("path").string(f.path).key("backup").boolean(f.backup);
          if (f.backup) w.key("label").string("Phone backup");
          w.end_object();
        }
      }
      w.end_array();
      if (!name.empty()) w.key("duckdns").string(name).key("duckdns_token").string(duck_token_);
      out.push_back(w.end_object().take());
      duck_token_.clear();  // handed on; the server keeps it, this computer doesn't
    }
  } else if (node == "publish") {
    out.push_back(req("publish").done());
  } else if (node == "publish_cancel") {
    out.push_back(req("cancel").done());
  } else if (node == "claim_go") {
    if (trim(local_).empty()) {
      note_ = "Type the server's address on your network first, like server.local:7878.";
    } else if (!passphrase_again_.empty() && passphrase_again_ != passphrase_) {
      note_ = "The two passphrases are not the same.";
    } else {
      req r("claim");
      r.s("local", local_).s("public", public_);
      if (!passphrase_.empty()) r.s("passphrase", passphrase_).b("new_passphrase", !passphrase_again_.empty());
      out.push_back(r.done());
    }
    passphrase_.clear();
    passphrase_again_.clear();
  } else if (node == "lock") {
    out.push_back(req("lock").done());
  } else if (node == "unlock_open") {
    unlock_wanted_ = true;
  } else if (node == "unlock_go") {
    if (!unlock_with_.empty()) out.push_back(req("unlock").s("passphrase", unlock_with_).done());
    unlock_with_.clear();
  } else if (node == "unlock_cancel") {
    unlock_wanted_ = false;
    unlock_with_.clear();
  } else if (node == "change_open") {
    changing_ = !changing_;
    passphrase_.clear();
    passphrase_again_.clear();
  } else if (node == "change_cancel") {
    changing_ = false;
    passphrase_.clear();
    passphrase_again_.clear();
  } else if (node == "change_set") {
    if (passphrase_.size() < 10) {
      note_ = "A passphrase needs at least 10 characters.";
    } else if (passphrase_ != passphrase_again_) {
      note_ = "The two passphrases are not the same.";
    } else {
      out.push_back(req("set_passphrase").s("passphrase", passphrase_).done());
      changing_ = false;
    }
    passphrase_.clear();
    passphrase_again_.clear();
  } else if (node == "pair") {
    out.push_back(req("pair").i("for_s", code_for_).done());
  } else if (node == "pair_stop") {
    out.push_back(req("pair_stop").done());
  } else if (node == "relay_remove") {
    out.push_back(req("set_relay").s("address", "").s("key", "").done());
  } else if (node == "relay_open") {
    relay_editing_ = !relay_editing_;
    relay_address_ = relay_now_;
    relay_key_.clear();
  } else if (node == "relay_cancel") {
    relay_editing_ = false;
    relay_key_.clear();
  } else if (node == "relay_save") {
    const std::string address = trim(relay_address_);
    if (address.empty()) {
      note_ = "Type the relay's address, like relay.example.com:7879.";
    } else if (relay_key_.size() < kRelayKeyMinimum) {
      note_ = "The relay key is at least 16 characters.";
    } else {
      out.push_back(req("set_relay").s("address", address).s("key", relay_key_).done());
      relay_editing_ = false;
    }
    relay_key_.clear();
  } else if (node == "new_add") {
    const std::string path = trim(new_path_);
    const std::string label = new_label_.empty() ? share_label(path) : new_label_;
    const std::int64_t limit = bytes(new_limit_), reserve = bytes(new_reserve_);
    if (path.empty() || path[0] != '/' || share_label(path).empty()) {
      note_ = "Type the folder's path on the server, starting with /.";
    } else if (new_backup_ && (limit <= 0 || reserve < 0)) {
      note_ = "Type the backup folder's limit and the space to keep free, in GB.";
    } else {
      req r("add_folder");
      r.s("label", label).s("path", path);
      if (new_backup_) r.s("backup", "1").s("limit", std::to_string(limit)).s("reserve", std::to_string(reserve));
      out.push_back(r.done());
      new_label_.clear();
      new_path_.clear();
      new_backup_ = false;
    }
  } else if (node == "edit_save") {
    const std::int64_t limit = bytes(edit_limit_), reserve = bytes(edit_reserve_);
    if (limit <= 0 || reserve < 0) {
      note_ = "Type the limit and the space to keep free, in GB.";
    } else {
      out.push_back(req("set_backup").s("id", editing_).s("limit", std::to_string(limit)).s("reserve", std::to_string(reserve)).done());
      editing_.clear();
    }
  } else if (node == "edit_cancel") {
    editing_.clear();
  } else if (node == "takeover_ask") {
    out.push_back(req("takeover_ask").done());
  } else if (node == "restart_open") {
    confirm_restart_ = true;
  } else if (node == "restart_cancel") {
    confirm_restart_ = false;
  } else if (node == "restart_go") {
    confirm_restart_ = false;
    out.push_back(req("takeover_restart").done());
  } else if (node == "handover_open") {
    confirm_hand_over_ = true;
  } else if (node == "handover_cancel") {
    confirm_hand_over_ = false;
  } else if (node == "handover_go") {
    confirm_hand_over_ = false;
    if (!asking_key_.empty()) out.push_back(req("takeover_allow").s("key", asking_key_).done());
  } else {
    std::size_t i = 0;
    std::string_view rest;
    if (split_row(node, "ph", i, rest) && i < phone_keys_.size()) {
      if (rest == "_remove") out.push_back(req("revoke").s("key", phone_keys_[i]).done());
    } else if (split_row(node, "fo", i, rest) && i < folder_ids_.size()) {
      if (rest == "_stop") {
        out.push_back(req("remove_folder").s("id", folder_ids_[i]).done());
      } else if (rest == "_limit") {
        editing_ = folder_ids_[i];
        edit_limit_ = gigabytes(folder_limit_[i]);
        edit_reserve_ = gigabytes(folder_reserve_[i]);
      }
    } else if (split_row(node, "hf", i, rest) && i < host_folders_.size()) {
      if (rest == "_remove") host_folders_.erase(host_folders_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
  return out;
}

void screen::forget_secrets() {
  if (holds_secret()) ++secret_gen_;
  duck_token_.clear();
  relay_key_.clear();
  relay_editing_ = false;
  passphrase_.clear();
  passphrase_again_.clear();
  unlock_with_.clear();
  changing_ = false;
  unlock_wanted_ = false;
  note_.clear();
}

}  // namespace mv::phone::card
