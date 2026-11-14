// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_drives.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/serve_tty.h"
#include "core/json.h"

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

namespace {

int fail(const std::string& why, int code) {
  print("driftwire host drives: " + why + "\n", true);
  return code;
}

std::string canonical_or(const std::string& p) {
  std::error_code ec;
  const fs::path in(std::u8string(reinterpret_cast<const char8_t*>(p.data()), p.size()));
  fs::path c = fs::weakly_canonical(in, ec);
  if (ec) c = in;
  const std::u8string u = c.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string parent_of(const std::string& p) {
  const fs::path in(std::u8string(reinterpret_cast<const char8_t*>(p.data()), p.size()));
  const std::u8string u = in.parent_path().u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace

drives_words parse_drives_words(std::span<const std::string> w) {
  drives_words d;
  const std::string what = w.empty() ? "list" : w[0];
  const auto rest = w.empty() ? w : w.subspan(1);
  if (what == "list" && rest.empty()) {
    d.act = drives_action::list;
  } else if (what == "choices" && !rest.empty()) {
    d.act = drives_action::choices;
    d.ids.assign(rest.begin(), rest.end());
  } else if (what == "combine" && rest.size() >= 2) {
    d.act = drives_action::combine;
    const auto c = drives::parse_choice(rest[0]);
    if (!c) {
      d.error = "combine takes separate, mirror, span, stripe or parity, then the drives";
      return d;
    }
    d.c = *c;
    d.ids.assign(rest.begin() + 1, rest.end());
  } else if (what == "replace" && rest.size() == 2) {
    d.act = drives_action::replace;
    d.set = rest[0];
    d.ids = {rest[1]};
  } else {
    d.error = "host drives takes list, choices DRIVE..., combine separate|mirror|span|stripe|parity DRIVE... "
              "--label NAME, or replace SET DRIVE";
  }
  return d;
}

drives::guard host_guard(const host_layout& l, const std::string& data_dir, bool test_hooks, const std::string& images_under) {
  drives::guard g;
  g.protected_paths.push_back(canonical_or(data_dir));                 // driftwire's: the host, its library
  g.protected_paths.push_back(canonical_or(host_catalogue_of(l)));
  const std::string mine = cli::data_dir();                            // the person's own driftwire, if `--data` is another
  if (!mine.empty()) {
    g.protected_paths.push_back(canonical_or(mine));
    // MediaViewer's own data folder sits beside driftwire's (Application Support, %LOCALAPPDATA%).
    g.protected_paths.push_back(canonical_or(parent_of(mine) + "/MediaViewer"));
  }
  if (const std::string home = home_dir(); !home.empty()) {
    g.protected_paths.push_back(canonical_or(home + "/Library/Application Support/MediaViewer"));
    g.protected_paths.push_back(canonical_or(home + "/.local/share/MediaViewer"));
  }
  if (const auto cfg = host_files::config(l, test_hooks)) {
    for (const auto& r : cfg->roots) g.shared_paths.push_back(canonical_or(r.path));
  }
  if (test_hooks) g.images_under = images_under.empty() ? std::string("\x01no disk at all") : canonical_or(images_under);
  return g;
}

std::string listing_json(const drives::listing& l, const drives::guard& g) {
  json::writer w;
  w.begin_object();
  w.key("os").string(drives::this_os() == drives::os_kind::mac ? "mac" : drives::this_os() == drives::os_kind::windows ? "windows" : "linux");
  w.key("pool_ok").boolean(l.pool_ok);
  if (!l.error.empty()) w.key("error").string(l.error);
  w.key("not_a_backup").string(drives::not_a_backup_text());
  w.key("disks").begin_array();
  for (const auto& d : l.disks) {
    w.begin_object().key("id").string(d.id).key("name").string(d.name).key("model").string(d.model);
    w.key("size").integer(static_cast<std::int64_t>(d.size)).key("used").integer(static_cast<std::int64_t>(drives::used_of(d)));
    w.key("folders").integer(static_cast<std::int64_t>(drives::folders_of(d)));
    w.key("facts").string(drives::facts_line(d));
    w.key("system").boolean(d.system).key("internal").boolean(d.internal).key("image").boolean(d.image);
    if (!d.smart.empty()) w.key("smart").string(d.smart);
    const std::string why = drives::refusal_of(d, g);
    if (!why.empty()) w.key("refused").string(why);
    w.key("volumes").begin_array();
    for (const auto& v : d.volumes) {
      w.begin_object().key("label").string(v.label).key("size").integer(static_cast<std::int64_t>(v.size));
      w.key("used").integer(static_cast<std::int64_t>(v.used)).key("folders").integer(static_cast<std::int64_t>(v.folders));
      w.key("mounted").boolean(!v.mount.empty()).end_object();
    }
    w.end_array().end_object();
  }
  w.end_array();
  w.key("sets").begin_array();
  for (const auto& s : l.sets) {
    w.begin_object().key("id").string(s.id).key("name").string(s.name).key("kind").string(drives::choice_word(s.kind));
    w.key("state").string(drives::health_word(s.state)).key("percent").integer(s.percent);
    w.key("size").integer(static_cast<std::int64_t>(s.size)).key("line").string(drives::health_line(s));
    w.key("members").begin_array();
    for (const auto& m : s.members) {
      w.begin_object().key("id").string(m.id).key("status").string(m.status).key("missing").boolean(m.missing).end_object();
    }
    w.end_array().end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

std::string choices_json(std::span<const drives::option> options) {
  json::writer w;
  w.begin_object().key("not_a_backup").string(drives::not_a_backup_text()).key("options").begin_array();
  for (const auto& o : options) {
    w.begin_object().key("choice").string(drives::choice_word(o.c)).key("title").string(o.title);
    w.key("offered").boolean(o.offered);
    if (!o.why_not.empty()) w.key("why_not").string(o.why_not);
    w.key("recommended").boolean(o.recommended).key("warning").boolean(o.warning);
    w.key("space").integer(static_cast<std::int64_t>(o.space)).key("space_words").string(o.space_words);
    w.key("dead_words").string(o.dead_words).key("speed_words").string(o.speed_words).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::string choices_text(std::span<const drives::option> options, int columns) {
  std::string out = std::string(drives::not_a_backup_text()) + "\n\n";
  // One block per choice: side by side needs room the terminal rarely has for five, so they
  // stack, each with the same three lines in the same order (the card puts them in columns).
  (void)columns;
  for (const auto& o : options) {
    out += "  " + o.title + std::string(" (") + drives::choice_word(o.c) + ")";
    if (o.recommended) out += "  recommended for photos";
    if (o.warning) out += "  warning";
    out += "\n";
    if (!o.offered) out += "    not offered: " + o.why_not + "\n";
    out += "    space      " + o.space_words + "\n";
    out += "    one dies   " + o.dead_words + "\n";
    out += "    speed      " + o.speed_words + "\n";
  }
  return out;
}

std::string drives_status_line(std::span<const drives::set_health> sets) {
  if (sets.empty()) return {};
  std::string out;
  for (const auto& s : sets) {
    out += (out.empty() ? "drives      " : "\n            ") + drives::health_line(s);
  }
  return out;
}

// ---- the commands ---------------------------------------------------------------------------

namespace {

bool read_stdin_line(std::string& out) {
  if (!std::getline(std::cin, out)) return false;
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return true;
}

// The OS's mount points as the guard's folders are kept (canonical: /var is /private/var on the Mac).
drives::listing listed(drives::port& p) {
  drives::listing l = p.list();
  for (auto& d : l.disks) {
    for (auto& v : d.volumes) {
      if (!v.mount.empty()) v.mount = canonical_or(v.mount);
    }
  }
  return l;
}

struct ctx {
  const args& a;
  const host_layout& l;
  bool test_hooks;
  drives_run run;
  std::unique_ptr<drives::port> port;
  drives::guard g;
};

// After the names are typed (which may take a while): every drive listed again, still the one
// confirmed, and still passing every guard (a shared folder or a catalogue may have appeared).
std::string still_ok(ctx& c, std::span<const drives::disk> confirmed) {
  const drives::listing now = listed(*c.port);
  if (!now.error.empty()) return now.error;
  for (const auto& d : confirmed) {
    const auto it = std::find_if(now.disks.begin(), now.disks.end(), [&](const drives::disk& x) { return x.id == d.id; });
    if (it == now.disks.end() || !drives::same_disk(d, *it)) return d.name + " changed since it was confirmed";
    if (const std::string why = drives::refusal_of(*it, c.g); !why.empty()) return d.name + ": " + why;
  }
  return {};
}

int list_cmd(ctx& c) {
  const drives::listing l = listed(*c.port);
  if (c.a.json) {
    print(listing_json(l, c.g) + "\n");
    return l.error.empty() ? 0 : 1;
  }
  if (!l.error.empty()) return fail(l.error + ".", 1);
  std::string out;
  for (const auto& d : l.disks) {
    out += "  " + d.id + std::string(10 - std::min<std::size_t>(9, d.id.size()), ' ') + drives::facts_line(d) + "\n";
    if (const std::string why = drives::refusal_of(d, c.g); !why.empty()) out += "            cannot be erased: " + why + "\n";
  }
  if (l.disks.empty()) out += "  no drives\n";
  if (!l.sets.empty()) {
    out += "\n";
    for (const auto& s : l.sets) out += "  " + drives::health_line(s) + "\n";
  }
  if (!l.pool_ok && drives::this_os() == drives::os_kind::windows) {
    out += "\nStorage Spaces is not available on this PC: drives can be shared one by one, not combined.\n";
  }
  out += "\ndriftwire host drives choices DRIVE... shows what each choice gives for those drives.\n";
  print(out);
  return 0;
}

std::vector<drives::disk> picked(const drives::listing& l, std::span<const std::string> ids, std::string& error) {
  std::vector<drives::disk> out;
  for (const auto& id : ids) {
    const auto it = std::find_if(l.disks.begin(), l.disks.end(), [&](const drives::disk& d) { return d.id == id; });
    if (it == l.disks.end()) {
      error = "there is no drive " + id + " (driftwire host drives list shows them)";
      return {};
    }
    out.push_back(*it);
  }
  return out;
}

int choices_cmd(ctx& c, const drives_words& w) {
  const drives::listing l = listed(*c.port);
  if (!l.error.empty()) return fail(l.error + ".", 1);
  std::string error;
  const auto ds = picked(l, w.ids, error);
  if (!error.empty()) return fail(error + ".", 2);
  std::vector<std::uint64_t> sizes;
  for (const auto& d : ds) sizes.push_back(d.size);
  const auto opts = drives::options_for(sizes, drives::this_os(), l.pool_ok);
  print(c.a.json ? choices_json(opts) + "\n" : choices_text(opts, 80));
  return 0;
}

// Each drive's name, typed. False when one does not match (nothing is erased then).
bool ask_names(ctx& c, const drives::plan& p, std::vector<std::string>& typed) {
  for (const auto& d : p.disks) {
    if (!c.a.confirm_stdin) print("Type the drive's name to erase it (" + d.name + "): ");
    std::string line;
    const bool got = c.run.read_line ? c.run.read_line(line) : read_stdin_line(line);
    if (!got) return false;
    typed.push_back(line);
  }
  return drives::confirmed(p, typed);
}

std::string share_volume(ctx& c, const std::string& mount, const std::string& label) {
  std::string shared_as;
  const std::string why = host_files::share(c.l, mount, label, false, 0, c.test_hooks, shared_as);
  return why.empty() ? "shared as \"" + shared_as + "\"" : "not shared: " + why;
}

int combine_cmd(ctx& c, const drives_words& w) {
  if (!c.a.confirm_stdin && !c.run.read_line && !serve_stdin_is_terminal()) {
    return fail("it asks for each drive's name at a terminal (or give --confirm-stdin).", 2);
  }
  const drives::listing l = listed(*c.port);
  if (!l.error.empty()) return fail(l.error + ".", 1);
  const std::string label = w.c == drives::choice::separate ? std::string() : c.a.label;
  const drives::plan p = drives::make_plan(w.c, label, w.ids, l.disks, c.g, drives::this_os(), l.pool_ok, l.sets);
  if (!p.error.empty()) return fail(p.error + ".", 2);
  std::string out;
  for (const auto& d : p.disks) out += "  " + drives::facts_line(d) + "\n";
  if (!p.refusals.empty()) {
    for (const auto& r : p.refusals) out += "  refused: " + r + "\n";
    print(out);
    return fail(w.c == drives::choice::separate ? "nothing was shared." : "nothing was erased.", 1);
  }
  if (w.c == drives::choice::separate) {
    for (const auto& d : p.disks) {
      for (const auto& v : d.volumes) {
        if (!v.mount.empty()) out += "  " + (v.label.empty() ? d.name : v.label) + ": " + share_volume(c, v.mount, v.label) + "\n";
      }
    }
    print(out);
    return 0;
  }
  std::vector<std::uint64_t> sizes;
  for (const auto& d : p.disks) sizes.push_back(d.size);
  const auto opts = drives::options_for(sizes, drives::this_os(), l.pool_ok);
  const auto& o = opts[static_cast<std::size_t>(p.c)];
  out += "\n" + o.title + ": " + o.space_words + "\n  If one drive dies: " + o.dead_words + "\n\n";
  out += std::string(drives::not_a_backup_text()) + "\n\n";
  out += "EVERYTHING on " + std::string(p.disks.size() == 2 ? "both drives" : "these drives") +
         " is erased, and cannot be brought back.\n";
  print(out);
  std::vector<std::string> typed;
  if (!ask_names(c, p, typed)) return fail("a name did not match; nothing was erased.", 1);
  if (const std::string why = still_ok(c, p.disks); !why.empty()) return fail(why + "; nothing was erased.", 1);
  print(c.run.elevate ? "Asking for an administrator (the system's own prompt)...\n" : "Combining...\n");
  const drives::run_result r = c.port->create(p, c.run.elevate);
  if (!r.ok) return fail(r.why + ".", 1);
  std::string done = "Made \"" + p.label + "\" (" + drives::choice_word(p.c) + ", " + drives::size_words(p.space) + ")";
  if (!r.mount.empty()) {
    done += "; " + share_volume(c, r.mount, p.label);
  } else {
    done += "; share it with driftwire host share once it is mounted";
  }
  print(done + ".\n");
  return 0;
}

int replace_cmd(ctx& c, const drives_words& w) {
  if (!c.a.confirm_stdin && !c.run.read_line && !serve_stdin_is_terminal()) {
    return fail("it asks for the new drive's name at a terminal (or give --confirm-stdin).", 2);
  }
  const drives::listing l = listed(*c.port);
  if (!l.error.empty()) return fail(l.error + ".", 1);
  const auto s = std::find_if(l.sets.begin(), l.sets.end(), [&](const drives::set_health& x) { return x.name == w.set || x.id == w.set; });
  if (s == l.sets.end()) return fail("there is no combined drive called \"" + w.set + "\".", 2);
  if (s->kind != drives::choice::mirror && s->kind != drives::choice::parity) {
    return fail("a " + std::string(drives::choice_word(s->kind)) + " has nothing to rebuild from: its files on the dead drive are gone.", 1);
  }
  std::string error;
  const auto fresh = picked(l, w.ids, error);
  if (!error.empty()) return fail(error + ".", 2);
  const drives::disk& d = fresh.front();
  std::string out = "  " + drives::health_line(*s) + "\n";
  for (const auto& m : s->members) {
    if (m.missing) out += "  missing or failed: " + (m.id.empty() ? std::string("a drive no longer attached") : m.id) + " (" + m.status + ")\n";
  }
  out += "  new drive: " + drives::facts_line(d) + "\n";
  if (const std::string why = drives::refusal_of(d, c.g); !why.empty()) {
    print(out);
    return fail("refused: " + d.name + ": " + why + "; nothing was erased.", 1);
  }
  out += "\nEVERYTHING on " + d.name + " is erased; the set then rebuilds onto it (hours for a big drive).\n";
  print(out);
  drives::plan p;
  p.c = drives::choice::mirror;
  p.disks = {d};
  std::vector<std::string> typed;
  if (!ask_names(c, p, typed)) return fail("the name did not match; nothing was erased.", 1);
  if (const std::string why = still_ok(c, p.disks); !why.empty()) return fail(why + "; nothing was erased.", 1);
  print(c.run.elevate ? "Asking for an administrator (the system's own prompt)...\n" : "Replacing...\n");
  const drives::run_result r = c.port->replace(*s, d, c.run.elevate);
  if (!r.ok) return fail(r.why + ".", 1);
  print("Added " + d.name + " to \"" + s->name + "\"; it is rebuilding (driftwire host status shows how far).\n");
  return 0;
}

}  // namespace

int run_host_drives(const args& a, const host_layout& l, const std::string& data_dir, bool test_hooks, drives_run run) {
  const drives_words w = parse_drives_words(a.paths);
  if (!w.error.empty()) return fail(w.error + ".", 2);
  if (w.act == drives_action::combine && w.c != drives::choice::separate && a.label.empty()) {
    return fail("combine needs --label NAME: the combined drive's name.", 2);
  }
  if (test_hooks) run.elevate = false;  // the test build touches its own images only (the guard)
  ctx c{a, l, test_hooks, std::move(run), nullptr, {}};
  c.port = c.run.make_port ? c.run.make_port() : drives::os_port();
  c.g = host_guard(l, data_dir, test_hooks, a.test_data);
  switch (w.act) {
    case drives_action::list: return list_cmd(c);
    case drives_action::choices: return choices_cmd(c, w);
    case drives_action::combine: return combine_cmd(c, w);
    case drives_action::replace: return replace_cmd(c, w);
  }
  return 2;
}

// ---- the keeper -----------------------------------------------------------------------------

namespace {

class drives_keeper final : public host_keeper {
 public:
  explicit drives_keeper(drives_keeper_config c) : c_(std::move(c)) {}
  ~drives_keeper() override { stop(); }

  void start(const keeper_context& ctx) override {
    if (ctx.loopback_only && !c_.on_loopback) return;
    {
      std::lock_guard g(m_);
      ctx_ = ctx;
      stopping_ = false;
    }
    t_ = std::thread([this] { loop(); });
  }

  void stop() noexcept override {
    {
      std::lock_guard g(m_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (t_.joinable()) t_.join();
  }

  void ports_changed(const keeper_context& ctx) noexcept override {
    std::lock_guard g(m_);
    ctx_.storage_changed = ctx.storage_changed;
  }

  std::string status_line() const override {
    std::lock_guard g(m_);
    return line_;
  }

  std::vector<std::pair<std::string, std::int64_t>> state_numbers() const override {
    std::lock_guard g(m_);
    if (count_ == 0) return {};
    return {{"drives_sets", static_cast<std::int64_t>(count_)}, {"drives_health", static_cast<std::int64_t>(worst_)}};
  }

 private:
  void loop() {
    auto port = c_.make_port ? c_.make_port() : drives::os_port();
    std::unique_lock lock(m_);
    while (!stopping_) {
      lock.unlock();
      drives::listing l = port->sets_only();
      if (!l.error.empty()) {
        // The tools could not be asked this time: what was known stands, and nothing is said twice.
        lock.lock();
        cv_.wait_for(lock, std::chrono::milliseconds(c_.every_ms), [this] { return stopping_; });
        continue;
      }
      // A set that was there and is gone from the OS's list (every drive of it unplugged) is offline, not healthy.
      for (const auto& [id, known] : known_) {
        const bool listed = std::any_of(l.sets.begin(), l.sets.end(), [&](const drives::set_health& s) { return s.id == id; });
        if (!listed) {
          drives::set_health gone = known;
          gone.state = drives::health::offline;
          gone.members.clear();
          l.sets.push_back(std::move(gone));
        }
      }
      std::vector<std::string> notes;
      for (const auto& s : l.sets) {
        const auto before = last_.find(s.id);
        const std::string words = drives::change_words(s, before == last_.end() ? drives::health::healthy : before->second);
        if (!words.empty()) notes.push_back(words);
      }
      for (const auto& n : notes) port->notify("MediaViewer", n);
      lock.lock();
      last_.clear();
      for (const auto& s : l.sets) {
        last_[s.id] = s.state;
        known_[s.id] = s;
      }
      line_ = drives_status_line(l.sets);
      count_ = l.sets.size();
      worst_ = drives::worst(l.sets);
      const std::string word = count_ == 0 || worst_ == drives::health::healthy ? std::string() : drives::health_word(worst_);
      if (word != storage_ && ctx_.storage_changed) ctx_.storage_changed(word);
      storage_ = word;
      cv_.wait_for(lock, std::chrono::milliseconds(c_.every_ms), [this] { return stopping_; });
    }
  }

  drives_keeper_config c_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::thread t_;
  bool stopping_ = false;
  keeper_context ctx_;
  std::string line_, storage_;
  std::size_t count_ = 0;
  drives::health worst_ = drives::health::healthy;
  std::map<std::string, drives::health> last_;
  std::map<std::string, drives::set_health> known_;  // every set seen while the host runs (one deleted on purpose stays offline until a restart)
};

}  // namespace

std::unique_ptr<host_keeper> make_drives_keeper(drives_keeper_config c) { return std::make_unique<drives_keeper>(std::move(c)); }

}  // namespace mv::driftwire::cli
