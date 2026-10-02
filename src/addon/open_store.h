// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Installed open add-ons (plan/23 "Identity and trust", "Install, update,
// remove"): add-ons from other makers, each signed by its publisher.
//
//   <root>/<id>/<version>/     manifest.json, manifest.json.sig, the files
//   <root>/<id>/publisher.json the key the user agreed to, written by the app
//   <root>/.staging/<n>/       a package being unpacked
//
// <root> is io::open_addons_dir(), beside the first-party add-ons folder and
// never inside it: store.h lists MediaViewer's own add-ons under MediaViewer's
// key, this lists everyone else's under theirs, and neither reads the other's
// folder. The folder does not exist until the first install.
//
// A publisher's key is pinned per add-on when the user first agrees to it.
// From then on only that key installs under that id, and every listing
// re-checks the signature, the key and every file. A folder put there by hand
// has no publisher.json and is never loaded.
//
// Worker threads only.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/open_manifest.h"
#include "addon/store.h"
#include "core/result.h"

namespace mv::addon {

struct open_installed {
  std::string id;
  std::string version;
  std::string dir;  // the version folder
  install_state state = install_state::invalid;
  rejection why = rejection::none;
  open_manifest m;
};

enum class package_relation : std::uint8_t {
  fresh = 0,        // nothing under this id
  update,           // newer than the installed version, same publisher
  repair,           // the installed version again, same publisher
  downgrade,        // older than the installed version: refused
  other_publisher,  // the id is installed under another key: refused
};

[[nodiscard]] const char* relation_name(package_relation r) noexcept;

// What a package is, before anything is written. `m` is filled whenever the
// publisher's signature held, so a refusal can still say whose add-on it was.
struct package_offer {
  rejection why = rejection::bad_package;
  std::string detail;  // which file or theme, and the theme's fault; never a path outside the package
  open_manifest m;
  std::string sha256;  // of the whole package: what install() must be handed back
  package_relation relation = package_relation::fresh;
  std::string installed_version;
  [[nodiscard]] bool ok() const noexcept { return why == rejection::none; }
};

class open_store {
 public:
  explicit open_store(std::string root, std::uint32_t api = kContributionApi);

  // The newest version of each add-on, verified. Empty when the folder does
  // not exist; nothing is created.
  [[nodiscard]] std::vector<open_installed> list() const;
  [[nodiscard]] result<open_installed> find(std::string_view id) const;

  // Reads nothing but `package` and what is installed.
  [[nodiscard]] package_offer inspect(std::span<const std::uint8_t> package) const;

  // Installs the package the user agreed to: `approved_sha256` is the offer's.
  // On refusal `why` (when given) says which rule, as inspect() would.
  [[nodiscard]] result<open_installed> install(std::span<const std::uint8_t> package,
                                               std::string_view approved_sha256,
                                               rejection* why = nullptr) const;

  // Removes every version and the publisher record. status::invalid_arg when
  // nothing is installed under `id`.
  [[nodiscard]] expected remove(std::string_view id) const;

  // A theme of an installed, verified add-on as the chromes read it
  // (theme.h theme_to_json, plus "addon", "id" and "name").
  [[nodiscard]] result<std::string> theme_json(std::string_view addon_id,
                                               std::string_view theme_id) const;

  // At start, if the folder exists: clear stale staging.
  void startup_cleanup() const;

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

 private:
  [[nodiscard]] open_installed inspect_installed(const std::string& version_dir,
                                                 const std::string& id,
                                                 const std::string& pinned_key) const;
  [[nodiscard]] bool best_in(const std::string& id, open_installed& out) const;

  std::string root_;
  std::uint32_t api_;
};

}  // namespace mv::addon
