// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "wire/serve_backup.h"
#include "wire/serve_wire.h"
#include "wire/wire.h"
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
  // (limited_: as of the last folders() answer.)

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
    // Read with the list: a link gone since answers nothing about the scope (unreachable).
    std::int64_t expires = 0;
    bool scoped = false;
    const int g = l_.grant(expires, scoped);
    if (g < 0) return err(status::io);
    limited_ = g == 1 && scoped;
    return out;
  }
  bool limited_to_some_folders() const override { return limited_.load(std::memory_order_acquire); }
  bool writes_at_once() override {
    auto yes = l_.writes_at_once();
    return yes && *yes;
  }
  result<bool> trash(const std::string& root, const std::string& rel) override {
    serve::client::change_refused why;
    auto r = l_.remove(root, rel, &why);
    if (r) return true;
    // Gone already is done; an offline folder is not.
    if (r.error() == status::not_found && why.why == serve::client::change_why::not_found) return false;
    return err(r.error());
  }
  expected make_folder(const std::string& root, const std::string& rel) override {
    serve::client::change_refused why;
    auto r = l_.make_folder(root, rel, &why);
    if (r) return {};
    using w = serve::client::change_why;
    if (why.why == w::unsupported) return err(status::unsupported_format);
    if (why.why == w::exists || why.why == w::path) return err(status::invalid_arg);
    return err(r.error());
  }
  result<serve::stat_info> space(const std::string& root) override { return l_.stat(root, ""); }
  result<std::vector<serve::list_entry>> list(const std::string& root, const std::string& rel) override {
    return l_.list_whole(root, rel);
  }
  result<serve::client::write_done> write(const std::string& root, const std::string& rel, std::uint64_t size,
                                          const std::string& hash_hex, std::uint64_t at, const serve::client::read_fn& read,
                                          const std::function<void(std::uint64_t)>& progress, const cancel_flag* cancel,
                                          serve::client::write_refused* why, bool meta, std::int64_t taken) override {
    return l_.write(root, rel, size, hash_hex, at, read, progress, cancel, why, meta, taken);
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
  // The run's thread and the chooser's both ask folders(): each reads what the last answer said.
  std::atomic<bool> limited_{false};
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

  // The service's mkdir (serve.cpp make_folder), on this phone.
  expected make_folder(const std::string& root, const std::string& rel) override {
    if (root != root_.id || !driftwire::safe_relative(rel)) return err(status::permission_denied);
    if (auto st = io::stat_beneath(root_.path, rel)) {
      return st->is_directory ? expected{} : expected(err(status::invalid_arg));
    }
    std::error_code ec;
    std::filesystem::create_directories(fs_path(root_.path), ec);
    auto made = io::make_directories_beneath(root_.path, rel);
    if (!made) return err(made.error());
    return {};
  }

  // The service's delete (serve.cpp trash_entry), on this phone: into the folder's hidden trash,
  // `<kTrashFolder>/<YYYY-MM-DD>/<rel>`, by a rename; a taken name gets a number.
  result<bool> trash(const std::string& root, const std::string& rel) override {
    if (root != root_.id || !driftwire::safe_relative(rel)) return err(status::permission_denied);
    const std::string from = io::join_path(root_.path, io::native_relative(rel));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fs_path(from), ec)) return false;
    using namespace std::chrono;
    const year_month_day d{floor<days>(system_clock::now())};
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02u-%02u", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                  static_cast<unsigned>(d.day()));
    const auto slash = rel.rfind('/');
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    const std::string in_trash = std::string(serve::kTrashFolder) + "/" + day + (slash == std::string::npos ? "" : "/" + rel.substr(0, slash));
    auto dir = io::make_directories_beneath(root_.path, in_trash);
    if (!dir) return err(dir.error());
    for (int n = 1; n <= serve::backup::kMaxNumbered; ++n) {
      auto renamed = io::rename_no_replace(from, io::join_path(*dir, serve::backup::numbered(name, n)));
      if (!renamed) return err(renamed.error());
      if (*renamed == io::rename_outcome::renamed) return true;
    }
    return err(status::busy);
  }

  // The service's write, step for step (serve.cpp open_write and commit), on this phone.
  result<serve::client::write_done> write(const std::string& root, const std::string& rel_in, std::uint64_t size,
                                          const std::string& hash_hex, std::uint64_t at, const serve::client::read_fn& read,
                                          const std::function<void(std::uint64_t)>& progress, const cancel_flag* cancel,
                                          serve::client::write_refused* why, bool meta, std::int64_t taken) override {
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
        if (taken != serve::kNoTime) (void)io::set_file_times(io::join_path(dir.value(), *found), taken);
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
    if (!meta && taken != serve::kNoTime && (done.k == kind::placed || done.k == kind::existing)) {
      (void)io::set_file_times(io::join_path(dir.value(), done.name), taken);
    }
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

std::optional<destination> parse_destination(std::string_view dest) {
  const auto slash = dest.find('/');
  destination d{std::string(dest.substr(0, slash)), slash == std::string_view::npos ? std::string() : std::string(dest.substr(slash + 1))};
  if (d.root.empty() || d.root.size() > 128) return std::nullopt;
  if (slash != std::string_view::npos && !driftwire::safe_relative(d.sub)) return std::nullopt;
  return d;
}

result<std::vector<std::string>> subfolders(target& t, std::string_view dest) {
  const auto d = parse_destination(dest);
  if (!d) return err(status::invalid_arg);
  auto listing = t.list(d->root, d->sub);
  if (!listing) return err(listing.error());
  std::vector<std::string> out;
  for (const auto& e : *listing) {
    if (e.dir && valid_name(e.name)) out.push_back(e.name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

result<std::string> make_subfolder(target& t, std::string_view dest, std::string_view name) {
  const auto d = parse_destination(dest);
  if (!d) return err(status::invalid_arg);
  if (!valid_name(name)) return err(status::invalid_arg);
  const std::string rel = d->sub.empty() ? std::string(name) : d->sub + "/" + std::string(name);
  if (auto made = t.make_folder(d->root, rel); !made && made.error() != status::unsupported_format) {
    return err(made.error());
  }
  return d->root + "/" + rel;
}

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

engine::engine(options o, target& t, source& s, ledger& l)
    : o_(std::move(o)), d_(parse_destination(o_.root).value_or(destination{})), t_(t), s_(s), l_(l) {}

std::string engine::in_dest(const std::string& rel) const {
  if (d_.sub.empty()) return rel;
  return rel.empty() ? d_.sub : d_.sub + "/" + rel;
}

void engine::set_items(std::vector<item> items) { items_ = std::move(items); }

void engine::set_deleted(std::vector<std::string> keys) { deleted_ = std::move(keys); }

std::vector<std::string> engine::deleted_done() const {
  std::lock_guard lock(m_);
  return deleted_done_;
}

void engine::trash_deleted(const cancel_flag* cancel) {
  if (deleted_.empty()) return;
  std::set<std::string> in_photos;
  for (const auto& it : items_) in_photos.insert(it.key);
  std::map<std::string, std::vector<row>> rows;
  const std::set<std::string> wanted(deleted_.begin(), deleted_.end());
  for (auto& r : l_.all(o_.root)) {
    if (wanted.count(r.key)) rows[r.key].push_back(std::move(r));
  }
  // Which of them are really to go: backed up here, and not in Photos again (restored from
  // Recently Deleted: its copy stays).
  std::set<std::string> going;
  for (const auto& key : deleted_) {
    if (!in_photos.count(key) && !rows[key].empty()) going.insert(key);
  }
  // Many at once is not someone deleting photos: iCloud Photos turned off with "Remove from
  // iPhone", signing out of iCloud, a library reset. Their backups are exactly what a backup is
  // for, so they stay where they are and the keys are done with (never mirrored later either).
  const bool bulk = going.size() > kMaxDeletedAtOnce;
  // A file another photo still in the ledger relies on (the same picture twice in Photos is sent
  // once and recorded for both): it stays for that one; only this photo's row goes.
  std::set<std::string> relied_on;
  for (const auto& r : l_.all(o_.root)) {
    if (!going.count(r.key)) relied_on.insert(r.rel);
  }
  bool changed = false;
  bool out_of_reach = false;
  for (const auto& key : deleted_) {
    if (cancel && cancel->stopped()) break;
    bool finished = true;
    if (going.count(key) && !bulk) {
      for (const auto& r : rows[key]) {
        if (!relied_on.count(r.rel)) {
          auto moved = t_.trash(d_.root, r.rel);
          if (!moved) {
            finished = false;
            // Refused for this one file (not allowed, a name the service will not move): it
            // waits, and the others are not held up behind it. Anything else is the folder,
            // the service or the link: all of it next time.
            if (moved.error() != status::permission_denied) out_of_reach = true;
            break;
          }
          if (*moved) ++st_.trashed;
        }
        (void)l_.remove(r.root, r.key, r.kind, r.name);
        changed = true;
      }
    }
    if (out_of_reach) break;
    if (!finished) continue;
    std::lock_guard lock(m_);
    deleted_done_.push_back(key);
  }
  if (changed) copy_ledger();
}

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
  auto ids = t_.ledger_ids(d_.root);
  if (!ids) return false;
  for (const auto& id : *ids) {
    auto text = t_.read_ledger(d_.root, id);
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
  if (auto top = t_.list(d_.root, ""); !top && top.error() != status::not_found) return err(top.error());
  // One listing per folder (§10b).
  for (const auto& d : dirs) {
    auto listing = t_.list(d_.root, d);
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
    auto r = t_.write(d_.root, t_.device_id() + "/" + serve::kLedgerFile, text.size(), hash, at, read, {}, nullptr, &why, true);
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

  if (d_.root.empty()) return stop_with(state::no_folder);  // not a destination
  auto folders = t_.folders();
  if (!folders) {
    if (folders.error() == status::unsupported_format) return stop_with(state::too_old);
    return stop_with(state::unreachable);
  }
  const auto chosen = std::find_if(folders->begin(), folders->end(), [&](const folder& f) { return f.id == d_.root; });
  // Gone, or (to a phone limited to some folders) no longer named to it: the two look the same
  // from here, so "choose another", whose list then says which.
  if (chosen == folders->end()) return stop_with(state::no_folder);
  if (!chosen->writable) return stop_with(state::not_allowed);
  if (!restore_if_empty()) return stop_with(state::unreachable);
  if (auto checked = check_folder(); !checked) {
    return stop_with(checked.error() == status::permission_denied ? state::not_allowed : state::unreachable);
  }
  trash_deleted(cancel);

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
  // Up to uploads_at_once files on their way at once, each from its own scratch file: a file
  // waits a round trip for the service's first credit and another for its confirmation, so one
  // at a time left the link idle between small photos. The next file is exported and hashed
  // while the others send. Everything but the sending (the ledger, the report, the state) stays
  // on this thread.
  const std::size_t lanes = t_.writes_at_once() ? static_cast<std::size_t>(std::max(1, o_.uploads_at_once)) : 1;
  struct upload {
    std::size_t item = 0;
    std::string key, kind, name, rel, hash, scratch;
    std::uint64_t size = 0, at = 0;
    std::int64_t taken = serve::kNoTime;
    std::size_t lane = 0;
    io::file_reader in;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<bool> finished{false};
    result<serve::client::write_done> written = err(status::io);
    serve::client::write_refused why;
    std::thread t;
  };
  std::vector<std::string> scratch_of(lanes);
  for (std::size_t i = 0; i < lanes; ++i) {
    scratch_of[i] = io::join_path(o_.scratch_dir, i == 0 ? std::string("backup-current") : "backup-current-" + std::to_string(i));
  }
  std::vector<bool> lane_busy(lanes, false);
  std::vector<std::unique_ptr<upload>> sending;
  std::mutex um;
  std::condition_variable ucv;
  std::uint64_t events = 0;  // under um: a file finished, or sent more
  struct item_track {
    int pending = 0;
    bool whole = true;
    bool queued_all = false;
  };
  std::map<std::size_t, item_track> tracks;
  int since_copy = 0;
  bool dirty = false;
  std::optional<state> halt;  // the first state that stops the run
  bool halt_copies_ledger = false;
  upload* shown = nullptr;    // the file st_.current names

  struct scrub {
    const std::vector<std::string>& p;
    ~scrub() {
      std::error_code e;
      for (const auto& x : p) std::filesystem::remove(fs_path(x), e);
    }
  } scrubbed{scratch_of};

  const auto item_finished = [&](std::size_t idx) {
    const auto t = tracks.find(idx);
    if (t == tracks.end() || !t->second.queued_all || t->second.pending > 0) return;
    if (t->second.whole && item_done(items_[idx])) {
      ++st_.backed_up;
      std::lock_guard lock(m_);
      std::erase(remaining_, items_[idx].key);
    }
    tracks.erase(t);
    say();
  };
  // A sent file's answer, on this thread.
  const auto settle = [&](upload& u) {
    auto& track = tracks[u.item];
    --track.pending;
    const auto& written = u.written;
    const auto& why = u.why;
    if (!written) {
      track.whole = false;
      std::optional<state> stop;
      // After a cancel, whatever the others ended with is the cancel's doing (one stream
      // stopped can end another's read): stopped, not unreachable.
      if (written.error() == status::cancelled || (cancel && cancel->stopped())) {
        stop = state::idle;
      } else if (why.why == serve::kWhyLimit || why.why == serve::kWhyFull || why.why == serve::kWhyNotAllowed) {
        stop = why.why == serve::kWhyLimit ? state::limit : why.why == serve::kWhyFull ? state::full : state::not_allowed;
        halt_copies_ledger = halt_copies_ledger || !halt;
      } else if (written.error() == status::io || written.error() == status::timeout) {
        stop = state::unreachable;
      } else if (written.error() == status::unsupported_format) {
        stop = state::too_old;
      } else {
        st_.failures.push_back({u.key, u.name, why.why == serve::kWhyHash ? "hash" : "error"});
      }
      if (stop && !halt) halt = stop;
    } else if (auto added = l_.add({u.key, u.kind, u.name, u.size, u.hash, o_.root, written->name, now_unix()}); !added) {
      // Only the service's confirmation makes a row (it hashed what reached its disk).
      st_.failures.push_back({u.key, u.name, "error"});
      track.whole = false;
    } else {
      dirty = true;
      ++st_.copied;
      ++st_.files_done;
      st_.bytes += written->existing ? 0 : u.size - u.at;
      if (++since_copy >= o_.ledger_copy_every) {
        since_copy = 0;
        copy_ledger();
      }
    }
    item_finished(u.item);
  };
  const auto scratch_in_use = [&] {
    std::uint64_t n = 0;
    for (const auto& u : sending) n += u->size;
    return n;
  };
  // Settles what has finished; then, while `full()` holds, waits for more to (showing progress).
  const auto reap_while = [&](const std::function<bool()>& full) {
    for (;;) {
      std::uint64_t seen = 0;
      {
        std::lock_guard lock(um);
        seen = events;  // before looking: what happens after wakes the wait below
      }
      for (auto it = sending.begin(); it != sending.end();) {
        if (!(*it)->finished.load()) {
          ++it;
          continue;
        }
        auto u = std::move(*it);
        it = sending.erase(it);
        u->t.join();
        lane_busy[u->lane] = false;
        std::filesystem::remove(fs_path(u->scratch), ec);
        if (shown == u.get()) shown = nullptr;
        settle(*u);
      }
      if (sending.empty() || !full()) return;
      std::unique_lock lock(um);
      ucv.wait(lock, [&] { return events != seen; });
      lock.unlock();
      if (shown && shown->size) {
        st_.current_fraction = static_cast<double>(shown->sent.load()) / static_cast<double>(shown->size);
        say();
      }
    }
  };
  const auto notify = [&] {
    {
      std::lock_guard lock(um);
      ++events;
    }
    ucv.notify_all();
  };

  for (std::size_t idx = 0; idx < items_.size() && !halt; ++idx) {
    const auto& it = items_[idx];
    if (cancel && cancel->stopped()) {
      halt = state::idle;  // leaving the app: no ledger upload now; the next run sends it
      break;
    }
    if (item_done(it)) continue;
    tracks[idx];
    int ymd[3] = {it.year, it.month, it.day};
    for (std::size_t i = 0; i < it.resources.size() && !halt; ++i) {
      const auto& res = it.resources[i];
      if (l_.find(o_.root, it.key, res.kind, res.name)) continue;
      // A lane free, and (past the first) the scratch files of those still sending under the
      // budget: a phone's free space is spent on one file and parallel_bytes more at most.
      reap_while([&] {
        return sending.size() >= lanes || scratch_in_use() >= static_cast<std::uint64_t>(o_.parallel_bytes);
      });
      if (halt || !tracks[idx].whole) break;
      const std::size_t lane = static_cast<std::size_t>(std::find(lane_busy.begin(), lane_busy.end(), false) - lane_busy.begin());
      const std::string& scratch = scratch_of[lane];
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
          halt = state::idle;
          break;
        }
        st_.failures.push_back({it.key, res.name, exported.error() == status::not_found ? "gone" : "export"});
        tracks[idx].whole = false;
        break;
      }
      auto u = std::make_unique<upload>();
      if (auto o = u->in.open(scratch, io::read_mode::sequential); !o) {
        st_.failures.push_back({it.key, res.name, "export"});
        tracks[idx].whole = false;
        break;
      }
      u->size = u->in.size();
      auto hashed = serve::backup::hash_file(scratch);
      if (!hashed) {
        st_.failures.push_back({it.key, res.name, "export"});
        tracks[idx].whole = false;
        break;
      }
      u->hash = hashed.value();
      // The same file under another key (a local id that changed): already there.
      if (const auto same = l_.find_same(o_.root, res.name, u->size, u->hash)) {
        if (l_.add({it.key, res.kind, res.name, u->size, u->hash, o_.root, same->rel, now_unix()})) {
          ++st_.files_done;
          dirty = true;
        }
        continue;
      }
      const std::string dir = subdir(ymd[0], ymd[1], ymd[2], o_.folders);
      u->rel = in_dest(dir.empty() ? res.name : dir + "/" + res.name);
      // One of this name in this folder already on its way (a duplicated photo, two IMG_0001.JPG
      // in a month): the service holds that name until it is done, so this one waits for it,
      // then is either the same file (in the ledger now) or the service's " (2)", as one at a
      // time.
      const auto same_place = [&] {
        return std::any_of(sending.begin(), sending.end(), [&](const auto& x) { return x->rel == u->rel; });
      };
      if (same_place()) {
        reap_while(same_place);
        if (halt) break;
        if (const auto same = l_.find_same(o_.root, res.name, u->size, u->hash)) {
          if (l_.add({it.key, res.kind, res.name, u->size, u->hash, o_.root, same->rel, now_unix()})) {
            ++st_.files_done;
            dirty = true;
          }
          continue;
        }
      }
      u->item = idx;
      u->key = it.key;
      u->kind = res.kind;
      u->name = res.name;
      u->taken = it.taken;
      u->scratch = scratch;
      u->lane = lane;
      lane_busy[lane] = true;
      ++tracks[idx].pending;
      upload* raw = u.get();
      shown = raw;
      u->t = std::thread([this, raw, cancel, &notify] {
        const auto read = file_read_fn(raw->in);
        for (int attempt = 0; attempt < 4; ++attempt) {
          raw->written = t_.write(d_.root, raw->rel, raw->size, raw->hash, raw->at, read,
                                  [&](std::uint64_t sent) {
                                    raw->sent.store(sent);
                                    notify();
                                  },
                                  cancel, &raw->why, false, raw->taken);
          if (raw->written) break;
          if (raw->why.why == serve::kWhyOffset) {
            raw->at = raw->why.have;  // this phone's unfinished copy from an earlier run
            continue;
          }
          if (raw->why.why == serve::kWhyHash) {
            raw->at = 0;  // the service removed its copy: once more from the start
            continue;
          }
          break;
        }
        raw->finished.store(true);
        notify();
      });
      sending.push_back(std::move(u));
    }
    tracks[idx].queued_all = true;
    item_finished(idx);
  }
  // What is on its way finishes (or fails with the link) before the run says how it ended.
  reap_while([] { return true; });
  if (halt) {
    if (*halt != state::idle && halt_copies_ledger && dirty) copy_ledger();
    return stop_with(*halt);
  }
  if (dirty || restored_) copy_ledger();
  return stop_with(st_.failures.empty() ? state::done : state::done_failures);
}

}  // namespace mv::mobile::backup
