// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "addons/driftwire/serve_backup.h"
#include "addons/driftwire/serve_wire.h"
#include "addons/driftwire/wire.h"
#include "core/json.h"
#include "io/content_hash.h"
#include "io/file_port.h"

namespace mv::mobile::backup {

namespace serve = driftwire::serve;

namespace {

std::filesystem::path fs_path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string two(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

std::string dir_of(const std::string& rel) {
  const auto slash = rel.rfind('/');
  return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string name_of(const std::string& rel) {
  const auto slash = rel.rfind('/');
  return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

bool lower_hex64(std::string_view s) noexcept {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool valid_name(std::string_view n) {
  return !n.empty() && n.size() <= serve::kMaxWriteName && n.find('/') == std::string_view::npos && driftwire::safe_relative(n);
}

// Whether `got` is `name` or a numbered form of it (the service's " (2)").
bool named_as(std::string_view got, std::string_view name) {
  for (int k = 1; k <= serve::backup::kMaxNumbered; ++k) {
    if (got == serve::backup::numbered(name, k)) return true;
  }
  return false;
}

// Reads a file at an offset, for a write.
serve::client::read_fn file_read_fn(io::file_reader& r) {
  return [&r](std::uint64_t at, std::span<std::uint8_t> into) -> result<std::size_t> { return r.read_at(at, into); };
}

}  // namespace

bool valid_layout(int value) noexcept { return value == 1 || value == 2 || value == 4 || value == 5; }

std::string subdir(int year, int month, int day, layout l) {
  const std::string y = std::to_string(year);
  switch (l) {
    case layout::year: return y;
    case layout::year_month: return y + "/" + two(month);
    case layout::year_month_day: return y + "/" + two(month) + "/" + two(day);
    case layout::flat: return {};
  }
  return y + "/" + two(month);
}

// ---- the NAS, over the link ----------------------------------------------------------------

namespace {

class over_link final : public target {
 public:
  explicit over_link(link_session& l) : l_(l) {}

  result<std::vector<folder>> folders() override {
    auto can = l_.can_write();
    if (!can) return err(can.error());
    if (!*can) return err(status::unsupported_format);
    auto roots = l_.roots();
    if (!roots) return err(roots.error());
    std::vector<folder> out;
    for (const auto& r : *roots) {
      if (r.backup) out.push_back({r.id, r.label, r.writable});
    }
    return out;
  }
  result<serve::stat_info> space(const std::string& root) override { return l_.stat(root, ""); }
  result<std::vector<serve::list_entry>> list(const std::string& root, const std::string& rel) override {
    return l_.list_whole(root, rel);
  }
  result<serve::client::write_done> write(const std::string& root, const std::string& rel, std::uint64_t size,
                                          const std::string& hash_hex, std::uint64_t at, const serve::client::read_fn& read,
                                          const std::function<void(std::uint64_t)>& progress, const cancel_flag* cancel,
                                          serve::client::write_refused* why, bool meta) override {
    return l_.write(root, rel, size, hash_hex, at, read, progress, cancel, why, meta);
  }
  result<std::vector<std::string>> ledger_ids(const std::string& root) override {
    auto listing = l_.list_meta(root, "");
    if (!listing) {
      if (listing.error() == status::not_found) return std::vector<std::string>{};
      return err(listing.error());
    }
    std::vector<std::string> out;
    for (const auto& e : *listing) {
      if (e.dir) out.push_back(e.name);
    }
    return out;
  }
  result<std::string> read_ledger(const std::string& root, const std::string& id) override {
    std::string text;
    auto r = l_.read_meta(root, id + "/" + serve::kLedgerFile, [&](std::uint64_t, std::span<const std::uint8_t> b) {
      if (text.size() + b.size() > serve::kMaxLedgerBytes) return false;
      text.append(reinterpret_cast<const char*>(b.data()), b.size());
      return true;
    }, nullptr);
    if (!r) return err(r.error());
    return text;
  }
  std::string device_id() const override { return l_.device_folder_id(); }

 private:
  link_session& l_;
};

// ---- the sample library's folder on this phone ---------------------------------------------

class on_phone final : public target {
 public:
  on_phone(std::string path, std::string device_id, std::uint64_t limit, std::function<std::uint64_t(const std::string&)> free_bytes)
      : device_id_(std::move(device_id)) {
    root_.id = kSampleFolderId;
    root_.label = kSampleFolderLabel;
    root_.path = std::move(path);
    root_.backup = true;
    root_.limit = limit;
    root_.reserve = serve::kDefaultReserve;  // the phone itself keeps 1 GB free
    book_.may_write = [](const key32&) { return true; };
    book_.free_bytes = std::move(free_bytes);
  }

  result<std::vector<folder>> folders() override {
    std::error_code ec;
    std::filesystem::create_directories(fs_path(root_.path), ec);
    if (ec) return err(status::io);
    return std::vector<folder>{{root_.id, root_.label, true}};
  }
  result<serve::stat_info> space(const std::string& root) override {
    if (root != root_.id) return err(status::permission_denied);
    serve::stat_info s;
    s.dir = true;
    book_.space(root_, s);
    return s;
  }
  result<std::vector<serve::list_entry>> list(const std::string& root, const std::string& rel) override {
    if (root != root_.id || (!rel.empty() && !driftwire::safe_relative(rel))) return err(status::permission_denied);
    auto children = io::list_beneath(root_.path, rel, true);
    if (!children) {
      if (children.error() == status::permission_denied) return err(status::permission_denied);
      std::error_code ec;
      const auto where = rel.empty() ? fs_path(root_.path) : fs_path(io::join_path(root_.path, io::native_relative(rel)));
      return err(std::filesystem::exists(where, ec) ? status::io : status::not_found);
    }
    std::vector<serve::list_entry> out;
    for (const auto& c : *children) {
      if (!driftwire::safe_relative(c.name)) continue;  // as the service lists: no hidden names
      out.push_back({c.name, c.size, c.mtime_unix, c.is_directory});
    }
    return out;
  }

  // The service's write, step for step (serve.cpp open_write and commit), on this phone.
  result<serve::client::write_done> write(const std::string& root, const std::string& rel_in, std::uint64_t size,
                                          const std::string& hash_hex, std::uint64_t at, const serve::client::read_fn& read,
                                          const std::function<void(std::uint64_t)>& progress, const cancel_flag* cancel,
                                          serve::client::write_refused* why, bool meta) override {
    const auto refuse = [&](const char* w, std::uint64_t have = 0) -> result<serve::client::write_done> {
      if (why) *why = {w, have};
      return err(status::permission_denied);
    };
    if (why) *why = {};
    if (root != root_.id || rel_in.empty() || !driftwire::safe_relative(rel_in)) return refuse(serve::kWhyNotAllowed);
    std::string rel_dir = dir_of(rel_in);
    const std::string name = name_of(rel_in);
    if (name.size() > serve::kMaxWriteName) return refuse(serve::kWhyName);
    if (meta) {
      if (rel_dir != device_id_ || name != serve::kLedgerFile || size > serve::kMaxLedgerBytes) {
        return refuse(serve::kWhyNotAllowed);
      }
      rel_dir = std::string(serve::kBackupMetaFolder) + "/" + device_id_;
    }
    std::error_code ec;
    std::filesystem::create_directories(fs_path(root_.path), ec);
    auto dir = rel_dir.empty() ? result<std::string>(root_.path) : io::make_directories_beneath(root_.path, rel_dir);
    if (!dir) return err(dir.error());
    const std::string shown_dir = meta ? std::string() : rel_dir;
    const auto shown = [&](const std::string& n) { return shown_dir.empty() ? n : shown_dir + "/" + n; };
    if (!meta) {
      if (const auto found = serve::backup::find_existing(dir.value(), name, size, hash_hex, &book_)) {
        // This device's unfinished copy of it is no use now (as the service does).
        const std::string stale = io::join_path(dir.value(), serve::backup::part_name(name, device_id_));
        if (book_.claim(stale)) {
          const auto st = std::filesystem::symlink_status(fs_path(stale), ec);
          if (!ec && std::filesystem::is_regular_file(st)) {
            const auto bytes = std::filesystem::file_size(fs_path(stale), ec);
            if (!ec && std::filesystem::remove(fs_path(stale), ec) && !ec) book_.removed(root_.id, bytes);
          }
          ec.clear();
          book_.drop(stale);
        }
        return serve::client::write_done{shown(*found), true};
      }
    }
    const std::string part = io::join_path(dir.value(), serve::backup::part_name(name, device_id_));
    if (!book_.claim(part)) return refuse(serve::kWhyBusy);
    struct unclaim {
      serve::backup::book& b;
      const std::string& p;
      ~unclaim() { b.drop(p); }
    } claimed{book_, part};
    std::uint64_t have = 0;
    const auto pst = std::filesystem::symlink_status(fs_path(part), ec);
    if (!ec && std::filesystem::exists(pst)) {
      if (!std::filesystem::is_regular_file(pst)) return err(status::io);
      have = std::filesystem::file_size(fs_path(part), ec);
      if (ec) return err(status::io);
      if (have > size) {
        std::filesystem::remove(fs_path(part), ec);
        if (ec) return err(status::io);
        book_.removed(root_.id, have);
        have = 0;
      }
    }
    if (at != have) return refuse(serve::kWhyOffset, have);
    const std::uint64_t left = size - have;
    switch (book_.reserve(root_, left, meta)) {
      case serve::backup::book::verdict::ok: break;
      case serve::backup::book::verdict::limit: return refuse(serve::kWhyLimit);
      case serve::backup::book::verdict::full: return refuse(serve::kWhyFull);
    }
    std::uint64_t held = left;
    struct release {
      serve::backup::book& b;
      const std::string& id;
      std::uint64_t& held;
      ~release() { b.release(id, held); }
    } released{book_, root_.id, held};
    io::file_writer out;
    if (have > 0) {
      if (auto o = out.open_resume(part, have); !o) return err(status::io);
    } else {
      auto o = out.create_new(part);
      if (!o || *o != io::rename_outcome::renamed) return err(status::io);
    }
    std::vector<std::uint8_t> buf(serve::kBulkChunk);
    std::uint64_t offset = have;
    while (offset < size) {
      if (cancel && cancel->stopped()) {
        (void)out.close();
        return err(status::cancelled);
      }
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - offset));
      auto got = read(offset, std::span<std::uint8_t>(buf.data(), want));
      if (!got || *got == 0) {
        (void)out.close();
        return err(got ? status::io : got.error());
      }
      if (auto w = out.write_at(offset, std::span<const std::uint8_t>(buf.data(), *got)); !w) {
        (void)out.close();
        return err(status::io);
      }
      offset += *got;
      held -= *got;
      book_.wrote(root_.id, *got);
      if (progress) progress(offset);
    }
    const bool flushed = out.flush_durable().has_value();
    const bool closed = out.close().has_value();
    if (!flushed || !closed) return err(status::io);
    serve::backup::commit_result done;
    {
      const auto folder = book_.lock_folder(dir.value());
      done = serve::backup::commit_part(dir.value(), name, part, size, hash_hex, meta, &book_);
    }
    if (done.freed != 0) book_.removed(root_.id, done.freed);
    using kind = serve::backup::commit_result::kind;
    switch (done.k) {
      case kind::placed: return serve::client::write_done{shown(done.name), false};
      case kind::existing: return serve::client::write_done{shown(done.name), true};
      case kind::bad_hash:
        if (why) *why = {serve::kWhyHash, 0};
        return err(status::corrupt);
      case kind::failed: break;
    }
    return err(status::io);
  }

  result<std::vector<std::string>> ledger_ids(const std::string& root) override {
    if (root != root_.id) return err(status::permission_denied);
    auto dirs = io::child_directories(io::join_path(root_.path, serve::kBackupMetaFolder));
    if (!dirs) return std::vector<std::string>{};
    return *dirs;
  }
  result<std::string> read_ledger(const std::string& root, const std::string& id) override {
    if (root != root_.id || !driftwire::safe_relative(id)) return err(status::permission_denied);
    io::file_reader r;
    if (auto o = r.open_beneath(root_.path, std::string(serve::kBackupMetaFolder) + "/" + id + "/" + serve::kLedgerFile); !o) {
      return err(o.error());
    }
    if (r.size() > serve::kMaxLedgerBytes) return err(status::corrupt);
    std::string text(static_cast<std::size_t>(r.size()), '\0');
    auto got = r.read_at(0, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
    if (!got) return err(got.error());
    text.resize(*got);
    return text;
  }
  std::string device_id() const override { return device_id_; }

 private:
  serve::root_config root_;
  std::string device_id_;
  serve::backup::book book_;
};

}  // namespace

std::unique_ptr<target> link_target(link_session& link) { return std::make_unique<over_link>(link); }

std::unique_ptr<target> local_target(const std::string& folder_path, const std::string& device_id, std::uint64_t limit,
                                     std::function<std::uint64_t(const std::string&)> free_bytes) {
  return std::make_unique<on_phone>(folder_path, device_id, limit, std::move(free_bytes));
}

// ---- the ledger -------------------------------------------------------------------------------

struct ledger::impl {
  sqlite3* db = nullptr;
  mutable std::mutex m;
  ~impl() {
    if (db) sqlite3_close_v2(db);
  }
  [[nodiscard]] bool exec(const char* sql) const { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
};

namespace {

struct stmt {
  sqlite3_stmt* s = nullptr;
  stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
  ~stmt() { sqlite3_finalize(s); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
  void text(int i, std::string_view v) { sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
  void num(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); }
  [[nodiscard]] std::string col(int i) const {
    const auto* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s, i))) : std::string();
  }
};

constexpr const char* kSelect = "SELECT key, kind, name, size, hash, root, rel, at FROM rows";

row row_of(const stmt& q) {
  row r;
  r.key = q.col(0);
  r.kind = q.col(1);
  r.name = q.col(2);
  r.size = static_cast<std::uint64_t>(sqlite3_column_int64(q.s, 3));
  r.hash = q.col(4);
  r.root = q.col(5);
  r.rel = q.col(6);
  r.at = sqlite3_column_int64(q.s, 7);
  return r;
}

}  // namespace

result<std::unique_ptr<ledger>> ledger::open(const std::string& path) {
  std::unique_ptr<ledger> l(new ledger());
  l->impl_ = std::make_unique<impl>();
  if (sqlite3_open_v2(path.c_str(), &l->impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  // A row is the promise that a file is safe: it is on disk before the next one starts.
  if (!l->impl_->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;") ||
      !l->impl_->exec("CREATE TABLE IF NOT EXISTS rows (key TEXT NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL, "
                      "size INTEGER NOT NULL, hash TEXT NOT NULL, root TEXT NOT NULL, rel TEXT NOT NULL, at INTEGER NOT NULL, "
                      "PRIMARY KEY (root, key, kind, name));"
                      "CREATE INDEX IF NOT EXISTS rows_same ON rows (name, size, hash);")) {
    return err(status::corrupt);
  }
  return l;
}

ledger::~ledger() = default;

expected ledger::add(const row& r) {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, "INSERT OR REPLACE INTO rows (key, kind, name, size, hash, root, rel, at) VALUES (?,?,?,?,?,?,?,?)");
  q.text(1, r.key);
  q.text(2, r.kind);
  q.text(3, r.name);
  q.num(4, static_cast<std::int64_t>(r.size));
  q.text(5, r.hash);
  q.text(6, r.root);
  q.text(7, r.rel);
  q.num(8, r.at);
  if (sqlite3_step(q.s) != SQLITE_DONE) return err(status::io);
  return {};
}

expected ledger::add_all(const std::vector<row>& rows) {
  {
    std::lock_guard lock(impl_->m);
    if (!impl_->exec("BEGIN;")) return err(status::io);
  }
  for (const auto& r : rows) {
    if (auto a = add(r); !a) {
      std::lock_guard lock(impl_->m);
      (void)impl_->exec("ROLLBACK;");
      return a;
    }
  }
  std::lock_guard lock(impl_->m);
  return impl_->exec("COMMIT;") ? expected{} : expected(err(status::io));
}

expected ledger::remove(const std::string& root, const std::string& key, const std::string& kind, const std::string& name) {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, "DELETE FROM rows WHERE root = ? AND key = ? AND kind = ? AND name = ?");
  q.text(1, root);
  q.text(2, key);
  q.text(3, kind);
  q.text(4, name);
  if (sqlite3_step(q.s) != SQLITE_DONE) return err(status::io);
  return {};
}

std::vector<row> ledger::all(const std::string& root) const {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, (std::string(kSelect) + " WHERE root = ?").c_str());
  q.text(1, root);
  std::vector<row> out;
  while (sqlite3_step(q.s) == SQLITE_ROW) out.push_back(row_of(q));
  return out;
}

std::optional<row> ledger::find(const std::string& root, const std::string& key, const std::string& kind,
                                const std::string& name) const {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, (std::string(kSelect) + " WHERE root = ? AND key = ? AND kind = ? AND name = ?").c_str());
  q.text(1, root);
  q.text(2, key);
  q.text(3, kind);
  q.text(4, name);
  if (sqlite3_step(q.s) != SQLITE_ROW) return std::nullopt;
  return row_of(q);
}

std::optional<row> ledger::find_same(const std::string& root, const std::string& name, std::uint64_t size,
                                     const std::string& hash) const {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, (std::string(kSelect) + " WHERE root = ? AND name = ? AND size = ? AND hash = ?").c_str());
  q.text(1, root);
  q.text(2, name);
  q.num(3, static_cast<std::int64_t>(size));
  q.text(4, hash);
  if (sqlite3_step(q.s) != SQLITE_ROW) return std::nullopt;
  return row_of(q);
}

std::size_t ledger::size(const std::string& root) const {
  std::lock_guard lock(impl_->m);
  stmt q(impl_->db, "SELECT COUNT(*) FROM rows WHERE root = ?");
  q.text(1, root);
  return sqlite3_step(q.s) == SQLITE_ROW ? static_cast<std::size_t>(sqlite3_column_int64(q.s, 0)) : 0;
}

expected ledger::clear() {
  std::lock_guard lock(impl_->m);
  return impl_->exec("DELETE FROM rows;") ? expected{} : expected(err(status::io));
}

std::string ledger::to_json(std::string_view library_id, const std::string& root) const {
  json::writer w;
  w.begin_object().key("v").integer(1).key("library_id").string(library_id).key("rows").begin_array();
  for (const auto& r : all(root)) {
    w.begin_object()
        .key("k").string(r.key)
        .key("r").string(r.kind)
        .key("n").string(r.name)
        .key("s").integer(static_cast<std::int64_t>(r.size))
        .key("h").string(r.hash)
        .key("f").string(r.root)
        .key("p").string(r.rel)
        .key("t").integer(r.at)
        .end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<row>> ledger::parse_json(std::string_view text, std::string_view library_id) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->integer("v").value_or(0) != 1) return std::nullopt;
  const std::string* lib = doc->str("library_id");
  if (!lib || *lib != library_id) return std::nullopt;
  const json::value* rows = doc->find("rows");
  if (!rows || rows->k != json::kind::array) return std::nullopt;
  std::vector<row> out;
  for (const auto& v : rows->a) {
    const std::string *k = v.str("k"), *r = v.str("r"), *n = v.str("n"), *h = v.str("h"), *f = v.str("f"), *p = v.str("p");
    const auto s = v.integer("s");
    if (!k || !r || !n || !h || !f || !p || !s || *s < 0) continue;  // a row this build cannot read is only re-copied
    // A row that could not be one of ours: never let it point the folder check elsewhere.
    if (k->empty() || k->size() > 512 || !valid_name(*n) || !lower_hex64(*h) || !driftwire::safe_relative(*p) ||
        static_cast<std::uint64_t>(*s) > driftwire::kMaxFileBytes || !named_as(name_of(*p), *n)) {
      continue;
    }
    out.push_back({*k, *r, *n, static_cast<std::uint64_t>(*s), *h, *f, *p, v.integer("t").value_or(0)});
  }
  return out;
}

// ---- the run ----------------------------------------------------------------------------------

struct engine::present_set {
  std::map<std::string, std::uint64_t> files;  // rel -> size, of the folders the ledger points into
};

engine::engine(options o, target& t, source& s, ledger& l) : o_(std::move(o)), t_(t), s_(s), l_(l) {}

void engine::set_items(std::vector<item> items) { items_ = std::move(items); }

std::vector<std::string> engine::remaining() const {
  std::lock_guard lock(m_);
  return remaining_;
}

bool engine::item_done(const item& it) const {
  if (it.resources.empty()) return false;  // nothing listed for it: never counted as safe
  for (const auto& r : it.resources) {
    if (!l_.find(o_.root, it.key, r.kind, r.name)) return false;
  }
  return true;
}

bool engine::restore_if_empty() {
  if (l_.size(o_.root) != 0) return true;
  auto ids = t_.ledger_ids(o_.root);
  if (!ids) return false;
  for (const auto& id : *ids) {
    auto text = t_.read_ledger(o_.root, id);
    if (!text) continue;
    const auto rows = ledger::parse_json(*text, o_.library_id);
    if (!rows) continue;
    std::vector<row> keep;
    for (const auto& r : *rows) {
      if (r.root != o_.root) continue;
      // Never trusted on its own: the folder check below drops any row whose file is not there.
      if (!l_.find(r.root, r.key, r.kind, r.name)) keep.push_back(r);
    }
    (void)l_.add_all(keep);
    restored_ = restored_ || !keep.empty();
  }
  return true;
}

result<engine::present_set> engine::check_folder() {
  present_set present;
  const auto rows = l_.all(o_.root);
  std::set<std::string> dirs;
  for (const auto& r : rows) dirs.insert(dir_of(r.rel));
  // The folder itself first: refused there is the folder not being this phone's to write.
  if (auto top = t_.list(o_.root, ""); !top && top.error() != status::not_found) return err(top.error());
  // One listing per folder (§10b).
  for (const auto& d : dirs) {
    auto listing = t_.list(o_.root, d);
    if (!listing) {
      // Gone, or a subfolder refused (a row that named a path it may not): its rows are copied again.
      if (listing.error() == status::not_found || listing.error() == status::permission_denied) continue;
      return err(listing.error());  // the link, a truncated listing: nothing is dropped
    }
    for (const auto& e : *listing) {
      if (!e.dir) present.files[d.empty() ? e.name : d + "/" + e.name] = e.size;
    }
  }
  // A row whose file is gone, or is not that file any more, is copied again.
  for (const auto& r : rows) {
    const auto it = present.files.find(r.rel);
    if (it == present.files.end() || it->second != r.size) (void)l_.remove(r.root, r.key, r.kind, r.name);
  }
  return present;
}

void engine::copy_ledger() {
  const std::string text = l_.to_json(o_.library_id, o_.root);
  const std::string hash =
      io::hash_bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())).hex();
  const serve::client::read_fn read = [&text](std::uint64_t at, std::span<std::uint8_t> into) -> result<std::size_t> {
    if (at >= text.size()) return std::size_t{0};
    const std::size_t n = std::min<std::size_t>(into.size(), text.size() - static_cast<std::size_t>(at));
    std::memcpy(into.data(), text.data() + at, n);
    return n;
  };
  std::uint64_t at = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    serve::client::write_refused why;
    auto r = t_.write(o_.root, t_.device_id() + "/" + serve::kLedgerFile, text.size(), hash, at, read, {}, nullptr, &why, true);
    if (r) return;
    // An older copy's unfinished part: going on from it fails the hash, which removes it; then from 0.
    if (why.why == serve::kWhyOffset) {
      at = why.have;
    } else if (why.why == serve::kWhyHash) {
      at = 0;
    } else {
      return;  // best effort: the phone's own ledger is the one that counts
    }
  }
}

state engine::run(const cancel_flag* cancel, const std::function<void(const status_report&)>& report) {
  st_ = {};
  st_.total = items_.size();
  const auto say = [&] {
    if (report) report(st_);
  };
  const auto stop_with = [&](state s) {
    st_.now = s;
    st_.current.clear();
    say();
    return s;
  };
  st_.now = state::checking;
  say();

  auto folders = t_.folders();
  if (!folders) {
    if (folders.error() == status::unsupported_format) return stop_with(state::too_old);
    return stop_with(state::unreachable);
  }
  const auto chosen = std::find_if(folders->begin(), folders->end(), [&](const folder& f) { return f.id == o_.root; });
  if (chosen == folders->end()) return stop_with(state::no_folder);
  if (!chosen->writable) return stop_with(state::not_allowed);
  if (!restore_if_empty()) return stop_with(state::unreachable);
  if (auto checked = check_folder(); !checked) {
    return stop_with(checked.error() == status::permission_denied ? state::not_allowed : state::unreachable);
  }

  {
    std::lock_guard lock(m_);
    remaining_.clear();
    for (const auto& it : items_) {
      if (item_done(it)) {
        ++st_.backed_up;
      } else {
        remaining_.push_back(it.key);
      }
    }
  }
  st_.files_done = l_.size(o_.root);
  st_.now = state::running;
  say();

  std::error_code ec;
  std::filesystem::create_directories(fs_path(o_.scratch_dir), ec);
  const std::string scratch = io::join_path(o_.scratch_dir, "backup-current");
  struct scrub {
    const std::string& p;
    ~scrub() {
      std::error_code e;
      std::filesystem::remove(fs_path(p), e);
    }
  } scrubbed{scratch};
  int since_copy = 0;
  bool dirty = false;

  for (const auto& it : items_) {
    if (cancel && cancel->stopped()) {
      return stop_with(state::idle);  // leaving the app: no ledger upload now; the next run sends it
    }
    if (item_done(it)) continue;
    bool whole = true;
    int ymd[3] = {it.year, it.month, it.day};
    for (std::size_t i = 0; i < it.resources.size(); ++i) {
      const auto& res = it.resources[i];
      if (l_.find(o_.root, it.key, res.kind, res.name)) continue;
      std::filesystem::remove(fs_path(scratch), ec);
      st_.current = res.name;
      st_.current_fraction = 0;
      say();
      int read_ymd[3] = {ymd[0], ymd[1], ymd[2]};
      auto exported = s_.export_resource(it, i, scratch, cancel, read_ymd);
      if (exported && read_ymd[0] >= 1900 && read_ymd[0] <= 9999 && read_ymd[1] >= 1 && read_ymd[1] <= 12 && read_ymd[2] >= 1 &&
          read_ymd[2] <= 31) {
        std::copy(read_ymd, read_ymd + 3, ymd);
      }
      if (!exported) {
        if (exported.error() == status::cancelled) {
          return stop_with(state::idle);
        }
        st_.failures.push_back({it.key, res.name, exported.error() == status::not_found ? "gone" : "export"});
        whole = false;
        break;
      }
      io::file_reader in;
      if (auto o = in.open(scratch, io::read_mode::sequential); !o) {
        st_.failures.push_back({it.key, res.name, "export"});
        whole = false;
        break;
      }
      const std::uint64_t size = in.size();
      auto hashed = serve::backup::hash_file(scratch);
      if (!hashed) {
        st_.failures.push_back({it.key, res.name, "export"});
        whole = false;
        break;
      }
      const std::string hash = hashed.value();
      // The same file under another key (a local id that changed): already there.
      if (const auto same = l_.find_same(o_.root, res.name, size, hash)) {
        if (l_.add({it.key, res.kind, res.name, size, hash, o_.root, same->rel, now_unix()})) {
          ++st_.files_done;
          dirty = true;
        }
        continue;
      }
      const std::string dir = subdir(ymd[0], ymd[1], ymd[2], o_.folders);
      const std::string rel = dir.empty() ? res.name : dir + "/" + res.name;
      const auto read = file_read_fn(in);
      std::uint64_t at = 0;
      result<serve::client::write_done> written = err(status::io);
      serve::client::write_refused why;
      for (int attempt = 0; attempt < 4; ++attempt) {
        written = t_.write(o_.root, rel, size, hash, at, read,
                           [&](std::uint64_t sent) {
                             st_.current_fraction = size ? static_cast<double>(sent) / static_cast<double>(size) : 1.0;
                             say();
                           },
                           cancel, &why, false);
        if (written) break;
        if (why.why == serve::kWhyOffset) {
          at = why.have;  // this phone's unfinished copy from an earlier run
          continue;
        }
        if (why.why == serve::kWhyHash) {
          at = 0;  // the service removed its copy: once more from the start
          continue;
        }
        break;
      }
      if (!written) {
        if (written.error() == status::cancelled) {
          return stop_with(state::idle);
        }
        if (why.why == serve::kWhyLimit || why.why == serve::kWhyFull || why.why == serve::kWhyNotAllowed) {
          if (dirty) copy_ledger();
          return stop_with(why.why == serve::kWhyLimit ? state::limit
                           : why.why == serve::kWhyFull ? state::full
                                                        : state::not_allowed);
        }
        if (written.error() == status::io || written.error() == status::timeout) {
          return stop_with(state::unreachable);
        }
        if (written.error() == status::unsupported_format) return stop_with(state::too_old);
        st_.failures.push_back({it.key, res.name, why.why == serve::kWhyHash ? "hash" : "error"});
        whole = false;
        break;
      }
      // Only the service's confirmation makes a row (it hashed what reached its disk).
      if (auto added = l_.add({it.key, res.kind, res.name, size, hash, o_.root, written->name, now_unix()}); !added) {
        st_.failures.push_back({it.key, res.name, "error"});
        whole = false;
        break;
      }
      dirty = true;
      ++st_.copied;
      ++st_.files_done;
      st_.bytes += written->existing ? 0 : size - at;
      if (++since_copy >= o_.ledger_copy_every) {
        since_copy = 0;
        copy_ledger();
      }
    }
    if (whole && item_done(it)) {
      ++st_.backed_up;
      std::lock_guard lock(m_);
      std::erase(remaining_, it.key);
    }
    say();
  }
  if (dirty || restored_) copy_ledger();
  return stop_with(st_.failures.empty() ? state::done : state::done_failures);
}

}  // namespace mv::mobile::backup
