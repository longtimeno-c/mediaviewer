// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/phone/bundled.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include "core/json.h"

namespace mv::phone::bundled {

namespace fs = std::filesystem;

namespace {

fs::path path_of(const std::string& utf8) {
#if defined(_WIN32)
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
  return fs::path(utf8);
#endif
}

std::string utf8_of(const fs::path& p) {
#if defined(_WIN32)
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
#else
  return p.string();
#endif
}

constexpr const char* kRecord = "installed.json";

struct record {
  std::string version;
  std::uintmax_t size = 0;
};

record read_record(const fs::path& dir) {
  std::ifstream f(dir / kRecord, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  record r;
  const auto doc = json::parse(text, 2);
  if (!doc || doc->k != json::kind::object) return r;
  if (const std::string* v = doc->str("version")) r.version = *v;
  if (const auto n = doc->integer("size"); n && *n > 0) r.size = static_cast<std::uintmax_t>(*n);
  return r;
}

bool write_record(const fs::path& dir, const record& r) {
  json::writer w;
  w.begin_object().key("version").string(r.version).key("size").integer(static_cast<std::int64_t>(r.size)).end_object();
  const std::string text = w.take();
  const fs::path tmp = dir / "installed.json.new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, dir / kRecord, ec);
  return !ec;
}

// The old copies a Windows install renamed aside: gone once nothing runs them.
void sweep_old(const fs::path& dir) noexcept {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    if (name.find(".old-") != std::string::npos) {
      std::error_code rm;
      fs::remove(it->path(), rm);  // still running: it stays until the next install
    }
  }
}

}  // namespace

const char* program_name() noexcept {
#if defined(_WIN32)
  return "driftwire.exe";
#else
  return "driftwire";
#endif
}

bool rename_aside_here() noexcept {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

std::string bin_dir(const std::string& feature_dir) { return utf8_of(path_of(feature_dir) / "bin"); }

std::string program_in(const std::string& dir) { return utf8_of(path_of(dir) / program_name()); }

result<installed> install(const std::string& source_dir, const std::string& dir_utf8, const std::string& version,
                          bool rename_aside) {
  const fs::path src = path_of(source_dir), dir = path_of(dir_utf8);
  const fs::path target = dir / program_name();
  std::error_code ec;
  // The program and what it loads beside it (Windows: its DLLs), program last.
  std::vector<fs::path> files;
  std::uintmax_t size = 0;
  for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (!it->is_regular_file(fe) || fe) continue;
    const std::string name = utf8_of(it->path().filename());
    if (name == program_name()) continue;
    files.push_back(it->path());
    size += it->file_size(fe);
  }
  const std::uintmax_t program_size = fs::file_size(src / program_name(), ec);
  if (ec || program_size == 0) return err(status::not_found);
  files.push_back(src / program_name());
  size += program_size;
  fs::create_directories(dir, ec);
  if (ec) return err(status::io);
  sweep_old(dir);

  installed out;
  out.program = utf8_of(target);
  const bool there = fs::exists(target, ec);
  const record was = read_record(dir);
  if (there && was.version == version && was.size == size) return out;  // in place: nothing copied

  // Every file beside its place first: a copy cut short never stands where the
  // login entry looks.
  std::vector<std::pair<fs::path, fs::path>> staged;  // .new, final
  for (const fs::path& f : files) {
    const fs::path final_path = dir / f.filename();
    const fs::path next = dir / (utf8_of(f.filename()) + ".new");
    fs::remove(next, ec);
    if (!fs::copy_file(f, next, fs::copy_options::overwrite_existing, ec) || ec) {
      for (const auto& [n, fin] : staged) fs::remove(n, ec);
      fs::remove(next, ec);
      return err(status::io);
    }
    staged.emplace_back(next, final_path);
  }
  fs::permissions(staged.back().first, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                           fs::perms::others_read | fs::perms::others_exec,
                  ec);
  // Then each renamed into place, the program last. A file in use (Windows: the
  // running .exe and the DLLs it loaded) cannot be replaced but can be renamed:
  // it goes aside first, and a later install removes it.
  for (const auto& [next, final_path] : staged) {
    if (rename_aside && fs::exists(final_path, ec)) {
      fs::path aside;
      for (int n = 1; n < 100000; ++n) {
        aside = dir / (utf8_of(final_path.filename()) + ".old-" + std::to_string(n));
        if (!fs::exists(aside, ec)) break;
      }
      fs::rename(final_path, aside, ec);
      if (ec) return err(status::io);
    }
    fs::rename(next, final_path, ec);  // POSIX: atomic over the old one; a running one keeps its inode
    if (ec) return err(status::io);
  }
  out.replaced = there;
  if (!write_record(dir, {version, size})) return err(status::io);
  return out;
}

void remove(const std::string& dir_utf8) noexcept {
  try {
    const fs::path dir = path_of(dir_utf8);
    std::error_code ec;
    // Everything here is this add-on's (bin is its own folder); a file still
    // running is renamed aside and stays until a later install or removal.
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code rm;
      fs::remove(it->path(), rm);
    }
    fs::remove(dir, ec);  // only when empty
  } catch (...) {
  }
}

}  // namespace mv::phone::bundled
