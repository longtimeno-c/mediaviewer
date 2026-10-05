// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/open_json.h"

#include <vector>

#include "addon/package.h"
#include "core/json.h"
#include "io/file_port.h"
#include "io/paths.h"

namespace mv::addon {
namespace {

const char* state_name(install_state s) noexcept {
  switch (s) {
    case install_state::ok: return "ok";
    case install_state::needs_update: return "needs_update";
    default: return "invalid";
  }
}

// The whole package, read once. `why` is too_large when it is over the cap
// and bad_package when it cannot be read.
bool read_package_file(std::string_view path, std::vector<std::uint8_t>& out, rejection& why) {
  why = rejection::bad_package;
  auto st = io::stat_path(path);
  if (!st || st->is_directory) return false;
  if (st->size > kPackageMaxBytes) {
    why = rejection::too_large;
    return false;
  }
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential)) return false;
  out.resize(static_cast<std::size_t>(st->size));
  std::size_t have = 0;
  while (have < out.size()) {
    auto got = in.read(std::span<std::uint8_t>(out.data() + have, out.size() - have));
    if (!got) return false;
    if (*got == 0) break;
    have += *got;
  }
  // A file that shrank or grew while it was read is not the file to trust.
  if (have != out.size()) return false;
  std::uint8_t more = 0;
  if (auto extra = in.read(std::span<std::uint8_t>(&more, 1)); !extra || *extra != 0) return false;
  return true;
}

// What the person installing reads when a package is refused. One wording
// for both hosts. Never a path outside the package, never a hash.
std::string refusal_text(rejection why, const std::string& detail, const open_manifest& m,
                         const std::string& installed_version) {
  const std::string name = m.name.empty() ? std::string("This add-on") : m.name;
  switch (why) {
    case rejection::none:
      return {};
    case rejection::too_large:
      return "This add-on is larger than 64 MB, the most MediaViewer installs from a file or a link.";
    case rejection::bad_package:
      return "This file is not a MediaViewer add-on, or it was not packed the way MediaViewer "
             "reads one.";
    case rejection::missing_signature:
      return "This add-on is not signed, so nothing says its files are the ones its maker packed.";
    case rejection::bad_signature:
      return "This add-on's signature does not match it. It was changed after its maker signed it.";
    case rejection::file_missing:
    case rejection::file_mismatch:
    case rejection::unexpected_file:
      return "This add-on's files are not the ones its maker signed.";
    case rejection::unsafe_path:
      return "This add-on names a file outside its own folder.";
    case rejection::code_not_allowed:
      return name + " contains code. MediaViewer installs add-ons from other makers only when "
                    "they are data, such as themes.";
    case rejection::needs_update:
      return name + " needs a newer MediaViewer.";
    case rejection::other_publisher:
      return name + " is already installed from a different maker. Remove the installed one "
                    "first if this is the one you trust.";
    case rejection::downgrade:
      return name + " " + installed_version + " is installed, and this is the older " +
             m.version + ".";
    case rejection::invalid_theme: {
      const bool contrast = detail.find("low_contrast") != std::string::npos;
      const std::string theme_id = detail.substr(0, detail.find(':'));
      return "The theme \"" + theme_id + "\" in " + name +
             (contrast ? " has text too close in colour to its background to read."
                       : " is not a theme MediaViewer can read.");
    }
    case rejection::changed:
      return "The file changed after it was checked, so nothing was installed.";
    default:
      return "This add-on's description is not one MediaViewer can read.";
  }
}

std::string counted(std::size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The sheet's "Adds / Can / Cannot" lines, from what the manifest holds and
// never from what its maker wrote about it (docs/design/25 "Identity and trust").
void write_abilities(json::writer& w, const open_manifest& m) {
  w.key("adds").string(m.themes.empty() ? std::string("Nothing this MediaViewer uses")
                                        : counted(m.themes.size(), "theme", "themes"));
  w.key("can").begin_array();
  if (!m.themes.empty()) w.string("Change how MediaViewer's bars, panes and text look");
  w.end_array();
  // Contribution API 1 is data: true of every add-on this host installs.
  w.key("cannot").string("Run code, read your files, or use the network");
}

void write_addon(json::writer& w, const open_manifest& m) {
  write_abilities(w, m);
  w.key("id").string(m.id);
  w.key("name").string(m.name);
  w.key("version").string(m.version);
  w.key("description").string(m.description);
  w.key("licence").string(m.licence);
  w.key("size").integer(static_cast<std::int64_t>(m.installed_size));
  w.key("update_url").string(m.update_url);
  w.key("publisher").begin_object();
  w.key("name").string(m.publisher_name);
  w.key("url").string(m.publisher_url);
  w.key("key").string(m.publisher_key);
  w.key("fingerprint").string(key_fingerprint(m.publisher_key));
  w.end_object();
  w.key("api").begin_object();
  w.key("min").integer(m.api_min);
  w.key("max").integer(m.api_max);
  w.end_object();
  w.key("themes").begin_array();
  for (const theme_ref& t : m.themes) {
    w.begin_object().key("id").string(t.id).key("name").string(t.name).end_object();
  }
  w.end_array();
}

}  // namespace

result<open_store> default_open_store() {
  MV_TRY(std::string root, io::open_addons_dir());
  return open_store(std::move(root));
}

std::string open_inspect_json(const open_store& store, std::string_view package_path) {
  json::writer w;
  w.begin_object();
  std::vector<std::uint8_t> package;
  rejection why = rejection::bad_package;
  if (!read_package_file(package_path, package, why)) {
    w.key("ok").boolean(false);
    w.key("why").string(rejection_name(why));
    w.key("message").string(refusal_text(why, {}, {}, {}));
    w.end_object();
    return w.take();
  }
  const package_offer offer = store.inspect(package);
  w.key("ok").boolean(offer.ok());
  w.key("why").string(rejection_name(offer.why));
  w.key("detail").string(offer.detail);
  w.key("sha256").string(offer.sha256);
  w.key("message").string(
      refusal_text(offer.why, offer.detail, offer.m, offer.installed_version));
  w.key("relation").string(relation_name(offer.relation));
  w.key("installed_version").string(offer.installed_version);
  if (!offer.m.publisher_key.empty()) write_addon(w, offer.m);
  w.end_object();
  return w.take();
}

std::string open_install_json(const open_store& store, std::string_view package_path,
                              std::string_view approved_sha256) {
  json::writer w;
  w.begin_object();
  std::vector<std::uint8_t> package;
  rejection why = rejection::bad_package;
  if (!read_package_file(package_path, package, why)) {
    w.key("ok").boolean(false);
    w.key("why").string(rejection_name(why));
    w.key("message").string(refusal_text(why, {}, {}, {}));
    w.end_object();
    return w.take();
  }
  why = rejection::none;
  auto installed = store.install(package, approved_sha256, &why);
  w.key("ok").boolean(installed.has_value());
  // A refusal with no rule behind it is the disk's: the folder could not be
  // written.
  w.key("why").string(installed ? "ok" : why == rejection::none ? "io" : rejection_name(why));
  w.key("message").string(
      installed ? std::string()
                : why == rejection::none
                      ? std::string("MediaViewer could not write to its add-ons folder.")
                      : refusal_text(why, {}, {}, {}));
  if (installed) {
    w.key("id").string(installed->id);
    w.key("version").string(installed->version);
  }
  w.end_object();
  return w.take();
}

std::string open_list_json(const open_store& store) {
  json::writer w;
  w.begin_array();
  for (const open_installed& i : store.list()) {
    w.begin_object();
    w.key("state").string(state_name(i.state));
    w.key("why").string(rejection_name(i.why));
    // The folder it sits in: what remove takes, whatever its manifest says.
    w.key("folder").string(i.id);
    if (i.m.publisher_key.empty()) {
      // Nothing about it could be trusted enough to show but where it sits.
      w.key("id").string(i.id);
      w.key("name").string(i.id);
      w.key("version").string(i.version);
    } else {
      write_addon(w, i.m);
    }
    w.end_object();
  }
  w.end_array();
  return w.take();
}

}  // namespace mv::addon
