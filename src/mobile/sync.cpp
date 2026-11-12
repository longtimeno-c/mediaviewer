// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/sync.h"

#include <fcntl.h>
#include <sodium.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>

#include "addons/ai/catalog.h"
#include "addons/driftwire/wire.h"
#include "core/json.h"

namespace mv::mobile {

namespace fs = std::filesystem;
namespace catalog = ai::catalog;

namespace {

constexpr std::uint64_t kMaxCurrentBytes = 4096;
constexpr std::uint64_t kMaxManifestBytes = 32u * 1024 * 1024;
constexpr std::uint64_t kSpaceMargin = 64u * 1024 * 1024;

std::string gen_folder(std::uint64_t g) {
  std::string s = std::to_string(g);
  return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

std::string gen_dir_name(std::uint64_t epoch, std::uint64_t generation) {
  return std::to_string(epoch) + "-" + std::to_string(generation);
}

bool write_atomic(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
#if defined(F_FULLFSYNC)
  ok = ok && (::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0);
#else
  ok = ok && ::fsync(fd) == 0;
#endif
  ::close(fd);
  if (!ok) {
    std::error_code ec;
    fs::remove(tmp, ec);
    return false;
  }
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string installed_json(const installed_state& s) {
  json::writer w;
  w.begin_object();
  w.key("library_id").string(s.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(s.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(s.generation));
  w.key("dir").string(s.dir);
  w.key("catalogue_root").string(s.catalogue_root);
  w.key("high_epoch").integer(static_cast<std::int64_t>(s.high_epoch));
  w.key("high_generation").integer(static_cast<std::int64_t>(s.high_generation));
  w.end_object();
  return w.str();
}

// A file from the service into memory, at most `cap` bytes.
result<std::string> read_small(remote_file& remote, const std::string& root, const std::string& rel, std::uint64_t cap,
                               const cancel_flag* cancel) {
  std::string out;
  bool too_big = false;
  auto r = remote.read_range(
      root, rel, 0, cap + 1,
      [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
        if (out.size() + bytes.size() > cap) {
          too_big = true;
          return false;
        }
        out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
      },
      cancel, false);
  if (too_big) return err(status::corrupt);
  if (!r) return err(r.error());
  return out;
}

std::string sha256_hex(std::string_view text) {
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(text.data()), text.size());
  return catalog::to_hex(std::span<const std::uint8_t>(h, sizeof h));
}

std::optional<std::string> sha256_file_hex(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = f.gcount();
    if (n > 0) crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(n));
  }
  if (f.bad()) return std::nullopt;
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, h);
  return catalog::to_hex(std::span<const std::uint8_t>(h, sizeof h));
}

std::uint64_t free_bytes(const fs::path& dir) {
  struct statfs st{};
  if (::statfs(dir.c_str(), &st) != 0) return 0;
  return static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_bsize);
}

sync_outcome outcome_of_read(status s) {
  if (s == status::cancelled) return sync_outcome::cancelled;
  return sync_outcome::unreachable;
}

}  // namespace

std::optional<installed_state> read_installed(const std::string& library_dir) {
  std::ifstream f(fs::path(library_dir) / "installed.json", std::ios::binary);
  if (!f) return std::nullopt;
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const std::string* id = doc->str("library_id");
  const std::string* dir = doc->str("dir");
  const std::string* root = doc->str("catalogue_root");
  const auto e = doc->integer("epoch"), g = doc->integer("generation"), he = doc->integer("high_epoch"),
             hg = doc->integer("high_generation");
  if (!id || !dir || !root || !e || !g || !he || !hg || *e < 0 || *g < 0 || *he < 0 || *hg < 0) return std::nullopt;
  installed_state s;
  s.library_id = *id;
  s.dir = *dir;
  s.catalogue_root = *root;
  s.epoch = static_cast<std::uint64_t>(*e);
  s.generation = static_cast<std::uint64_t>(*g);
  s.high_epoch = static_cast<std::uint64_t>(*he);
  s.high_generation = static_cast<std::uint64_t>(*hg);
  return s;
}

namespace {

// current.json on `root`: refused if not signed by the pinned key or naming
// another library. Fills `out` only on success.
struct current_read {
  sync_outcome failed = sync_outcome::unreachable;
  status read_error = status::ok;  // the read's own failure, when it failed
  std::optional<catalog::current_info> info;
};

current_read read_current(remote_file& remote, const std::string& root, const sync_config& c) {
  current_read out;
  auto text = read_small(remote, root, "current.json", kMaxCurrentBytes, nullptr);
  if (!text) {
    out.read_error = text.error();
    out.failed = outcome_of_read(text.error());
    if (text.error() == status::corrupt) out.failed = sync_outcome::rejected;
    return out;
  }
  auto info = catalog::verify_current(*text, c.publisher_key);
  if (!info || info->library_id != c.library_id) {
    out.failed = sync_outcome::rejected;
    return out;
  }
  out.info = std::move(*info);
  return out;
}

// Whether `root` holds a library.json naming this library: the desktop wrote it when the folder
// was chosen (phone-in-the-app.md §2), before any catalogue. Not signed, and not trusted for
// anything but this: the service the phone reached is already the pinned one, and file mode only
// lists the folders it names.
constexpr std::uint64_t kMaxLibraryBytes = 256u * 1024;
bool names_library(remote_file& remote, const std::string& root, const sync_config& c) {
  auto text = read_small(remote, root, "library.json", kMaxLibraryBytes, nullptr);
  if (!text) return false;
  const auto doc = json::parse(*text, 6);
  const std::string* id = doc ? doc->str("library_id") : nullptr;
  return id && *id == c.library_id;
}

// The root that holds a current.json signed for this library: a root of the
// wrong library or a forged file is not it. Roots are trusted only as far as
// their signed content. With none, `failed` says why, in this order:
//   - a read that did not answer (a timeout, the link): unreachable, never file mode on a guess;
//   - a current.json this phone refuses (another key, another library): rejected, so the phone
//     says the library was published from a different computer (§11.3), not file mode;
//   - no current.json anywhere, and a root holding this library's library.json: no_catalogue
//     (`library_root` names it): file mode;
//   - otherwise unreachable.
std::string find_root(remote_file& remote, const sync_config& c, sync_outcome& failed, std::string& library_root) {
  auto roots = remote.roots();
  if (!roots) {
    failed = outcome_of_read(roots.error());
    return {};
  }
  bool saw_rejected = false;
  std::optional<sync_outcome> read_failed;
  std::vector<std::string> absent;  // answered: no current.json there
  for (const auto& r : *roots) {
    auto attempt = read_current(remote, r.id, c);
    if (attempt.info) return r.id;
    if (attempt.failed == sync_outcome::rejected) {
      saw_rejected = true;
    } else if (attempt.read_error == status::not_found) {
      absent.push_back(r.id);
    } else if (!read_failed) {
      read_failed = attempt.failed;  // unreachable or cancelled
    }
  }
  if (read_failed) {
    failed = *read_failed;
    return {};
  }
  if (saw_rejected) {
    failed = sync_outcome::rejected;
    return {};
  }
  for (const auto& id : absent) {
    if (c.file_mode && names_library(remote, id, c)) {
      library_root = id;
      failed = sync_outcome::no_catalogue;
      return {};
    }
  }
  failed = sync_outcome::unreachable;
  return {};
}

bool safe_name(const std::string& name) { return driftwire::safe_relative(name) && name.find('/') == std::string::npos; }

}  // namespace

sync_plan check_catalogue(remote_file& remote, const sync_config& c) {
  sync_plan plan;
  if (sodium_init() < 0) return plan;
  const auto installed = read_installed(c.library_dir);
  std::string root = c.catalogue_root;
  if (root.empty() && installed) root = installed->catalogue_root;
  current_read current;
  if (!root.empty()) current = read_current(remote, root, c);
  if (root.empty() || (!current.info && current.failed == sync_outcome::unreachable)) {
    // Never asked yet, or that root no longer holds it: a folder may have moved.
    sync_outcome failed = sync_outcome::unreachable;
    root = find_root(remote, c, failed, plan.library_root);
    if (root.empty()) {
      plan.outcome = failed;
      return plan;
    }
    current = read_current(remote, root, c);
  }
  if (!current.info) {
    plan.outcome = current.failed;
    return plan;
  }
  plan.catalogue_root = root;
  plan.epoch = current.info->epoch;
  plan.generation = current.info->generation;
  if (installed && installed->library_id == c.library_id) {
    const bool older = plan.epoch < installed->high_epoch ||
                       (plan.epoch == installed->high_epoch && plan.generation < installed->high_generation);
    if (older) {
      plan.outcome = sync_outcome::older_refused;
      return plan;
    }
    if (plan.epoch == installed->epoch && plan.generation == installed->generation) {
      std::error_code ec;
      if (fs::exists(fs::path(c.library_dir) / "gen" / installed->dir, ec)) {
        plan.outcome = sync_outcome::up_to_date;
        return plan;
      }
    }
  }
  const std::string gen_rel = "gen/" + gen_folder(plan.generation);
  auto manifest = read_small(remote, root, gen_rel + "/manifest.json", kMaxManifestBytes, nullptr);
  if (!manifest) {
    plan.outcome = manifest.error() == status::corrupt ? sync_outcome::rejected : outcome_of_read(manifest.error());
    return plan;
  }
  // The manifest's hash is under the signature; nothing below is trusted before this.
  if (sha256_hex(*manifest) != current.info->manifest_sha256) {
    plan.outcome = sync_outcome::rejected;
    return plan;
  }
  const auto doc = json::parse(*manifest, 8);
  if (!doc || doc->str("library_id") == nullptr || *doc->str("library_id") != c.library_id ||
      doc->integer("epoch") != static_cast<std::int64_t>(plan.epoch) ||
      doc->integer("generation") != static_cast<std::int64_t>(plan.generation)) {
    plan.outcome = sync_outcome::rejected;
    return plan;
  }
  const auto add_row = [&](const json::value& v, bool model) -> bool {
    if (v.k != json::kind::object) return false;
    const std::string* name = v.str("name");
    const std::string* sha = v.str("sha256");
    const auto size = v.integer("size");
    if (!name || !sha || sha->size() != 64 || !size || *size < 0 || !safe_name(*name)) return false;
    sync_file f;
    f.size = static_cast<std::uint64_t>(*size);
    f.sha256 = *sha;
    f.model = model;
    if (model) {
      const std::string* path = v.str("path");
      if (!path || !driftwire::safe_relative(*path) || !path->starts_with("models/")) return false;
      f.remote_rel = *path;
      f.local_rel = *path;
    } else {
      f.remote_rel = gen_rel + "/" + *name;
      f.local_rel = *name;
    }
    plan.files.push_back(std::move(f));
    return true;
  };
  const json::value* files = doc->find("files");
  if (!files || files->k != json::kind::array) {
    plan.outcome = sync_outcome::rejected;
    return plan;
  }
  for (const json::value& v : files->a) {
    if (!add_row(v, false)) {
      plan.outcome = sync_outcome::rejected;
      return plan;
    }
  }
  if (const json::value* models = doc->find("model_files"); models && models->k == json::kind::array) {
    for (const json::value& v : models->a) {
      if (!add_row(v, true)) {
        plan.outcome = sync_outcome::rejected;
        return plan;
      }
    }
  }
  plan.manifest_text = std::move(*manifest);
  // What is already here, so the size shown is what still has to move.
  std::error_code ec;
  const fs::path staging = fs::path(c.library_dir) / "staging" / gen_dir_name(plan.epoch, plan.generation);
  for (const sync_file& f : plan.files) {
    plan.total_bytes += f.size;
    const fs::path done = f.model ? fs::path(c.library_dir) / f.local_rel : staging / f.local_rel;
    if (fs::file_size(done, ec) == f.size && !ec) {
      plan.done_bytes += f.size;
      continue;
    }
    ec.clear();
    const auto part = fs::file_size(fs::path(done.string() + ".part"), ec);
    if (!ec && part <= f.size) plan.done_bytes += part;
    ec.clear();
  }
  plan.outcome = sync_outcome::copy_available;
  return plan;
}

namespace {

// Writes the record naming generation `name` (already in gen/), then tidies.
sync_outcome finish_install(const sync_config& c, sync_plan& plan, const std::string& name) {
  std::error_code ec;
  const fs::path library = c.library_dir;
  const fs::path gen_root = library / "gen";
  const auto prior = read_installed(c.library_dir);
  installed_state next;
  next.library_id = c.library_id;
  next.epoch = plan.epoch;
  next.generation = plan.generation;
  next.dir = name;
  next.catalogue_root = plan.catalogue_root;
  next.high_epoch = plan.epoch;
  next.high_generation = plan.generation;
  if (prior && prior->library_id == c.library_id &&
      (prior->high_epoch > next.high_epoch ||
       (prior->high_epoch == next.high_epoch && prior->high_generation > next.high_generation))) {
    next.high_epoch = prior->high_epoch;
    next.high_generation = prior->high_generation;
  }
  if (!write_atomic(library / "installed.json", installed_json(next))) return sync_outcome::no_space;

  // Tidy: never fails the install. A reader of the old generation holds open
  // handles, which outlive the names.
  for (fs::directory_iterator it(gen_root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename() != name) {
      std::error_code e2;
      fs::remove_all(it->path(), e2);
      // The search engine's work folder for that generation goes with it.
      fs::remove_all(library / "engine" / it->path().filename(), e2);
    }
  }
  std::set<fs::path> kept_models;
  for (const sync_file& f : plan.files) {
    if (f.model) kept_models.insert(library / f.local_rel);
  }
  ec.clear();
  for (fs::recursive_directory_iterator it(library / "models", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && !kept_models.count(it->path()) && it->path().extension() != ".part") {
      std::error_code e2;
      fs::remove(it->path(), e2);
    }
  }
  plan.outcome = sync_outcome::installed;
  return plan.outcome;
}

}  // namespace

sync_outcome copy_catalogue(remote_file& remote, const sync_config& c, sync_plan& plan, const sync_progress& progress,
                            const cancel_flag* cancel, const sync_allowed& allowed) {
  if (plan.outcome != sync_outcome::copy_available) return plan.outcome;
  std::error_code ec;
  const fs::path library = c.library_dir;
  const std::string name = gen_dir_name(plan.epoch, plan.generation);
  const fs::path staging = library / "staging" / name;
  const fs::path gen_root = library / "gen";
  const fs::path installed_dir = gen_root / name;
  const auto may_move = [&] { return !allowed || allowed(); };

  // A crash between the folder's rename and the record's: the generation is
  // all here, so only the record is missing.
  {
    std::ifstream had(installed_dir / "manifest.json", std::ios::binary);
    if (had) {
      const std::string text((std::istreambuf_iterator<char>(had)), std::istreambuf_iterator<char>());
      if (text == plan.manifest_text) return finish_install(c, plan, name);
    }
  }
  fs::create_directories(staging, ec);
  if (ec) return sync_outcome::no_space;
  // A half-copied older generation is no use now.
  for (fs::directory_iterator it(library / "staging", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename() != name) {
      std::error_code e2;
      fs::remove_all(it->path(), e2);
    }
  }
  ec.clear();
  const std::uint64_t needed = plan.total_bytes - std::min(plan.done_bytes, plan.total_bytes);
  if (const auto room = free_bytes(library); room != 0 && needed + kSpaceMargin > room) {
    plan.outcome = sync_outcome::no_space;
    plan.needed_bytes = needed;
    return plan.outcome;
  }

  std::uint64_t done_total = plan.done_bytes;
  if (progress) progress(done_total, plan.total_bytes);
  for (const sync_file& f : plan.files) {
    const fs::path final_path = f.model ? library / f.local_rel : staging / f.local_rel;
    if (fs::file_size(final_path, ec) == f.size && !ec) continue;
    if (!may_move()) return sync_outcome::waiting;
    ec.clear();
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) return sync_outcome::no_space;
    const fs::path part = final_path.string() + ".part";
    std::uint64_t have = fs::file_size(part, ec);
    if (ec || have > f.size) {
      if (!ec) fs::remove(part, ec);
      have = 0;
      ec.clear();
    }
    if (have < f.size) {
      const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
      if (fd < 0) return sync_outcome::no_space;
      bool write_failed = false;
      bool paused = false;
      std::uint64_t at = have;
      auto got = remote.read_range(
          plan.catalogue_root, f.remote_rel, at, f.size - at,
          [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
            if (offset != at) return false;
            if (!may_move()) {
              paused = true;
              return false;
            }
            if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
              write_failed = true;
              return false;
            }
            at += bytes.size();
            done_total += bytes.size();
            if (progress) progress(done_total, plan.total_bytes);
            return true;
          },
          cancel, true);
#if defined(F_FULLFSYNC)
      (void)::fcntl(fd, F_FULLFSYNC);
#else
      (void)::fsync(fd);
#endif
      ::close(fd);
      if (write_failed) return sync_outcome::no_space;
      if (paused) return sync_outcome::waiting;
      if (!got) return outcome_of_read(got.error());
      if (at != f.size) return sync_outcome::unreachable;  // cut short: the next run resumes
    }
    if (cancel && cancel->stopped()) return sync_outcome::cancelled;
    const auto hash = sha256_file_hex(part);
    if (!hash || *hash != f.sha256) {
      std::error_code e2;
      fs::remove(part, e2);  // nothing of a bad file is resumed
      return sync_outcome::rejected;
    }
    fs::rename(part, final_path, ec);
    if (ec) return sync_outcome::no_space;
  }
  if (!write_atomic(staging / "manifest.json", plan.manifest_text)) return sync_outcome::no_space;

  // The commit: one rename of the folder, then one of the record.
  fs::create_directories(gen_root, ec);
  ec.clear();
  fs::remove_all(installed_dir, ec);
  ec.clear();
  fs::rename(staging, installed_dir, ec);
  if (ec) return sync_outcome::no_space;
  return finish_install(c, plan, name);
}

void remove_library(const std::string& library_dir) {
  std::error_code ec;
  fs::remove_all(library_dir, ec);
}

}  // namespace mv::mobile
