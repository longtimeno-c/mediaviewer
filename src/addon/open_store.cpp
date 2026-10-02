// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/open_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "addon/package.h"
#include "addon/theme.h"
#include "core/json.h"
#include "io/file_port.h"

namespace mv::addon {
namespace {

constexpr const char* kPublisherRecord = "publisher.json";
constexpr const char* kStaging = ".staging";
constexpr std::string_view kManifestName = "manifest.json";
constexpr std::string_view kSignatureName = "manifest.json.sig";

result<std::vector<std::uint8_t>> read_small(const std::string& path, std::uint64_t max_bytes) {
  auto st = io::stat_path(path);
  if (!st || st->is_directory || st->size > max_bytes) return err(status::io);
  io::file_reader in;
  MV_TRY_VOID(in.open(path, io::read_mode::sequential));
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st->size));
  std::size_t have = 0;
  while (have < bytes.size()) {
    MV_TRY(const std::size_t got,
           in.read(std::span<std::uint8_t>(bytes.data() + have, bytes.size() - have)));
    if (got == 0) break;
    have += got;
  }
  bytes.resize(have);
  return bytes;
}

expected write_new(const std::string& path, std::span<const std::uint8_t> bytes) {
  io::file_writer w;
  MV_TRY(const io::rename_outcome made, w.create_new(path));
  if (made != io::rename_outcome::renamed) return err(status::io);
  if (!bytes.empty()) MV_TRY_VOID(w.write(bytes));
  return w.close();
}

std::string_view text_of(std::span<const std::uint8_t> bytes) noexcept {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// The key the user agreed to for this add-on, or empty.
std::string pinned_key_of(const std::string& addon_dir) {
  auto bytes = read_small(io::join_path(addon_dir, kPublisherRecord), 4096);
  if (!bytes) return {};
  const auto doc = json::parse(text_of(*bytes), 4);
  if (!doc) return {};
  const std::string* key = doc->str("key");
  if (!key || key->size() != 64) return {};
  return *key;
}

std::uint64_t unique_counter() {
  static std::atomic<std::uint64_t> n{0};
  const auto t = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return (t << 8) ^ (++n);
}

std::vector<std::string> versions_newest_first(const std::string& addon_dir) {
  std::vector<std::string> sorted;
  if (auto versions = io::child_directories(addon_dir)) sorted = std::move(*versions);
  std::sort(sorted.begin(), sorted.end(),
            [](const std::string& a, const std::string& b) { return compare_versions(a, b) > 0; });
  return sorted;
}

// Every theme the manifest lists parses and passes theme.h. `read` hands back
// a listed file's bytes.
template <typename Read>
rejection check_themes(const open_manifest& m, std::string& detail, Read&& read) {
  for (const theme_ref& t : m.themes) {
    const auto bytes = read(t.path);
    if (!bytes) return rejection::file_missing;
    const theme_result r = parse_theme(text_of(bytes.value()));
    if (!r.ok()) {
      detail = t.id + ": " + theme_fault_name(r.fault);
      return rejection::invalid_theme;
    }
  }
  return rejection::none;
}

}  // namespace

const char* relation_name(package_relation r) noexcept {
  switch (r) {
    case package_relation::fresh: return "fresh";
    case package_relation::update: return "update";
    case package_relation::repair: return "repair";
    case package_relation::downgrade: return "downgrade";
    case package_relation::other_publisher: return "other_publisher";
  }
  return "unknown";
}

open_store::open_store(std::string root, std::uint32_t api) : root_(std::move(root)), api_(api) {}

open_installed open_store::inspect_installed(const std::string& version_dir, const std::string& id,
                                             const std::string& pinned_key) const {
  open_installed out;
  out.id = id;
  out.dir = version_dir;
  out.version = std::string(io::file_name_of(version_dir));
  auto bytes = read_small(io::join_path(version_dir, kManifestName), 256u << 10);
  auto sig = read_small(io::join_path(version_dir, kSignatureName), 256);
  if (!bytes) {
    out.why = rejection::malformed;
    return out;
  }
  open_decision d = check_open_manifest(
      *bytes, sig ? std::span<const std::uint8_t>(*sig) : std::span<const std::uint8_t>(), api_);
  out.m = d.m;
  if (d.why == rejection::needs_update) {
    // Still only under the key that was agreed to.
    if (pinned_key.empty() || d.m.publisher_key != pinned_key) {
      out.why = pinned_key.empty() ? rejection::not_approved : rejection::other_publisher;
      return out;
    }
    out.state = install_state::needs_update;
    out.why = d.why;
    return out;
  }
  if (!d.ok()) {
    out.why = d.why;
    return out;
  }
  // A manifest in the wrong folder is a copy, not an install.
  if (d.m.id != id || d.m.version != out.version) {
    out.why = rejection::unsafe_path;
    return out;
  }
  if (pinned_key.empty()) {
    out.why = rejection::not_approved;
    return out;
  }
  if (d.m.publisher_key != pinned_key) {
    out.why = rejection::other_publisher;
    return out;
  }
  out.why = verify_files(version_dir, std::span<const manifest_file>(d.m.files));
  if (out.why == rejection::none) {
    std::string detail;
    out.why = check_themes(d.m, detail, [&](const std::string& rel) {
      return read_small(io::join_path(version_dir, io::native_relative(rel)), kThemeMaxBytes);
    });
  }
  out.state = out.why == rejection::none ? install_state::ok : install_state::invalid;
  return out;
}

bool open_store::best_in(const std::string& id, open_installed& best) const {
  const std::string addon_dir = io::join_path(root_, id);
  const std::string pinned = pinned_key_of(addon_dir);
  bool have = false;
  for (const std::string& v : versions_newest_first(addon_dir)) {
    open_installed i = inspect_installed(io::join_path(addon_dir, v), id, pinned);
    if (!have || (best.state != install_state::ok && i.state == install_state::ok)) {
      best = std::move(i);
      have = true;
    }
    if (best.state == install_state::ok) break;
  }
  return have;
}

std::vector<open_installed> open_store::list() const {
  std::vector<open_installed> out;
  if (!io::stat_path(root_)) return out;
  auto dirs = io::child_directories(root_);
  if (!dirs) return out;
  for (const std::string& name : *dirs) {
    if (!valid_open_id(name)) continue;  // .staging, and anything that is not ours
    open_installed best;
    if (best_in(name, best)) out.push_back(std::move(best));
  }
  return out;
}

result<open_installed> open_store::find(std::string_view id) const {
  if (!valid_open_id(id)) return err(status::invalid_arg);
  const std::string name(id);
  if (!io::stat_path(io::join_path(root_, name))) return err(status::not_found);
  open_installed best;
  if (!best_in(name, best)) return err(status::not_found);
  return best;
}

package_offer open_store::inspect(std::span<const std::uint8_t> package) const {
  package_offer offer;
  const package_listing listing = read_package(package);
  if (!listing.ok()) {
    offer.why = listing.why;
    return offer;
  }
  const package_entry* manifest_entry = listing.find(kManifestName);
  const package_entry* signature_entry = listing.find(kSignatureName);
  if (!manifest_entry) {
    offer.why = rejection::malformed;
    return offer;
  }
  open_decision d = check_open_manifest(
      manifest_entry->bytes,
      signature_entry ? signature_entry->bytes : std::span<const std::uint8_t>(), api_);
  offer.m = std::move(d.m);
  offer.sha256 = sha256_hex(package);
  if (!d.ok()) {
    offer.why = d.why;
    return offer;
  }

  // Exactly the listed files, each with its signed size and hash.
  for (const manifest_file& f : offer.m.files) {
    const package_entry* e = listing.find(f.path);
    if (!e) {
      offer.why = rejection::file_missing;
      offer.detail = f.path;
      return offer;
    }
    if (e->bytes.size() != f.size || sha256_hex(e->bytes) != f.sha256) {
      offer.why = rejection::file_mismatch;
      offer.detail = f.path;
      return offer;
    }
  }
  if (listing.entries.size() != offer.m.files.size() + 2) {
    offer.why = rejection::unexpected_file;
    for (const package_entry& e : listing.entries) {
      const bool listed = e.path == kManifestName || e.path == kSignatureName ||
                          std::any_of(offer.m.files.begin(), offer.m.files.end(),
                                      [&](const manifest_file& f) { return f.path == e.path; });
      if (!listed) offer.detail = e.path;
    }
    return offer;
  }
  if (const rejection themes = check_themes(
          offer.m, offer.detail,
          [&](const std::string& rel) -> result<std::vector<std::uint8_t>> {
            const package_entry* e = listing.find(rel);
            if (!e || e->bytes.size() > kThemeMaxBytes) return err(status::io);
            return std::vector<std::uint8_t>(e->bytes.begin(), e->bytes.end());
          });
      themes != rejection::none) {
    offer.why = themes;
    return offer;
  }

  // Against what is installed under this id.
  const std::string addon_dir = io::join_path(root_, offer.m.id);
  if (io::stat_path(addon_dir)) {
    const std::string pinned = pinned_key_of(addon_dir);
    open_installed current;
    const bool have = best_in(offer.m.id, current);
    if (have) offer.installed_version = current.version;
    // A folder with versions but no record of consent is not an install to
    // build on; the package is treated as fresh and replaces it.
    if (!pinned.empty()) {
      if (pinned != offer.m.publisher_key) {
        offer.relation = package_relation::other_publisher;
        offer.why = rejection::other_publisher;
        return offer;
      }
      if (have) {
        const int order = compare_versions(offer.m.version, current.version);
        // An older package over a copy that works is a downgrade. Over one
        // that does not, it is the way back to a working add-on.
        if (order < 0 && current.state == install_state::ok) {
          offer.relation = package_relation::downgrade;
          offer.why = rejection::downgrade;
          return offer;
        }
        offer.relation = order > 0 ? package_relation::update : package_relation::repair;
      }
    }
  }
  offer.why = rejection::none;
  return offer;
}

result<open_installed> open_store::install(std::span<const std::uint8_t> package,
                                           std::string_view approved_sha256,
                                           rejection* why) const {
  const auto refuse = [why](rejection r, status s) {
    if (why) *why = r;
    return err(s);
  };
  const package_offer offer = inspect(package);
  if (!offer.ok()) {
    return refuse(offer.why, offer.why == rejection::needs_update ? status::unsupported_format
                                                                  : status::corrupt);
  }
  // What is installed is what the person was shown.
  if (approved_sha256.size() != 64 || offer.sha256 != approved_sha256) {
    return refuse(rejection::changed, status::corrupt);
  }
  const package_listing listing = read_package(package);
  if (!listing.ok()) return refuse(listing.why, status::corrupt);

  MV_TRY_VOID(io::make_directories(root_));
  const std::string staging_root = io::join_path(root_, kStaging);
  MV_TRY_VOID(io::make_directories(staging_root));
  const std::string staged = io::join_path(staging_root, std::to_string(unique_counter()));
  MV_TRY_VOID(io::make_directories(staged));
  struct consume {
    const std::string& dir;
    ~consume() { (void)io::remove_tree(dir); }
  } const cleanup{staged};

  for (const package_entry& e : listing.entries) {
    const std::string to = io::join_path(staged, io::native_relative(e.path));
    MV_TRY_VOID(io::make_directories(io::parent_of(to)));
    MV_TRY_VOID(write_new(to, e.bytes));
  }
  // The same check every later load makes, on what is now on disk.
  if (const rejection r = verify_files(staged, std::span<const manifest_file>(offer.m.files));
      r != rejection::none) {
    return refuse(r, status::corrupt);
  }

  const std::string addon_dir = io::join_path(root_, offer.m.id);
  MV_TRY_VOID(io::make_directories(addon_dir));
  const std::string target = io::join_path(addon_dir, offer.m.version);
  if (io::stat_path(target)) {
    if (!io::remove_tree(target)) return err(status::io);
  }
  MV_TRY(const io::rename_outcome moved, io::rename_no_replace(staged, target));
  if (moved != io::rename_outcome::renamed) return err(status::io);
  note_verified_move(staged, target);

  // The record of consent: the key, and when. Written by the app, outside
  // every version folder, so no package can carry its own.
  const std::string record = io::join_path(addon_dir, kPublisherRecord);
  if (pinned_key_of(addon_dir) != offer.m.publisher_key) {
    (void)io::remove_file(record);
    json::writer w;
    w.begin_object();
    w.key("schema").integer(1);
    w.key("key").string(offer.m.publisher_key);
    w.key("name").string(offer.m.publisher_name);
    w.key("approved_unix")
        .integer(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count());
    w.end_object();
    MV_TRY_VOID(write_new(record, bytes_of(w.str())));
  }

  // A data add-on holds nothing open, so the versions it replaces go now.
  for (const std::string& v : versions_newest_first(addon_dir)) {
    if (v != offer.m.version) (void)io::remove_tree(io::join_path(addon_dir, v));
  }

  open_installed out = inspect_installed(target, offer.m.id, offer.m.publisher_key);
  if (out.state != install_state::ok) return refuse(out.why, status::corrupt);
  if (why) *why = rejection::none;
  return out;
}

expected open_store::remove(std::string_view id) const {
  if (!valid_open_id(id)) return err(status::invalid_arg);
  const std::string addon_dir = io::join_path(root_, std::string(id));
  if (!io::stat_path(addon_dir)) return err(status::invalid_arg);
  return io::remove_tree(addon_dir);
}

result<std::string> open_store::theme_json(std::string_view addon_id,
                                           std::string_view theme_id) const {
  MV_TRY(const open_installed addon, find(addon_id));
  if (addon.state != install_state::ok) return err(status::corrupt);
  for (const theme_ref& t : addon.m.themes) {
    if (t.id != theme_id) continue;
    MV_TRY(const auto bytes,
           read_small(io::join_path(addon.dir, io::native_relative(t.path)), kThemeMaxBytes));
    const theme_result r = parse_theme(text_of(bytes));
    if (!r.ok()) return err(status::corrupt);
    json::writer w;
    w.begin_object();
    w.key("addon").string(addon.id);
    w.key("id").string(t.id);
    w.key("name").string(t.name);
    w.key("theme").raw(theme_to_json(r.t));
    w.end_object();
    return w.take();
  }
  return err(status::not_found);
}

void open_store::startup_cleanup() const {
  if (!io::stat_path(root_)) return;
  (void)io::remove_tree(io::join_path(root_, kStaging));
}

}  // namespace mv::addon
