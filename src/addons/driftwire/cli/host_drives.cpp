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
#include "addons/driftwire/cli/drive_copier.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/host_copy.h"
#include "addons/driftwire/cli/serve_tty.h"
#include "addons/driftwire/cli/volume.h"
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
  } else if (what == "copy" && rest.size() == 2) {
    d.act = drives_action::copy;
    d.ids.assign(rest.begin(), rest.end());
  } else if (what == "copy-stop" && rest.size() == 1) {
    d.act = drives_action::copy_stop;
    d.set = rest[0];
  } else {
    d.error = "host drives takes list, choices DRIVE..., combine separate|mirror|span|stripe|parity DRIVE... "
              "--label NAME, replace SET DRIVE, copy FROM TO, or copy-stop FROM";
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

namespace {

std::uint64_t free_of(const drives::disk& d) {
  std::uint64_t free = 0;
  for (const auto& v : d.volumes) {
    if (!v.mount.empty() && v.size > v.used) free += v.size - v.used;
  }
  return free;
}

const drives::volume_facts* mounted_volume(const drives::disk& d) {
  for (const auto& v : d.volumes) {
    if (!v.mount.empty()) return &v;
  }
  return nullptr;
}

bool same_path(const std::string& a, const std::string& b) {
  return drives::path_within(a, b, drives::this_os()) && drives::path_within(b, a, drives::this_os());
}

// The disk whose mounted volume is at `mount` ("": none listed).
std::string disk_at(const drives::listing& l, const std::string& mount) {
  if (mount.empty()) return {};
  for (const auto& d : l.disks) {
    for (const auto& v : d.volumes) {
      if (!v.mount.empty() && same_path(v.mount, mount)) return d.id;
    }
  }
  return {};
}

// Where a copy's volume is now ("": not attached). POSIX: a volume's root is a mount point, so a
// folder an unplugged drive left behind is not it. `plain`: the test build, whose disk images are
// mounted on folders it made (the guard keeps it to those).
std::string where_now(const std::string& path, const std::string& volume, bool plain) {
  serve::root_config r;
  r.path = path;
  r.volume = volume;
#if !defined(_WIN32)
  r.mount = !plain && path != "/";
#else
  (void)plain;
#endif
  return volume_locate(r).value_or(std::string());
}

}  // namespace

std::string listing_json(const drives::listing& l, const drives::guard& g, const std::string& host_folder) {
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
    w.key("free").integer(static_cast<std::int64_t>(free_of(d))).key("mounted").boolean(mounted_volume(d) != nullptr);
    w.key("used_words").string(drives::size_words(drives::used_of(d)));
    w.key("free_words").string(drives::size_words(free_of(d)));
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
  // The copies kept (host_copy.h): each with its drives' ids while they are attached, and how it stands.
  w.key("copies").begin_array();
  if (!host_folder.empty()) {
    const auto statuses = read_copy_statuses(host_folder);
    for (const auto& c : read_copies(host_folder)) {
      copy_status st;
      st.label = c.label;
      for (const auto& x : statuses) {
        if (x.label == c.label) st = x;
      }
      st.to_name = c.to_name;
      w.begin_object().key("label").string(c.label).key("to_name").string(c.to_name);
      const bool plain = !g.images_under.empty();
      w.key("from_id").string(disk_at(l, where_now(c.from, c.from_volume, plain)));
      w.key("to_id").string(disk_at(l, where_now(c.to, c.to_volume, plain)));
      w.key("state").string(copy_state_word(st.state));
      w.key("line").string(copy_line(st, drives::this_os() == drives::os_kind::mac)).end_object();
    }
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
      if (!v.mount.empty() && drive_copy::has_marker(v.mount)) d.copy_drive = true;
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
    print(listing_json(l, c.g, c.l.folder) + "\n");
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

// A copy's folder is named after the source: nothing a folder name can't hold.
std::string folder_safe(std::string label) {
  for (char& ch : label) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
        ch == '|' || static_cast<unsigned char>(ch) < 0x20) {
      ch = '-';
    }
  }
  while (!label.empty() && (label.back() == '.' || label.back() == ' ')) label.pop_back();
  return label.empty() ? std::string("Drive") : label;
}

int copy_cmd(ctx& c, const drives_words& w) {
  const drives::listing l = listed(*c.port);
  if (!l.error.empty()) return fail(l.error + ".", 1);
  std::string error;
  const auto ds = picked(l, w.ids, error);
  if (!error.empty()) return fail(error + ".", 2);
  const drives::disk& from = ds[0];
  const drives::disk& to = ds[1];
  if (from.id == to.id) return fail("the copy goes on another drive than the one copied.", 2);
  const drives::volume_facts* fv = mounted_volume(from);
  const drives::volume_facts* tv = mounted_volume(to);
  if (!fv) return fail(from.name + " isn't mounted, so nothing on it can be read: mount it first.", 1);
  if (!tv) return fail(to.name + " isn't mounted: mount it first. Nothing on it is erased.", 1);
  if (!c.g.images_under.empty()) {
    for (const auto* d : {&from, &to}) {
      if (!d->image || !drives::path_within(d->image_path, c.g.images_under, c.g.os)) {
        return fail("the test build touches only disk images made for its tests.", 1);
      }
    }
  }
  if (to.system) return fail(to.name + " holds this computer's system: choose another drive for the copy.", 1);
  if (to.read_only) return fail(to.name + " is read-only.", 1);
  if (from.copy_drive) return fail(from.name + " is itself a copy of another drive.", 1);
  for (const auto& p : c.g.protected_paths) {
    if (drives::path_within(p, tv->mount, c.g.os)) return fail(to.name + " holds MediaViewer's library or data: choose another drive.", 1);
  }
  for (const auto& p : c.g.shared_paths) {
    if (drives::path_within(p, tv->mount, c.g.os)) {
      return fail(to.name + " holds a folder this server shares, so the copy would be shared twice: take it back "
                  "with `driftwire host unshare LABEL` first, or choose another drive.", 1);
    }
  }
  const std::string label = folder_safe(fv->label.empty() ? from.name : fv->label);
  std::vector<copy_entry> copies = read_copies(c.l.folder);
  for (const auto& e : copies) {
    if (e.label == label || same_path(where_now(e.from, e.from_volume, c.test_hooks), fv->mount)) {
      return fail("a copy of " + from.name + " is kept already (on " + e.to_name + ").", 1);
    }
    if (same_path(where_now(e.to, e.to_volume, c.test_hooks), tv->mount)) {
      return fail(to.name + " keeps a copy of " + e.label + " already.", 1);
    }
  }
  if (to.copy_drive) {
    const auto m = drive_copy::read_marker(tv->mount);
    if (!m || m->label != label) return fail(to.name + " holds a copy of another drive.", 1);
  }
  const std::uint64_t needed = drives::used_of(from), room = free_of(to);
  if (needed > room) {
    return fail("the copy doesn't fit: " + from.name + " has " + drives::size_words(needed) + " on it, " + to.name +
                " has " + drives::size_words(room) + " free. Nothing was changed.", 1);
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(c.l.folder.data()), c.l.folder.size())), ec);
  if (!drive_copy::write_marker(tv->mount, {label, 0})) return fail("couldn't write to " + to.name + ".", 1);
  copies.push_back({label, to.name, fv->mount, volume_of(fv->mount).volume, tv->mount, volume_of(tv->mount).volume});
  if (!write_copies(c.l.folder, copies)) {
    drive_copy::remove_marker(tv->mount);
    return fail("couldn't save the copy's settings.", 1);
  }
  print("Keeping a copy of \"" + label + "\" on " + to.name + ", in the folder \"" + drive_copy::copy_folder_name(label) +
        "\". Nothing on " + from.name + " is changed or erased.\nThe first copy (" + drives::size_words(needed) +
        ") runs while the server runs; driftwire host status shows how far it is.\n");
  return 0;
}

int copy_stop_cmd(ctx& c, const drives_words& w) {
  const drives::listing l = listed(*c.port);
  std::vector<copy_entry> copies = read_copies(c.l.folder);
  const auto it = std::find_if(copies.begin(), copies.end(), [&](const copy_entry& e) {
    return e.label == w.set || (l.error.empty() && disk_at(l, where_now(e.from, e.from_volume, c.test_hooks)) == w.set);
  });
  if (it == copies.end()) return fail("no copy of \"" + w.set + "\" is kept.", 2);
  const copy_entry e = *it;
  copies.erase(it);
  if (!write_copies(c.l.folder, copies)) return fail("couldn't save the copy's settings.", 1);
  if (const std::string to = where_now(e.to, e.to_volume, c.test_hooks); !to.empty()) drive_copy::remove_marker(to);
  print("Stopped keeping a copy of \"" + e.label + "\". What was copied stays on " + e.to_name + ", in \"" +
        drive_copy::copy_folder_name(e.label) + "\".\n");
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
    case drives_action::copy: return copy_cmd(c, w);
    case drives_action::copy_stop: return copy_stop_cmd(c, w);
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
