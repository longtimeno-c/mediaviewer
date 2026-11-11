// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link_generation.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/json.h"

namespace mv::mobile {

struct generation_view::mapped {
  void* data = nullptr;
  std::size_t size = 0;
  ~mapped() {
    if (data) ::munmap(data, size);
  }
  [[nodiscard]] std::span<const std::uint8_t> span() const {
    return {static_cast<const std::uint8_t*>(data), size};
  }
};

namespace {

std::string uri_of(const std::string& path) {
  std::string out = "file:";
  for (const unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.') {
      out.push_back(static_cast<char>(c));
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      out += hex;
    }
  }
  return out + "?immutable=1";
}

}  // namespace

generation_view::~generation_view() {
  if (db_) sqlite3_close_v2(db_);
}

result<std::unique_ptr<generation_view>> generation_view::open(const std::string& dir) {
  std::unique_ptr<generation_view> v(new generation_view());
  v->dir_ = dir;
  const auto map_file = [&](const std::string& name) -> std::unique_ptr<mapped> {
    const std::string path = dir + "/" + name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
      ::close(fd);
      return nullptr;
    }
    void* data = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) return nullptr;
    auto m = std::make_unique<mapped>();
    m->data = data;
    m->size = static_cast<std::size_t>(st.st_size);
    return m;
  };
  v->tiny_ = map_file("tiny.pack");
  v->thumbs_ = map_file("thumbs.idx");
  std::ifstream f(dir + "/manifest.json", std::ios::binary);
  if (!f) return err(status::not_found);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto doc = json::parse(text, 8);
  if (!doc) return err(status::corrupt);
  v->created_ = static_cast<std::uint64_t>(doc->integer("created").value_or(0));
  v->assets_ = static_cast<std::uint64_t>(doc->integer("assets").value_or(0));
  if (const json::value* roots = doc->find("roots"); roots && roots->k == json::kind::array) {
    for (const json::value& r : roots->a) {
      const std::string* id = r.str("id");
      const std::string* share = r.str("share");
      const std::string* sub = r.str("sub");
      if (id && share) v->roots_[*id] = {*share, sub ? *sub : std::string()};
    }
  }
  const std::string db = dir + "/catalog.db";
  if (sqlite3_open_v2(uri_of(db).c_str(), &v->db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    return err(status::corrupt);
  }
  sqlite3_db_config(v->db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  return v;
}

std::optional<std::span<const std::uint8_t>> generation_view::tiny(std::int64_t asset) const {
  if (!tiny_) return std::nullopt;
  auto found = ai::catalog::find_tiny(tiny_->span(), asset);
  if (!found || !*found) return std::nullopt;
  return **found;
}

std::optional<ai::catalog::thumb_location> generation_view::thumb(std::int64_t asset) const {
  if (!thumbs_) return std::nullopt;
  auto found = ai::catalog::find_thumb(thumbs_->span(), asset);
  if (!found || !*found) return std::nullopt;
  return **found;
}

std::optional<asset_file> generation_view::file(std::int64_t asset) const {
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COALESCE(disk_path, path), size, kind, duration_ms FROM assets WHERE id = ?1", -1, &q,
                         nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  std::optional<asset_file> out;
  sqlite3_bind_int64(q, 1, asset);
  if (sqlite3_step(q) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
    const std::string path = text ? text : "";
    // "/<root uuid>/<relative path>"
    const auto slash = path.find('/', 1);
    if (path.size() > 1 && path.front() == '/' && slash != std::string::npos && slash + 1 < path.size()) {
      asset_file f;
      f.uuid = path.substr(1, slash - 1);
      f.rel = path.substr(slash + 1);
      f.size = static_cast<std::uint64_t>(sqlite3_column_int64(q, 1));
      f.kind = sqlite3_column_int(q, 2);
      f.duration_ms = sqlite3_column_int64(q, 3);
      out = std::move(f);
    }
  }
  sqlite3_finalize(q);
  return out;
}

void generation_view::for_each_asset(const std::function<void(const listed_asset&)>& fn) const {
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT id, kind, COALESCE(taken_at, mtime), duration_ms, path FROM assets WHERE kind IN (1, 2)"
                         " ORDER BY COALESCE(taken_at, mtime) DESC, id",
                         -1, &q, nullptr) != SQLITE_OK) {
    return;
  }
  while (sqlite3_step(q) == SQLITE_ROW) {
    listed_asset a;
    a.id = sqlite3_column_int64(q, 0);
    a.kind = sqlite3_column_int(q, 1);
    a.taken_at = sqlite3_column_int64(q, 2);
    a.duration_ms = sqlite3_column_int64(q, 3);
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(q, 4));
    std::string path = text ? text : "";
    if (const auto slash = path.rfind('/'); slash != std::string::npos) a.name = path.substr(slash + 1);
    fn(a);
  }
  sqlite3_finalize(q);
}

std::vector<moment_row> generation_view::moments(std::int64_t asset) const {
  std::vector<moment_row> out;
  const auto collect = [&](const char* sql, int kind_fixed) {
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &q, nullptr) != SQLITE_OK) return;  // an older catalogue without it
    sqlite3_bind_int64(q, 1, asset);
    while (sqlite3_step(q) == SQLITE_ROW) {
      moment_row m;
      m.start_ms = sqlite3_column_int64(q, 0);
      m.kind = kind_fixed;
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
      m.text = text ? text : "";
      if (!m.text.empty()) out.push_back(std::move(m));
    }
    sqlite3_finalize(q);
  };
  // Speech and on-screen text; an empty result is an ordinary clip.
  collect("SELECT start_ms, text FROM speech WHERE asset_id = ?1 GROUP BY start_ms ORDER BY start_ms LIMIT 500", 0);
  collect("SELECT start_ms, text FROM cues WHERE asset_id = ?1 ORDER BY start_ms LIMIT 500", 2);
  std::sort(out.begin(), out.end(), [](const moment_row& a, const moment_row& b) { return a.start_ms < b.start_ms; });
  return out;
}

}  // namespace mv::mobile
