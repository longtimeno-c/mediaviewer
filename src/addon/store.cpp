// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/store.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::addon {
namespace {

constexpr const char* kRemoveMarker = "remove.pending";  // in <dir>/, not a version

result<std::vector<std::uint8_t>> read_small(const std::string& path) {
  auto st = io::stat_path(path);
  if (!st || st->is_directory || st->size > (1u << 20)) return err(status::io);
  io::file_reader in;
  MV_TRY_VOID(in.open(path, io::read_mode::sequential));
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st->size));
  MV_TRY(const std::size_t got, in.read(std::span<std::uint8_t>(bytes.data(), bytes.size())));
  bytes.resize(got);
  return bytes;
}

std::string dir_name_for(const manifest& m) {
#if defined(__APPLE__)
  return m.name;  // ~/Library/Application Support/MediaViewer/Add-ons/Import/<version>
#else
  return m.id;    // %LocalAppData%\MediaViewer\addons\import\<version>
#endif
}

std::uint64_t unique_counter() {
  static std::atomic<std::uint64_t> n{0};
  const auto t = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return (t << 8) ^ (++n);
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) noexcept {
  const auto parts = [](const std::string& v, long long out[3]) {
    out[0] = out[1] = out[2] = -1;
    int k = 0;
    long long cur = 0;
    bool any = false;
    for (char c : v) {
      if (c == '.') {
        if (!any || k >= 2) return false;
        out[k++] = cur;
        cur = 0;
        any = false;
      } else if (c >= '0' && c <= '9') {
        cur = cur * 10 + (c - '0');
        any = true;
        if (cur > 999999999) return false;
      } else {
        return false;
      }
    }
    if (!any || k != 2) return false;
    out[2] = cur;
    return true;
  };
  long long pa[3];
  long long pb[3];
  const bool va = parts(a, pa);
  const bool vb = parts(b, pb);
  if (va != vb) return va ? 1 : -1;
  if (!va) return 0;
  for (int i = 0; i < 3; ++i) {
    if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
  }
  return 0;
}

store::store(std::string root, std::vector<std::uint8_t> public_key, std::uint32_t host_api)
    : root_(std::move(root)), key_(std::move(public_key)), host_api_(host_api) {}

installed store::inspect(const std::string& version_dir, const std::string& dir_name) const {
  installed out;
  out.dir = version_dir;
  out.data_dir = io::join_path(io::join_path(root_, dir_name), "data");
  auto bytes = read_small(io::join_path(version_dir, "manifest.json"));
  auto sig = read_small(io::join_path(version_dir, "manifest.json.sig"));
  if (!bytes) {
    out.why = rejection::malformed;
    return out;
  }
  decision d = check_manifest(*bytes, sig ? std::span<const std::uint8_t>(*sig)
                                          : std::span<const std::uint8_t>(), key_, host_api_);
  out.m = d.m;
  out.id = d.m.id;
  out.name = d.m.name;
  out.version = d.m.version;
  out.size = d.m.installed_size;
  if (d.why == rejection::needs_update) {
    out.state = install_state::needs_update;
    out.why = d.why;
    return out;
  }
  if (!d.trusted()) {
    out.why = d.why;
    return out;
  }
  // A manifest in the wrong folder is someone's copy-paste, not an install.
  if (dir_name_for(d.m) != dir_name || io::file_name_of(version_dir) != d.m.version) {
    out.why = rejection::unsafe_path;
    return out;
  }
  out.why = verify_files(version_dir, d.m);
  out.state = out.why == rejection::none ? install_state::ok : install_state::invalid;
  return out;
}

std::vector<installed> store::list() const {
  std::vector<installed> out;
  auto dirs = io::child_directories(root_);
  if (!dirs) return out;
  for (const std::string& name : *dirs) {
    const std::string addon_dir = io::join_path(root_, name);
    if (io::stat_path(io::join_path(addon_dir, kRemoveMarker))) continue;  // being removed
    auto versions = io::child_directories(addon_dir);
    if (!versions) continue;
    std::vector<std::string> sorted;
    for (const std::string& v : *versions) {
      if (v != "data") sorted.push_back(v);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const std::string& a, const std::string& b) { return compare_versions(a, b) > 0; });
    // The newest version that verifies wins; a tampered newer copy does not
    // hide a good older one, and is reported only if nothing verifies.
    installed best;
    bool have = false;
    for (const std::string& v : sorted) {
      installed i = inspect(io::join_path(addon_dir, v), name);
      if (!have || (best.state != install_state::ok && i.state == install_state::ok)) {
        best = std::move(i);
        have = true;
      }
      if (best.state == install_state::ok) break;
    }
    if (have) out.push_back(std::move(best));
  }
  return out;
}

result<installed> store::find(const std::string& id) const {
  for (installed& i : list()) {
    if (i.id == id) return std::move(i);
  }
  return err(status::invalid_arg);
}

result<std::string> store::make_staging() const {
  const std::string staging = io::join_path(root_, ".staging");
  MV_TRY_VOID(io::make_directories(staging));
  const std::string dir = io::join_path(staging, std::to_string(unique_counter()));
  MV_TRY_VOID(io::make_directories(dir));
  return dir;
}

result<installed> store::install(const std::string& staged_dir) const {
  struct consume {
    const std::string& dir;
    ~consume() { (void)io::remove_tree(dir); }
  } const cleanup{staged_dir};

  auto bytes = read_small(io::join_path(staged_dir, "manifest.json"));
  auto sig = read_small(io::join_path(staged_dir, "manifest.json.sig"));
  if (!bytes || !sig) return err(status::corrupt);
  const decision d = check_manifest(*bytes, *sig, key_, host_api_);
  if (!d.trusted()) return err(d.why == rejection::needs_update ? status::unsupported_format
                                                                : status::corrupt);
  if (verify_files(staged_dir, d.m) != rejection::none) return err(status::corrupt);

  // No downgrades: an older build is signed with the same key, so a replayed
  // old manifest (and whatever it fixed since) verifies like a new one. A
  // working installed version newer than this one refuses it; the same
  // version again is a repair and goes ahead. A newer copy that is tampered
  // or needs a newer app does not block an older one that works.
  if (auto current = find(d.m.id);
      current && current->state == install_state::ok &&
      compare_versions(current->version, d.m.version) > 0) {
    return err(status::corrupt);
  }

  const std::string name = dir_name_for(d.m);
  const std::string addon_dir = io::join_path(root_, name);
  MV_TRY_VOID(io::make_directories(addon_dir));
  (void)io::remove_file(io::join_path(addon_dir, kRemoveMarker));
  const std::string target = io::join_path(addon_dir, d.m.version);
  if (io::stat_path(target)) {
    // The same version again (a repair): replace our own folder.
    if (!io::remove_tree(target)) return err(status::io);
  }
  MV_TRY(const io::rename_outcome moved, io::rename_no_replace(staged_dir, target));
  if (moved != io::rename_outcome::renamed) return err(status::io);

  // Older versions go now, or at next start if one is loaded.
  if (auto versions = io::child_directories(addon_dir)) {
    for (const std::string& v : *versions) {
      if (v == "data" || v == d.m.version) continue;
      (void)io::remove_tree(io::join_path(addon_dir, v));
    }
  }
  installed out = inspect(target, name);
  if (out.state != install_state::ok) return err(status::corrupt);
  return out;
}

expected store::remove(const std::string& id, bool keep_data) const {
  auto dirs = io::child_directories(root_);
  if (!dirs) return err(status::io);
  bool found = false;
  bool ok = true;
  for (const std::string& name : *dirs) {
    const std::string addon_dir = io::join_path(root_, name);
    auto versions = io::child_directories(addon_dir);
    if (!versions) continue;
    bool mine = false;
    for (const std::string& v : *versions) {
      if (v == "data") continue;
      auto bytes = read_small(io::join_path(io::join_path(addon_dir, v), "manifest.json"));
      // Removal matches on the id inside any manifest, signed or not: a
      // tampered copy must be removable too.
      if (bytes) {
        const auto doc = json::parse(std::string_view(
            reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        const std::string* found_id = doc ? doc->str("id") : nullptr;
        mine = mine || (found_id && *found_id == id);
      }
    }
    if (!mine) continue;
    found = true;
    for (const std::string& v : *versions) {
      if (v == "data" && keep_data) continue;
      ok = io::remove_tree(io::join_path(addon_dir, v)).has_value() && ok;
    }
    if (!keep_data || !io::stat_path(io::join_path(addon_dir, "data"))) {
      if (!io::remove_tree(addon_dir)) ok = false;
    }
    if (!ok) {
      // A loaded library cannot be deleted on Windows: finish at next start.
      const std::string marker = keep_data ? "keep" : "delete";
      io::file_writer w;
      if (auto made = w.create_new(io::join_path(addon_dir, kRemoveMarker));
          made && *made == io::rename_outcome::renamed) {
        (void)w.write(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(marker.data()), marker.size()));
        (void)w.close();
      }
    }
  }
  if (!found) return err(status::invalid_arg);
  return {};
}

void store::startup_cleanup() const {
  (void)io::remove_tree(io::join_path(root_, ".staging"));
  auto dirs = io::child_directories(root_);
  if (!dirs) return;
  for (const std::string& name : *dirs) {
    const std::string addon_dir = io::join_path(root_, name);
    const std::string marker = io::join_path(addon_dir, kRemoveMarker);
    auto text = read_small(marker);
    if (!text) continue;
    const bool keep = std::string(text->begin(), text->end()) == "keep";
    if (auto versions = io::child_directories(addon_dir)) {
      for (const std::string& v : *versions) {
        if (v == "data" && keep) continue;
        (void)io::remove_tree(io::join_path(addon_dir, v));
      }
    }
    (void)io::remove_file(marker);
    if (!keep) (void)io::remove_tree(addon_dir);
  }
}

}  // namespace mv::addon
