// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/open_json.h"

#include <vector>

#include "addon/package.h"
#include "addon/settings_values.h"
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
      return name + " contains native code. MediaViewer runs code from other makers only as "
                    "a script, inside its limits.";
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
  std::string adds;
  const auto add = [&adds](const std::string& part) {
    if (!adds.empty()) adds += ", ";
    adds += part;
  };
  if (!m.themes.empty()) add(counted(m.themes.size(), "theme", "themes"));
  if (!m.settings.empty()) add("a settings page");
  if (!m.keymaps.empty()) add(counted(m.keymaps.size(), "key layout", "key layouts"));
  if (!m.commands.empty()) add(counted(m.commands.size(), "command", "commands"));
  if (adds.empty()) adds = m.runs_code() ? "A script" : "Nothing this MediaViewer uses";
  w.key("adds").string(adds);
  w.key("can").begin_array();
  if (!m.themes.empty()) w.string("Change how MediaViewer's bars, panes and text look");
  if (!m.settings.empty()) w.string("Add a page of its own settings to Settings");
  if (!m.keymaps.empty()) w.string("Offer other keys for MediaViewer's commands, when you choose one");
  // One line per thing the script may reach, from the manifest (docs/design/25
  // §5: computed, never the publisher's words).
  if (m.runs_code()) {
    w.string("Run its own script inside MediaViewer, stopped if it uses too much memory or time");
  }
  if (!m.commands.empty()) w.string("Add commands you can run from the keyboard");
  if (m.permissions & perm_folder_read) w.string("See the names of the files in the open folder");
  if (m.permissions & perm_selection) {
    w.string("See which file is on screen, and the marked files when you run its command");
  }
  if (m.permissions & perm_thumbnails) w.string("Show thumbnails of your photos and videos");
  w.end_array();
  // What it cannot do. Nothing grants the network or a change to a file under
  // API 3, so those lines are always here.
  std::vector<std::string> cannot;
  if (!m.runs_code()) cannot.emplace_back("run code");
  if (!(m.permissions & perm_folder_read)) cannot.emplace_back("read your files");
  if (m.runs_code()) cannot.emplace_back("change your files");
  cannot.emplace_back("use the network");
  std::string line;
  for (std::size_t i = 0; i < cannot.size(); ++i) {
    if (i > 0) line += cannot.size() > 2 ? (i + 1 == cannot.size() ? ", or " : ", ") : " or ";
    line += cannot[i];
  }
  line[0] = static_cast<char>(line[0] - 'a' + 'A');
  w.key("cannot").string(line);
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
  // API 2 (PR 56): whether Settings has a page for it, and its key layouts.
  w.key("settings").boolean(!m.settings.empty());
  w.key("keymaps").begin_array();
  for (const keymap_pack& k : m.keymaps) {
    w.begin_object().key("id").string(k.id).key("name").string(k.name).end_object();
  }
  w.end_array();
  // API 3: code.
  w.key("script").boolean(m.runs_code());
  w.key("permissions").begin_array();
  for (std::uint32_t bit = 1; bit <= kPermissionsAll; bit <<= 1) {
    if (m.permissions & bit) w.string(permission_name(bit));
  }
  w.end_array();
  w.key("commands").begin_array();
  for (const script_command& c : m.commands) {
    w.begin_object().key("id").string(c.id).key("name").string(c.name).key("key").string(c.key)
        .end_object();
  }
  w.end_array();
}

}  // namespace

namespace {

// The page of add-on `id`, open or first-party; empty when it is not
// installed and verified. `name` gets the add-on's name, `publisher` its
// publisher's key ("" for a first-party add-on: the app's own).
settings_page page_of(const open_store& open, const store* first_party, std::string_view id,
                      std::string* name, std::string* publisher, bool* found) {
  *found = false;
  publisher->clear();
  if (valid_open_id(id)) {
    auto installed = open.find(id);
    if (!installed || installed->state != install_state::ok) return {};
    *found = true;
    if (name) *name = installed->m.name;
    *publisher = installed->m.publisher_key;
    return installed->m.settings;
  }
  if (!first_party) return {};
  auto installed = first_party->find(std::string(id));
  if (!installed || installed->state != install_state::ok) return {};
  *found = true;
  if (name) *name = installed->m.name;
  return installed->m.settings;
}

}  // namespace

std::string addon_settings_page_json(const open_store& open, const store* first_party,
                                     std::string_view id) {
  auto values = default_settings_values();
  if (!values || !valid_settings_owner(id)) return {};
  std::string name, publisher;
  bool found = false;
  const settings_page page = page_of(open, first_party, id, &name, &publisher, &found);
  if (!found || page.empty()) return {};
  return values->page_json(id, publisher, page, name);
}

std::string addon_setting_set_json(const open_store& open, const store* first_party,
                                   std::string_view id, std::string_view key,
                                   std::string_view value_json) {
  const auto answer = [](bool ok, std::string_view message) {
    json::writer w;
    w.begin_object().key("ok").boolean(ok).key("message").string(message).end_object();
    return w.take();
  };
  auto values = default_settings_values();
  bool found = false;
  std::string publisher;
  const settings_page page =
      values && valid_settings_owner(id)
          ? page_of(open, first_party, id, nullptr, &publisher, &found)
          : settings_page{};
  if (!found) return answer(false, "That add-on is not installed.");
  if (!page.find(key)) return answer(false, "That setting is no longer on the add-on's page.");
  if (auto set = values->set(id, publisher, page, key, value_json); !set) {
    return answer(false, set.error() == status::invalid_arg
                             ? "That value is not one this setting takes."
                             : "MediaViewer could not save the setting.");
  }
  return answer(true, "");
}

bool addon_settings_forget(std::string_view id) {
  auto values = default_settings_values();
  return values && values->forget(id).has_value();
}

std::string open_keymap_json(const open_store& open, std::string_view addon_id,
                             std::string_view pack_id) {
  auto installed = open.find(addon_id);
  if (!installed || installed->state != install_state::ok) return {};
  for (const keymap_pack& p : installed->m.keymaps) {
    if (p.id != pack_id) continue;
    json::writer w;
    w.begin_object();
    w.key("addon").string(installed->m.id);
    w.key("id").string(p.id);
    w.key("name").string(p.name);
    w.key("bindings").begin_array();
    for (const keymap_binding& b : p.bindings) {
      w.begin_object().key("command").string(b.command).key("keys").begin_array();
      for (const std::string& k : b.keys) w.string(k);
      w.end_array().end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
  }
  return {};
}

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
    // Settings kept from another publisher's add-on under this id are not
    // this one's to read (settings_values.h). A failure leaves them unread.
    if (auto values = default_settings_values()) {
      (void)values->claim(installed->id, installed->m.publisher_key);
    }
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
