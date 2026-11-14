// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/phone/bundled.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

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
    if (name.rfind("driftwire.old-", 0) == 0) {
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

result<installed> install(const std::string& source, const std::string& dir_utf8, const std::string& version,
                          bool rename_aside) {
  const fs::path src = path_of(source), dir = path_of(dir_utf8);
  const fs::path target = dir / program_name();
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(src, ec);
  if (ec || size == 0) return err(status::not_found);
  fs::create_directories(dir, ec);
  if (ec) return err(status::io);
  sweep_old(dir);

  installed out;
  out.program = utf8_of(target);
  const bool there = fs::exists(target, ec);
  const record was = read_record(dir);
  if (there && was.version == version && was.size == size && fs::file_size(target, ec) == size && !ec) {
    return out;  // this version is in place: nothing copied
  }

  // Beside it first: a copy cut short never stands where the login entry looks.
  const fs::path staged = dir / (std::string(program_name()) + ".new");
  fs::remove(staged, ec);
  if (!fs::copy_file(src, staged, fs::copy_options::overwrite_existing, ec) || ec) {
    fs::remove(staged, ec);
    return err(status::io);
  }
  fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec,
                  ec);
  if (there && rename_aside) {
    // A running .exe cannot be replaced, but it can be renamed.
    fs::path aside;
    for (int n = 1; n < 1000; ++n) {
      aside = dir / ("driftwire.old-" + std::to_string(n) + (rename_aside_here() ? ".exe" : ""));
      if (!fs::exists(aside, ec)) break;
    }
    fs::rename(target, aside, ec);
    if (ec) {
      fs::remove(staged, ec);
      return err(status::io);
    }
  }
  fs::rename(staged, target, ec);  // POSIX: atomic over the old one; a running one keeps its inode
  if (ec) {
    fs::remove(staged, ec);
    return err(status::io);
  }
  out.replaced = there;
  if (!write_record(dir, {version, size})) return err(status::io);
  return out;
}

void remove(const std::string& dir_utf8) noexcept {
  try {
    const fs::path dir = path_of(dir_utf8);
    std::error_code ec;
    fs::remove(dir / program_name(), ec);
    fs::remove(dir / (std::string(program_name()) + ".new"), ec);
    fs::remove(dir / kRecord, ec);
    sweep_old(dir);
    fs::remove(dir, ec);  // only when empty
  } catch (...) {
  }
}

}  // namespace mv::phone::bundled
