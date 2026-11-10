// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/catalog.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

#include "addons/ai/index_db.h"
#include "addons/ai/sha256.h"
#include "core/json.h"

namespace mv::ai::catalog {
namespace fs = std::filesystem;
namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

class stmt {
 public:
  stmt(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
  }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
  stmt& bind(int i, const std::string& v) {
    sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  stmt& bind(int i, std::int64_t v) {
    sqlite3_bind_int64(s_, i, v);
    return *this;
  }
  // A row, or false at the end; a read error is `failed`, never "no more rows".
  bool next() {
    if (!s_) {
      failed = true;
      return false;
    }
    const int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) failed = true;
    return false;
  }
  [[nodiscard]] bool run() {
    (void)next();
    return !failed;
  }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = sqlite3_column_text(s_, col);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s_, col)))
             : std::string();
  }
  bool failed = false;

 private:
  sqlite3_stmt* s_ = nullptr;
};

struct db_closer {
  sqlite3* db;
  ~db_closer() { sqlite3_close_v2(db); }
};

// A table's columns in `schema`, in order. Hidden FTS columns are not listed.
std::vector<std::string> columns_of(sqlite3* db, const char* schema, const char* table) {
  std::vector<std::string> out;
  stmt q(db, "SELECT name FROM pragma_table_info(?1, ?2)");
  q.bind(1, std::string(table)).bind(2, std::string(schema));
  while (q.next()) out.push_back(q.text(0));
  return out;
}

// Both sides of a copy: the columns the target has, and those the source has too.
std::vector<std::string> shared_columns(sqlite3* db, const char* table) {
  const auto there = columns_of(db, "src", table);
  const std::set<std::string> from(there.begin(), there.end());
  std::vector<std::string> out;
  for (std::string& c : columns_of(db, "main", table)) {
    if (from.count(c)) out.push_back(std::move(c));
  }
  return out;
}

std::string quoted(const std::string& name) { return "\"" + name + "\""; }

std::string join_columns(const std::vector<std::string>& cols) {
  std::string out;
  for (const std::string& c : cols) out += (out.empty() ? "" : ", ") + quoted(c);
  return out;
}

// A path of this machine, as the phone names it: /<root uuid>/<relative path>.
std::string relocated(const std::string& column) {
#if defined(_WIN32)
  const std::string rest = "replace(substr(" + quoted(column) + ", length(?2) + 2), '\\', '/')";
#else
  // A backslash is an ordinary character in a POSIX file name.
  const std::string rest = "substr(" + quoted(column) + ", length(?2) + 2)";
#endif
  return "CASE WHEN COALESCE(" + quoted(column) + ", '') = '' THEN " + quoted(column) + " ELSE '/' || ?1 || '/' || " +
         rest + " END";
}

std::string trimmed(std::string path) {
#if defined(_WIN32)
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
#else
  while (path.size() > 1 && path.back() == '/') path.pop_back();
#endif
  return path;
}

// Keeps the answering tower and everything that is not a picture tower (sound,
// document pages): a migration leaves two towers' vectors in the index, and a
// phone must not carry the one nothing answers from.
const char* const kSpecFilter =
    "(spec = ?1 OR NOT (spec GLOB 'clip-vit-*' OR spec GLOB 'openclip-*'))";

std::string slurp(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A local file of the generation: deliver() copies it with a verified, flushed
// copy, so a power cut here costs only this build.
[[nodiscard]] bool write_text(const std::string& path, std::string_view text) {
  std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return static_cast<bool>(out);
}

bool cancelled(const control& c) { return c.cancelled && c.cancelled(); }
void pause_for_viewer(const control& c) {
  if (c.yield) c.yield();
}
void report(const control& c, double f) {
  if (c.report) c.report(f);
}

expected copy_tables(sqlite3* db, const build_options& o, build_counts& n, const control& c) {
  const auto step = [&](double f) -> expected {
    if (cancelled(c)) return err(status::cancelled);
    report(c, f);
    pause_for_viewer(c);
    return {};
  };
  if (!exec(db, "BEGIN")) return err(status::io);
  // meta first: the schema row was made by index_db::open and stays this build's.
  if (!exec(db, "INSERT OR REPLACE INTO main.meta(key, value) SELECT key, value FROM src.meta WHERE key != 'schema'")) {
    return err(status::io);
  }
  {
    const auto cols = shared_columns(db, "roots");
    std::string select;
    for (const std::string& col : cols) {
      select += (select.empty() ? "" : ", ") + (col == "path" ? std::string("'/' || ?1") : quoted(col));
    }
    for (const root_map& r : o.roots) {
      stmt one(db, "INSERT INTO main.roots(" + join_columns(cols) + ") SELECT " + select + " FROM src.roots WHERE id = ?2");
      if (!one.bind(1, r.uuid).bind(2, r.id).run()) return err(status::io);
    }
  }
  {
    const auto cols = shared_columns(db, "assets");
    std::string select;
    for (const std::string& col : cols) {
      select += (select.empty() ? "" : ", ") + (col == "path" || col == "motion" ? relocated(col) : quoted(col));
    }
    for (const root_map& r : o.roots) {
      stmt in(db, "INSERT INTO main.assets(" + join_columns(cols) + ") SELECT " + select +
                      " FROM src.assets WHERE root_id = ?3");
      if (!in.bind(1, r.uuid).bind(2, trimmed(r.path)).bind(3, r.id).run()) return err(status::io);
    }
  }
  MV_TRY_VOID(step(0.15));
  // The rows of the chosen assets, table by table. A table this index does not
  // have (never expected) is simply not copied: its columns come back empty.
  struct linked {
    const char* table;
    const char* where;
    bool spec_bound;
  };
  static constexpr linked kLinked[] = {
      {"progress", "asset_id IN (SELECT id FROM main.assets)", false},
      {"frames", "asset_id IN (SELECT id FROM main.assets)", true},
      {"speech", "asset_id IN (SELECT id FROM main.assets)", false},
      {"cues", "asset_id IN (SELECT id FROM main.assets)", false},
      {"tracks", "asset_id IN (SELECT id FROM main.assets)", false},
      {"passages", "asset_id IN (SELECT id FROM main.assets)", false},
      {"passage_vecs", "passage_id IN (SELECT id FROM main.passages)", false},
  };
  double f = 0.15;
  for (const linked& t : kLinked) {
    const auto cols = shared_columns(db, t.table);
    if (!cols.empty()) {
      std::string sql = "INSERT INTO main." + std::string(t.table) + "(" + join_columns(cols) + ") SELECT " +
                        join_columns(cols) + " FROM src." + t.table + " WHERE " + t.where;
      if (t.spec_bound) sql += std::string(" AND ") + kSpecFilter;
      stmt in(db, sql);
      if (t.spec_bound) in.bind(1, o.active_spec);
      if (!in.run()) return err(status::io);
    }
    f += 0.1;
    MV_TRY_VOID(step(f));
  }
  // Search text: rows of a full-text table are addressed by rowid.
  for (const auto& [table, owner] : {std::pair{"asset_text", "assets"}, std::pair{"passage_text", "passages"}}) {
    const auto cols = shared_columns(db, table);
    if (cols.empty()) continue;
    const std::string sql = "INSERT INTO main." + std::string(table) + "(rowid, " + join_columns(cols) +
                            ") SELECT rowid, " + join_columns(cols) + " FROM src." + table +
                            " WHERE rowid IN (SELECT id FROM main." + owner + ")";
    stmt in(db, sql);
    if (!in.run()) return err(status::io);
  }
  {
    stmt q(db, "SELECT COUNT(*) FROM main.assets");
    if (!q.next()) return err(status::io);
    n.assets = static_cast<std::uint64_t>(std::stoll(q.text(0)));
  }
  {
    stmt q(db, "SELECT COUNT(*) FROM main.frames WHERE spec = ?1");
    q.bind(1, o.active_spec);
    if (!q.next()) return err(status::io);
    n.frames = static_cast<std::uint64_t>(std::stoll(q.text(0)));
  }
  {
    stmt q(db, "SELECT value FROM main.meta WHERE key = 'schema'");
    if (!q.next()) return err(status::io);
    n.schema = std::stoi(q.text(0));
  }
  if (!exec(db, "COMMIT")) return err(status::io);
  return {};
}

expected make_database(const build_options& o, const std::string& file, build_counts& n, const control& c) {
  {
    // The same migrations as the app's own index, so a reader cannot tell it apart.
    MV_TRY(auto fresh, index_db::open(file));
    (void)fresh;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(status::io);
  }
  {
    db_closer closer{db};
    sqlite3_busy_timeout(db, 5000);
    // One file, no journal beside it: it is thrown away if this fails, and a
    // phone opens it read-only.
    if (!exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=FULL; PRAGMA foreign_keys=OFF;")) return err(status::io);
    {
      stmt a(db, "ATTACH ?1 AS src");
      if (!a.bind(1, o.index_db).run()) return err(status::io);
    }
    MV_TRY_VOID(copy_tables(db, o, n, c));
    if (!exec(db, "DETACH src")) return err(status::io);
    stmt q(db, "PRAGMA quick_check(1)");
    if (!q.next() || q.text(0) != "ok") return err(status::corrupt);
  }
  return {};
}

void add_file_row(json::writer& w, const std::string& dir, const std::string& name) {
  std::error_code ec;
  const auto size = fs::file_size(fs_path(dir + "/" + name), ec);
  const auto hash = sha256_file_hex(dir + "/" + name);
  w.begin_object();
  w.key("name").string(name);
  w.key("size").integer(ec ? 0 : static_cast<std::int64_t>(size));
  w.key("sha256").string(hash.value_or(std::string()));
  w.end_object();
}

}  // namespace

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> make_keypair() {
  (void)sodium_init();
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES), sec(crypto_sign_ed25519_SECRETKEYBYTES);
  crypto_sign_ed25519_keypair(pub.data(), sec.data());
  return {std::move(pub), std::move(sec)};
}

std::vector<std::uint8_t> public_of(std::span<const std::uint8_t> secret) {
  if (secret.size() != crypto_sign_ed25519_SECRETKEYBYTES) return {};
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES);
  if (crypto_sign_ed25519_sk_to_pk(pub.data(), secret.data()) != 0) return {};
  return pub;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15u]);
  }
  return out;
}

std::string fingerprint(std::span<const std::uint8_t> public_key) {
  sha256 h;
  h.update(public_key);
  const std::string hex = h.finish_hex();
  return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4);
}

namespace {

// Exactly the bytes a signature covers. Each text field has its length first,
// so no two different sets of fields can read as the same message.
std::string signed_message(const current_info& c) {
  const auto number = [](std::uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
    return s;
  };
  std::string m = "mediaviewer.catalog.current.v1";
  m += number(c.library_id.size()) + c.library_id;
  m += number(c.epoch);
  m += number(c.generation);
  m += number(c.manifest_sha256.size()) + c.manifest_sha256;
  return m;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  if (hex.size() % 2 != 0) return {};
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return {};
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

}  // namespace

result<current_info> verify_current(std::string_view text, std::span<const std::uint8_t> public_key) {
  (void)sodium_init();
  if (public_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) return err(status::invalid_arg);
  const auto doc = json::parse(text, 4);
  if (!doc) return err(status::corrupt);
  const std::string* id = doc->str("library_id");
  const std::string* manifest = doc->str("manifest_sha256");
  const std::string* signature = doc->str("signature");
  const auto epoch = doc->integer("epoch");
  const auto generation = doc->integer("generation");
  if (!id || !manifest || !signature || !epoch || !generation || *epoch < 0 || *generation < 0) return err(status::corrupt);
  const std::vector<std::uint8_t> sig = from_hex(*signature);
  if (sig.size() != crypto_sign_ed25519_BYTES) return err(status::corrupt);
  current_info c;
  c.library_id = *id;
  c.epoch = static_cast<std::uint64_t>(*epoch);
  c.generation = static_cast<std::uint64_t>(*generation);
  c.manifest_sha256 = *manifest;
  const std::string m = signed_message(c);
  if (crypto_sign_ed25519_verify_detached(sig.data(), reinterpret_cast<const unsigned char*>(m.data()), m.size(),
                                          public_key.data()) != 0) {
    return err(status::permission_denied);
  }
  return c;
}

result<build_counts> build(const build_options& o, const control& c) {
  if (o.roots.empty() || o.active_spec.empty()) return err(status::invalid_arg);
  std::error_code ec;
  fs::remove_all(fs_path(o.out_dir), ec);
  fs::create_directories(fs_path(o.out_dir), ec);
  if (ec) return err(status::io);
  const auto fail = [&](status s) -> result<build_counts> {
    std::error_code e2;
    fs::remove_all(fs_path(o.out_dir), e2);
    return err(s);
  };
  build_counts n;
  const std::string file = o.out_dir + "/catalog.db";
  if (const expected made = make_database(o, file, n, c); !made) return fail(made.error());

  std::vector<std::string> names{"catalog.db"};
  if (!write_text(o.out_dir + "/settings.json", o.settings_json)) return fail(status::io);
  names.push_back("settings.json");
  if (!o.labels_file.empty() && fs::exists(fs_path(o.labels_file), ec)) {
    fs::copy_file(fs_path(o.labels_file), fs_path(o.out_dir + "/labels.f32"), ec);
    if (ec) return fail(status::io);
    names.push_back("labels.f32");
  }
  if (cancelled(c)) return fail(status::cancelled);

  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(o.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(o.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(o.generation));
  w.key("created").integer(o.created_unix);
  w.key("schema").integer(n.schema);
  w.key("active_spec").string(o.active_spec);
  w.key("assets").integer(static_cast<std::int64_t>(n.assets));
  w.key("frames").integer(static_cast<std::int64_t>(n.frames));
  w.key("roots").begin_array();
  for (const root_map& r : o.roots) {
    w.begin_object();
    w.key("id").string(r.uuid);
    w.key("share").string(r.share);
    w.key("sub").string(r.sub);
    w.end_object();
  }
  w.end_array();
  w.key("files").begin_array();
  for (const std::string& name : names) add_file_row(w, o.out_dir, name);
  w.end_array();
  w.end_object();
  if (!write_text(o.out_dir + "/manifest.json", w.str())) return fail(status::io);
  for (const std::string& name : names) n.bytes += static_cast<std::uint64_t>(fs::file_size(fs_path(o.out_dir + "/" + name), ec));
  return n;
}

namespace {

std::string base_of(const std::string& destination) { return trimmed(destination) + "/.mediaviewer"; }

// The generation directories present, by number; a .part or a stranger is not one.
std::vector<std::uint64_t> generations_in(const std::string& gen_root) {
  std::vector<std::uint64_t> out;
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    std::uint64_t g = 0;
    const char* const end_of_name = name.data() + name.size();
    const auto [stop, why] = std::from_chars(name.data(), end_of_name, g);
    if (name.empty() || why != std::errc{} || stop != end_of_name) continue;  // a stranger, or too long to be ours
    out.push_back(g);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string gen_name(std::uint64_t g) {
  std::string s = std::to_string(g);
  return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

// Every generation but the newest two goes once it is old enough, and so does
// a part folder a dead run left. Failing to tidy never fails a publish.
void tidy(const deliver_options& o, const std::string& gen_root) {
  const auto gens = generations_in(gen_root);
  const auto stale = [&](const fs::path& p) {
    std::error_code ec;
    const auto when = fs::last_write_time(p, ec);
    if (ec) return false;
    const auto age = fs::file_time_type::clock::now() - when;
    return std::chrono::duration_cast<std::chrono::seconds>(age).count() >= o.keep_seconds;
  };
  std::error_code ec;
  // A reader may have started on a generation until the one after it arrived,
  // so the grace runs from then, not from when this one was written.
  for (std::size_t i = 0; i + 2 < gens.size(); ++i) {
    if (stale(fs_path(gen_root + "/" + gen_name(gens[i + 1])))) {
      fs::remove_all(fs_path(gen_root + "/" + gen_name(gens[i])), ec);
    }
  }
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    if (name.size() > 5 && name.ends_with(".part") && name != gen_name(o.generation) + ".part" && stale(it->path())) {
      std::error_code e2;
      fs::remove_all(it->path(), e2);
    }
  }
}

}  // namespace

result<std::uint64_t> published_generation(const std::string& destination, const std::string& library_id,
                                           std::span<const std::uint8_t> public_key) {
  std::error_code ec;
  if (!fs::is_directory(fs_path(destination), ec)) return err(status::io);
  const std::string base = base_of(destination);
  if (fs::exists(fs_path(base + "/library.json"), ec)) {
    // Only a file that reads and names someone else is another library's.
    const auto lib = json::parse(slurp(base + "/library.json"), 4);
    const std::string* id = lib ? lib->str("library_id") : nullptr;
    if (!id) return err(status::io);
    const std::string* key = lib->str("public_key");
    if (*id != library_id || !key || *key != to_hex(public_key)) return err(status::permission_denied);
  }
  if (!fs::exists(fs_path(base + "/current.json"), ec)) return std::uint64_t{0};
  const auto cur = json::parse(slurp(base + "/current.json"), 4);
  if (!cur) return std::uint64_t{0};
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, cur->integer("generation").value_or(0)));
}

expected deliver(const deliver_options& o, const control& c) {
  if (o.secret_key.size() != kSecretKeyBytes || o.public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  MV_TRY(const std::uint64_t there, published_generation(o.destination, o.library_id, o.public_key));
  if (there >= o.generation) return err(status::invalid_arg);  // numbers only go up

  const std::string base = base_of(o.destination);
  const std::string gen_root = base + "/gen";
  const std::string name = gen_name(o.generation);
  const std::string part = gen_root + "/" + name + ".part";
  const std::string done = gen_root + "/" + name;
  MV_TRY_VOID(o.make_directories(gen_root));
  std::error_code ec;
  fs::remove_all(fs_path(part), ec);
  ec.clear();
  MV_TRY_VOID(o.make_directories(part));

  if (!fs::exists(fs_path(base + "/library.json"), ec)) {
    json::writer w;
    w.begin_object();
    w.key("format").integer(kFormat);
    w.key("library_id").string(o.library_id);
    w.key("public_key").string(to_hex(o.public_key));
    w.end_object();
    const std::string tmp = base + "/library.json.part";
    if (!write_text(tmp, w.str())) return err(status::io);
    fs::rename(fs_path(tmp), fs_path(base + "/library.json"), ec);
    if (ec) return err(status::io);
  }

  // The manifest goes last, so a part folder with one has every file.
  std::vector<std::string> names;
  for (fs::directory_iterator it(fs_path(o.gen_dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = utf8_of(it->path().filename());
    if (n != "manifest.json") names.push_back(n);
  }
  if (ec) return err(status::io);
  std::sort(names.begin(), names.end());
  names.push_back("manifest.json");
  double i = 0;
  for (const std::string& n : names) {
    if (cancelled(c)) return err(status::cancelled);
    report(c, i++ / static_cast<double>(names.size()));
    MV_TRY_VOID(o.copy(o.gen_dir + "/" + n, part + "/" + n));
  }
  const auto manifest_hash = sha256_file_hex(part + "/manifest.json");
  if (!manifest_hash) return err(status::io);

  // The two renames are the commit: first the generation exists, then it is
  // current. A folder already under this name is a run that got no further:
  // current.json names an older one (checked above), so no reader is on it.
  fs::remove_all(fs_path(done), ec);
  ec.clear();
  fs::rename(fs_path(part), fs_path(done), ec);
  if (ec) return err(status::io);
  current_info info;
  info.library_id = o.library_id;
  info.epoch = o.epoch;
  info.generation = o.generation;
  info.manifest_sha256 = *manifest_hash;
  const std::string message = signed_message(info);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(), o.secret_key.data()) != 0) {
    return err(status::io);
  }
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(o.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(o.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(o.generation));
  w.key("manifest_sha256").string(*manifest_hash);
  w.key("signature").string(to_hex(signature));
  w.end_object();
  const std::string tmp = base + "/current.json.part";
  if (!write_text(tmp, w.str())) return err(status::io);
  fs::rename(fs_path(tmp), fs_path(base + "/current.json"), ec);
  if (ec) return err(status::io);
  report(c, 1.0);
  tidy(o, gen_root);
  return {};
}

}  // namespace mv::ai::catalog
