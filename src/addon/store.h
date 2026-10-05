// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Installed add-ons on disk (docs/design/18 "Location", "Updates").
//
//   <addons>/<dir>/<version>/   manifest.json, manifest.json.sig, the files
//   <addons>/<dir>/data/        the add-on's own data (import.db)
//   <addons>/.staging/<n>/      a download being verified
//
// <dir> is the id on Windows ("import") and the name on Mac ("Import"), as
// docs/design/18 spells the two locations. Every load re-verifies the signature and
// every file, so an offline sideload (a folder dropped in) is checked exactly
// like a download. With no add-on installed nothing here writes anything.
// Worker threads only.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "addon/manifest.h"
#include "core/result.h"

namespace mv::addon {

enum class install_state : std::uint8_t {
  ok = 0,
  needs_update = 1,  // valid, but its host API range excludes this app
  invalid = 2,       // tampered, unsigned, or incomplete: never loaded
};

struct installed {
  std::string id;
  std::string name;
  std::string version;
  std::string dir;       // the version folder
  std::string data_dir;  // <addons>/<dir>/data
  std::uint64_t size = 0;
  install_state state = install_state::invalid;
  rejection why = rejection::none;
  manifest m;
};

class store {
 public:
  // `root`: the addons folder (io::addons_dir()). `public_key`: the pinned
  // key, or a test key.
  store(std::string root, std::vector<std::uint8_t> public_key, std::uint32_t host_api);

  // The newest version of each add-on, verified. [worker]
  [[nodiscard]] std::vector<installed> list() const;
  [[nodiscard]] result<installed> find(const std::string& id) const;

  // A fresh staging folder for a download, under the addons folder so the
  // final move is a rename on one volume.
  [[nodiscard]] result<std::string> make_staging() const;

  // Verifies a staged folder (manifest.json + .sig + files) and moves it into
  // place. The folder is consumed either way. A family over its ceiling
  // (family_ceiling; the AI pack's 3 GB) is refused: status::unsupported_format,
  // like an add-on that needs a newer app; family_usage says which. An older version than one that
  // is installed and verifies is refused (status::corrupt): no downgrades. Older versions stay until
  // the next start (startup_cleanup): the one replaced may be running. The newest that verifies loads.
  [[nodiscard]] result<installed> install(const std::string& staged_dir) const;

  // Installed bytes of a family's verified members, and its ceiling (0 none).
  struct family_room {
    std::uint64_t used = 0;
    std::uint64_t ceiling = 0;
  };
  [[nodiscard]] family_room family_usage(std::string_view family) const;

  // Removes every version. keep_data = false also deletes <dir>/data; true
  // keeps it but for <dir>/data/cache, an add-on's derived, rebuildable files.
  // Anything locked (loaded) is marked and removed at next start.
  [[nodiscard]] expected remove(const std::string& id, bool keep_data) const;

  // At start: finish removals a loaded add-on deferred, clear stale staging.
  void startup_cleanup() const;

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

 private:
  [[nodiscard]] installed inspect(const std::string& version_dir, const std::string& dir_name) const;
  // The newest version of one add-on folder that verifies (list()'s rule).
  [[nodiscard]] bool best_in(const std::string& dir_name, installed& out) const;
  // The newest signed manifest in an add-on folder, WITHOUT hashing its
  // files: which add-on a folder holds, and its signed installed size. A
  // multi-GB pack (the AI Core) makes a full verify cost seconds, so only the
  // add-on being loaded is hashed (Milestone H).
  [[nodiscard]] bool peek(const std::string& dir_name, manifest& out) const;

  std::string root_;
  std::vector<std::uint8_t> key_;
  std::uint32_t host_api_;
};

// "1.10.0" > "1.9.3". Malformed sorts first.
[[nodiscard]] int compare_versions(const std::string& a, const std::string& b) noexcept;

}  // namespace mv::addon
