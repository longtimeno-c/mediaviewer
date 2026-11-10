// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/catalogue.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "addons/ai/catalog.h"
#include "addons/ai/index_db.h"
#include "core/json.h"

namespace mv::mobile {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

class stmt {
 public:
  stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
  [[nodiscard]] bool ok() const noexcept { return s_ != nullptr; }
  stmt& bind(int i, std::int64_t v) {
    sqlite3_bind_int64(s_, i, v);
    return *this;
  }
  stmt& bind(int i, const std::string& v) {
    sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  [[nodiscard]] bool next() noexcept { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
  [[nodiscard]] std::int64_t i64(int c) const noexcept { return sqlite3_column_int64(s_, c); }
  [[nodiscard]] std::string text(int c) const {
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
    return t ? std::string(t, static_cast<std::size_t>(sqlite3_column_bytes(s_, c))) : std::string();
  }
  [[nodiscard]] bool null(int c) const noexcept { return sqlite3_column_type(s_, c) == SQLITE_NULL; }

 private:
  sqlite3_stmt* s_ = nullptr;
};

result<sqlite3*> open_immutable(const std::string& path) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(ai::immutable_uri(path).c_str(), &db,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(rc == SQLITE_CANTOPEN ? status::not_found : status::corrupt);
  }
  sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  return db;
}

result<std::string> slurp(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return err(status::not_found);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Strings the publisher wrote; a manifest field that is not one is a bad manifest.
std::string field(const json::value& v, std::string_view key) {
  const std::string* s = v.str(key);
  return s ? *s : std::string();
}

// A shared path from the signed manifest, still kept inside the library folder.
bool stays_inside(std::string_view rel) {
  if (rel.empty() || rel.front() == '/') return false;
  std::size_t at = 0;
  while (at <= rel.size()) {
    const std::size_t end = std::min(rel.find('/', at), rel.size());
    const std::string_view part = rel.substr(at, end - at);
    if (part.empty() || part == "." || part == "..") return false;
    at = end + 1;
  }
  return true;
}

result<manifest> read_manifest(const std::string& dir, const std::string& library_dir) {
  MV_TRY(std::string text, slurp(dir + "/manifest.json"));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  if (doc->integer("format").value_or(0) != ai::catalog::kFormat) return err(status::unsupported_format);
  manifest m;
  const auto generation = doc->integer("generation");
  if (!generation || *generation <= 0) return err(status::corrupt);
  m.generation = static_cast<std::uint64_t>(*generation);
  m.created_unix = doc->integer("created").value_or(0);
  m.active_spec = field(*doc, "active_spec");
  m.model_spec = field(*doc, "model_spec");
  if (m.active_spec.empty()) return err(status::corrupt);
  const json::value* files = doc->find("files");
  if (!files || files->k != json::kind::array) return err(status::corrupt);
  std::error_code ec;
  for (const json::value& f : files->a) {
    const std::string name = field(f, "name");
    const auto size = f.integer("size");
    if (name.empty() || !size || name.find('/') != std::string::npos) return err(status::corrupt);
    if (static_cast<std::int64_t>(fs::file_size(fs_path(dir + "/" + name), ec)) != *size || ec) return err(status::corrupt);
    m.files[name] = static_cast<std::uint64_t>(*size);
  }
  if (!m.files.count("catalog.db")) return err(status::corrupt);
  if (const json::value* models = doc->find("model_files"); models && models->k == json::kind::array) {
    for (const json::value& f : models->a) {
      model_file out;
      out.name = field(f, "name");
      const std::string rel = field(f, "path");
      out.size = static_cast<std::uint64_t>(f.integer("size").value_or(0));
      if (out.name.empty() || out.name.find('/') != std::string::npos || !stays_inside(rel)) return err(status::corrupt);
      out.path = library_dir + "/" + rel;
      m.models.push_back(std::move(out));
    }
  }
  if (const json::value* roots = doc->find("roots"); roots && roots->k == json::kind::array) {
    for (const json::value& r : roots->a) m.roots.push_back({field(r, "id"), field(r, "share"), field(r, "sub")});
  }
  return m;
}

std::string_view leaf(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::int32_t year_month(std::int64_t wall) {
  const std::time_t t = static_cast<std::time_t>(wall);
  std::tm tm{};
  gmtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 100 + tm.tm_mon + 1;
}

}  // namespace

std::optional<std::pair<std::uint16_t, std::uint16_t>> jpeg_size(std::span<const std::uint8_t> j) {
  if (j.size() < 4 || j[0] != 0xFF || j[1] != 0xD8) return std::nullopt;
  std::size_t i = 2;
  while (i + 9 < j.size()) {
    if (j[i] != 0xFF) return std::nullopt;
    while (i < j.size() && j[i] == 0xFF) ++i;  // fill bytes
    if (i >= j.size()) return std::nullopt;
    const std::uint8_t marker = j[i++];
    if (marker == 0xD9 || marker == 0xDA) return std::nullopt;
    if (i + 2 > j.size()) return std::nullopt;
    const std::size_t length = (std::size_t{j[i]} << 8) | j[i + 1];
    const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (frame) {
      if (i + 7 > j.size()) return std::nullopt;
      const auto h = static_cast<std::uint16_t>((std::size_t{j[i + 3]} << 8) | j[i + 4]);
      const auto w = static_cast<std::uint16_t>((std::size_t{j[i + 5]} << 8) | j[i + 6]);
      if (w == 0 || h == 0) return std::nullopt;
      return std::pair{w, h};
    }
    if (length < 2) return std::nullopt;
    i += length;
  }
  return std::nullopt;
}

catalogue::~catalogue() {
  if (db_) sqlite3_close_v2(db_);
  if (faces_) sqlite3_close_v2(faces_);
  if (tiny_) munmap(const_cast<std::uint8_t*>(tiny_), tiny_size_);
}

result<std::unique_ptr<catalogue>> catalogue::open(const std::string& generation_dir, const std::string& library_dir) {
  std::unique_ptr<catalogue> c(new catalogue());
  c->dir_ = generation_dir;
  MV_TRY(manifest m, read_manifest(generation_dir, library_dir));
  c->manifest_ = std::move(m);
  MV_TRY(sqlite3 * db, open_immutable(generation_dir + "/catalog.db"));
  c->db_ = db;
  if (c->manifest_.files.count("faces.db")) {
    MV_TRY(sqlite3 * faces, open_immutable(generation_dir + "/faces.db"));
    c->faces_ = faces;
  }
  if (c->manifest_.files.count("tiny.pack")) {
    const std::string path = generation_dir + "/tiny.pack";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return err(status::io);
    struct stat st {};
    void* mapped = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
      mapped = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    }
    ::close(fd);
    if (mapped == MAP_FAILED) return err(status::io);
    c->tiny_ = static_cast<const std::uint8_t*>(mapped);
    c->tiny_size_ = static_cast<std::size_t>(st.st_size);
  }
  MV_TRY_VOID(c->read_rows());
  return c;
}

std::uint32_t catalogue::folder_of_dir(std::string_view dir) {
  std::string key(dir);
  if (auto it = folder_by_key_.find(key); it != folder_by_key_.end()) return it->second;
  // "/<root uuid>" is a top-level folder; everything below it has a parent.
  std::int32_t parent = -1;
  std::string name(leaf(dir));
  const std::size_t slash = dir.rfind('/');
  if (slash != std::string_view::npos && slash > 0) {
    parent = static_cast<std::int32_t>(folder_of_dir(dir.substr(0, slash)));
  } else {
    for (const manifest::root& r : manifest_.roots) {
      if (r.uuid == name) {
        name = !r.sub.empty() ? std::string(leaf(r.sub)) : r.share;
        break;
      }
    }
  }
  const auto index = static_cast<std::uint32_t>(folders_.size());
  folders_.push_back(folder{key, std::move(name), parent, 0, false});
  folder_by_key_.emplace(std::move(key), index);
  if (parent >= 0) folders_[static_cast<std::size_t>(parent)].has_subfolders = true;
  return index;
}

expected catalogue::read_rows() {
  stmt q(db_, "SELECT id, path, kind, COALESCE(taken_at, mtime), duration_ms FROM assets WHERE kind IN (1, 2)");
  if (!q.ok()) return err(status::corrupt);
  while (q.next()) {
    row r;
    r.id = q.i64(0);
    const std::string path = q.text(1);
    r.kind = static_cast<std::uint8_t>(q.i64(2));
    r.wall_seconds = q.i64(3);
    r.duration_ms = q.i64(4);
    const std::string_view name = leaf(path);
    r.name_offset = static_cast<std::uint32_t>(names_.size());
    r.name_length = static_cast<std::uint16_t>(std::min<std::size_t>(name.size(), 0xFFFF));
    names_.append(name.substr(0, r.name_length));
    const std::size_t slash = path.rfind('/');
    r.folder = folder_of_dir(slash == std::string::npos ? std::string_view() : std::string_view(path).substr(0, slash));
    rows_.push_back(r);
  }
  std::sort(rows_.begin(), rows_.end(), [](const row& a, const row& b) {
    return a.wall_seconds != b.wall_seconds ? a.wall_seconds > b.wall_seconds : a.id > b.id;
  });
  by_id_.reserve(rows_.size());
  for (std::size_t i = 0; i < rows_.size(); ++i) by_id_.push_back({rows_[i].id, static_cast<std::uint32_t>(i)});
  std::sort(by_id_.begin(), by_id_.end());
  for (const row& r : rows_) {
    for (std::int32_t f = static_cast<std::int32_t>(r.folder); f >= 0; f = folders_[static_cast<std::size_t>(f)].parent) {
      ++folders_[static_cast<std::size_t>(f)].items;
    }
  }
  std::vector<std::uint32_t> all(rows_.size());
  for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<std::uint32_t>(i);
  all_ = std::make_shared<listing>(list_of(std::move(all)));
  aspects_ = std::make_unique<std::atomic<std::uint32_t>[]>(rows_.size());
  return {};
}

listing catalogue::list_of(std::vector<std::uint32_t> indices) const {
  listing out;
  out.indices = std::move(indices);
  for (std::uint32_t i = 0; i < out.indices.size(); ++i) {
    const std::int64_t wall = rows_[out.indices[i]].wall_seconds;
    const std::int32_t key = year_month(wall);
    if (out.months.empty() || out.months.back().key != key) out.months.push_back(month{key, wall, i, 0});
    ++out.months.back().count;
  }
  return out;
}

const row* catalogue::row_of(std::int64_t id) const noexcept {
  const auto it = std::lower_bound(by_id_.begin(), by_id_.end(), std::pair<std::int64_t, std::uint32_t>{id, 0});
  return it != by_id_.end() && it->first == id ? &rows_[it->second] : nullptr;
}

result<std::shared_ptr<const listing>> catalogue::in_folder(std::string_view key) const {
  const auto found = folder_by_key_.find(std::string(key));
  if (found == folder_by_key_.end()) return err(status::not_found);
  std::vector<char> inside(folders_.size(), 0);
  inside[found->second] = 1;
  // Parents are made before their children: one pass reaches every descendant.
  for (std::size_t i = 0; i < folders_.size(); ++i) {
    const std::int32_t p = folders_[i].parent;
    if (p >= 0 && inside[static_cast<std::size_t>(p)]) inside[i] = 1;
  }
  std::vector<std::uint32_t> keep;
  for (std::uint32_t i = 0; i < rows_.size(); ++i) {
    if (inside[rows_[i].folder]) keep.push_back(i);
  }
  return std::shared_ptr<const listing>(std::make_shared<listing>(list_of(std::move(keep))));
}

result<std::shared_ptr<const listing>> catalogue::of_person(std::string_view key) const {
  if (!faces_) return err(status::not_found);
  const std::string text(key);
  std::int64_t id = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return err(status::invalid_arg);
    id = id * 10 + (c - '0');
  }
  std::vector<std::uint32_t> keep;
  stmt q(faces_, "SELECT DISTINCT asset_id FROM faces WHERE person_id = ?1");
  q.bind(1, id);
  while (q.next()) {
    if (const row* r = row_of(q.i64(0))) keep.push_back(index_of(r));
  }
  std::sort(keep.begin(), keep.end());
  return std::shared_ptr<const listing>(std::make_shared<listing>(list_of(std::move(keep))));
}

result<std::vector<folder>> catalogue::folders_under(std::string_view parent) const {
  std::int32_t want = -1;
  if (!parent.empty()) {
    const auto found = folder_by_key_.find(std::string(parent));
    if (found == folder_by_key_.end()) return err(status::not_found);
    want = static_cast<std::int32_t>(found->second);
  }
  std::vector<folder> out;
  for (const folder& f : folders_) {
    if (f.parent == want) out.push_back(f);
  }
  std::sort(out.begin(), out.end(), [](const folder& a, const folder& b) { return a.name < b.name; });
  return out;
}

result<std::vector<person>> catalogue::people() const {
  std::vector<person> out;
  if (!faces_) return out;
  // The face a person is shown by: their highest-scoring face, the earliest
  // face id on a tie, so every run agrees.
  stmt q(faces_,
         "SELECT p.id, p.name, COUNT(DISTINCT f.asset_id),"
         " (SELECT asset_id FROM faces WHERE person_id = p.id ORDER BY score DESC, id LIMIT 1)"
         " FROM people p JOIN faces f ON f.person_id = p.id GROUP BY p.id ORDER BY p.name, p.id");
  if (!q.ok()) return err(status::corrupt);
  while (q.next()) {
    person p;
    p.key = std::to_string(q.i64(0));
    p.name = q.text(1);
    p.items = static_cast<std::uint32_t>(q.i64(2));
    p.face_asset = q.i64(3);
    out.push_back(std::move(p));
  }
  return out;
}

std::uint32_t catalogue::people_count() const {
  if (!faces_) return 0;
  stmt q(faces_, "SELECT COUNT(*) FROM people");
  return q.next() ? static_cast<std::uint32_t>(q.i64(0)) : 0;
}

result<details> catalogue::details_of(std::int64_t id) const {
  stmt q(db_,
         "SELECT path, size, place, camera, lens, iso, aperture_x100, focal_mm, shutter_den, shutter_us"
         " FROM assets WHERE id = ?1");
  if (!q.ok()) return err(status::corrupt);
  q.bind(1, id);
  if (!q.next()) return err(status::not_found);
  details d;
  const std::string path = q.text(0);
  d.file_bytes = q.i64(1);
  d.place = q.text(2);
  d.camera = q.text(3);
  d.lens = q.text(4);
  d.iso = static_cast<std::int32_t>(q.i64(5));
  d.aperture_x100 = static_cast<std::int32_t>(q.i64(6));
  d.focal_mm = static_cast<std::int32_t>(q.i64(7));
  d.shutter_den = static_cast<std::int32_t>(q.i64(8));
  d.shutter_us = q.i64(9);
  // "/<root uuid>/a/b/file.jpg" reads as "<share>/<sub>/a/b".
  const std::size_t last = path.rfind('/');
  std::string dir = last == std::string::npos ? std::string() : path.substr(0, last);
  std::string uuid = dir.size() > 1 ? dir.substr(1, dir.find('/', 1) - 1) : std::string();
  for (const manifest::root& r : manifest_.roots) {
    if (r.uuid != uuid) continue;
    const std::string rest = dir.substr(1 + uuid.size());
    d.folder = r.share;
    if (!r.sub.empty()) d.folder += "/" + r.sub;
    d.folder += rest;
    break;
  }
  if (d.folder.empty()) d.folder = dir;
  if (const std::size_t dot = path.rfind('.'); dot != std::string::npos && dot > last) {
    for (const char c : path.substr(dot + 1)) d.format += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return d;
}

filter_options catalogue::filters() const {
  filter_options out;
  {
    // A label's first part is the place a person names: `place:` matches the
    // parts of a label, not a whole label.
    std::map<std::string, std::uint32_t> seen;
    stmt q(db_, "SELECT place, COUNT(*) FROM assets WHERE place IS NOT NULL AND place != '' AND kind IN (1, 2) GROUP BY place");
    while (q.next()) {
      std::string label = q.text(0);
      if (label.starts_with("At sea")) continue;
      label = label.substr(0, label.find(','));
      seen[label] += static_cast<std::uint32_t>(q.i64(1));
    }
    std::vector<std::pair<std::uint32_t, std::string>> by_use;
    for (auto& [name, n] : seen) by_use.push_back({n, name});
    std::sort(by_use.begin(), by_use.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    if (by_use.size() > 30) by_use.resize(30);
    for (auto& [n, name] : by_use) out.places.push_back(std::move(name));
  }
  {
    stmt q(db_, "SELECT camera FROM assets WHERE camera IS NOT NULL AND camera != '' AND kind IN (1, 2)"
                " GROUP BY camera ORDER BY COUNT(*) DESC, camera LIMIT 20");
    while (q.next()) out.cameras.push_back(q.text(0));
  }
  std::set<std::int32_t, std::greater<>> years;
  for (const row& r : rows_) years.insert(year_month(r.wall_seconds) / 100);
  out.years.assign(years.begin(), years.end());
  return out;
}

std::string catalogue::speech_spec() const {
  stmt q(db_, "SELECT spec FROM speech GROUP BY spec ORDER BY COUNT(*) DESC, spec LIMIT 1");
  return q.next() ? q.text(0) : std::string();
}

std::optional<std::int64_t> catalogue::id_at(const std::string& path) const {
  stmt q(db_, "SELECT id FROM assets WHERE path = ?1");
  q.bind(1, path);
  if (!q.next()) return std::nullopt;
  return q.i64(0);
}

std::optional<std::span<const std::uint8_t>> catalogue::tile(std::int64_t id) const {
  if (!tiny_) return std::nullopt;
  auto found = ai::catalog::find_tiny(std::span<const std::uint8_t>(tiny_, tiny_size_), id);
  if (!found || !*found) return std::nullopt;
  return **found;
}

void catalogue::fill_aspects() {
  if (aspects_filled_.exchange(true)) return;
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const auto bytes = tile(rows_[i].id);
    if (!bytes) continue;
    if (const auto size = jpeg_size(*bytes)) {
      aspects_[i].store((std::uint32_t{size->first} << 16) | size->second, std::memory_order_relaxed);
    }
  }
}

std::pair<std::uint16_t, std::uint16_t> catalogue::aspect(const row& r) const noexcept {
  const std::uint32_t packed = aspects_[static_cast<std::size_t>(&r - rows_.data())].load(std::memory_order_relaxed);
  return {static_cast<std::uint16_t>(packed >> 16), static_cast<std::uint16_t>(packed & 0xFFFF)};
}

}  // namespace mv::mobile
