// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sound text tower's download (docs/plans/mobile.md §9, "Sound search"; sound_tower.h): each
// pinned file into <library>/sound/clap-general/<name>.part, resumed by range, hashed whole and
// kept only if it is the pinned file, then verified.txt. Bytes move only while `allowed` says so
// (the phone's Wi-Fi rule), and stop at once when it no longer does.
#include <sodium.h>
#include <sqlite3.h>
#include <sys/mount.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "addons/ai/catalog.h"
#include "addons/ai/index_db.h"
#include "mobile/sound_fetch.h"
#include "mobile/sound_tower.h"

namespace mv::mobile::sound {
namespace {

namespace fs = std::filesystem;

std::string sha256_file_hex(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = f.gcount();
    if (n > 0) crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(n));
  }
  if (f.bad()) return {};
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, h);
  return ai::catalog::to_hex(std::span<const std::uint8_t>(h, sizeof h));
}

std::uint64_t free_bytes(const fs::path& dir) {
  struct statfs st{};
  if (::statfs(dir.c_str(), &st) != 0) return 0;
  return static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_bsize);
}

std::uint64_t size_or_zero(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : n;
}

struct transfer {
  std::FILE* out = nullptr;
  std::uint64_t done_before = 0;  // every earlier file's bytes, and this one's part already here
  std::uint64_t written = 0;
  bool write_failed = false;
  const cancel_flag* cancel = nullptr;
  mv_sync_allowed_fn allowed = nullptr;
  mv_sync_progress_fn progress = nullptr;
  void* ctx = nullptr;
  bool refused = false;  // `allowed` said no: bytes may not move now
  std::uint64_t total = 0;

  bool go_on() {
    if (cancel && cancel->stopped()) return false;
    if (allowed && allowed(ctx) == 0) {
      refused = true;
      return false;
    }
    return true;
  }
};

int32_t sink_bytes(void* ctx, uint64_t, const uint8_t* data, size_t size) {
  auto* t = static_cast<transfer*>(ctx);
  if (!t->go_on()) return 0;
  if (size && std::fwrite(data, 1, size, t->out) != size) {
    t->write_failed = true;
    return 0;
  }
  t->written += size;
  if (t->progress) t->progress(t->ctx, t->done_before + t->written, t->total);
  return 1;
}

int32_t stopped_now(void* ctx) { return static_cast<transfer*>(ctx)->go_on() ? 0 : 1; }

bool write_verified(const fs::path& dir, pin files) {
  const fs::path part = dir / (std::string(kVerified) + ".part");
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    const std::string text = verified_text(files);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(part, dir / std::string(kVerified), ec);
  return !ec;
}

// Anything in the folder that is not this pin's file or its part: an older pin's leftovers.
void remove_strangers(const fs::path& dir, pin files) {
  std::error_code ec;
  std::vector<fs::path> gone;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    bool ours = name == kVerified;
    for (const pinned_file& f : files) {
      ours = ours || name == f.name || name == std::string(f.name) + ".part";
    }
    if (!ours) gone.push_back(it->path());
  }
  for (const fs::path& p : gone) fs::remove_all(p, ec);
}

}  // namespace

int32_t fetch_all(const std::string& library_dir, const std::string& base, mv_fetch_fn fetch, void* fetch_ctx,
                  const cancel_flag* cancel, mv_sync_allowed_fn allowed, mv_sync_progress_fn progress, void* ctx,
                  pin files) {
  const std::string dir_utf8 = folder(library_dir);
  if (ready(dir_utf8, files)) return MV_LINK_OK;
  const fs::path dir = utf8_path(dir_utf8);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return MV_LINK_INTERNAL;
  fs::remove(dir / std::string(kVerified), ec);  // an older pin's record
  remove_strangers(dir, files);

  // What is already here counts; what is still to come must fit, with room to spare.
  std::uint64_t have = 0;
  for (const pinned_file& f : files) {
    const fs::path whole = dir / std::string(f.name);
    have += size_or_zero(whole) == f.size ? f.size : std::min(size_or_zero(dir / (std::string(f.name) + ".part")), f.size);
  }
  const std::uint64_t still = total_bytes(files) - have;
  if (still > 0 && free_bytes(dir) < still + (64ull << 20)) return MV_LINK_NO_SPACE;

  transfer t;
  t.cancel = cancel;
  t.allowed = allowed;
  t.progress = progress;
  t.ctx = ctx;
  t.total = total_bytes(files);
  std::uint64_t done = 0;
  for (const pinned_file& f : files) {
    const std::string name(f.name);
    const fs::path whole = dir / name;
    const fs::path part = dir / (name + ".part");
    if (size_or_zero(whole) == f.size && sha256_file_hex(whole) == f.sha256) {
      done += f.size;
      continue;
    }
    fs::remove(whole, ec);
    std::uint64_t at = size_or_zero(part);
    if (at > f.size) {
      fs::remove(part, ec);
      at = 0;
    }
    if (at < f.size) {
      if (!t.go_on()) return t.refused ? MV_LINK_WAITING : MV_LINK_CANCELLED;
      t.out = std::fopen(part.c_str(), "ab");
      if (!t.out) return MV_LINK_INTERNAL;
      t.done_before = done + at;
      t.written = 0;
      t.write_failed = false;
      const std::string url = base + name;
      const int32_t got = fetch(fetch_ctx, url.c_str(), at, f.size - at, &sink_bytes, &t, &stopped_now, &t);
      const bool closed = std::fclose(t.out) == 0;
      t.out = nullptr;
      if (t.refused) return MV_LINK_WAITING;
      if (cancel && cancel->stopped()) return MV_LINK_CANCELLED;
      if (t.write_failed || !closed) return free_bytes(dir) < (f.size - at) ? MV_LINK_NO_SPACE : MV_LINK_INTERNAL;
      if (got == MV_LINK_NOT_FOUND) return MV_LINK_NOT_FOUND;  // the pin is gone: an app update names another
      if (got != MV_LINK_OK) return got == MV_LINK_CANCELLED ? MV_LINK_CANCELLED : MV_LINK_UNREACHABLE;
      if (size_or_zero(part) != f.size) {
        // A server that sent more than was asked for, or less: nothing of it is resumed.
        if (size_or_zero(part) > f.size) fs::remove(part, ec);
        return MV_LINK_UNREACHABLE;
      }
    }
    if (sha256_file_hex(part) != f.sha256) {
      fs::remove(part, ec);  // nothing of a file that is not the pinned one is kept or resumed
      return MV_LINK_REJECTED;
    }
    fs::rename(part, whole, ec);
    if (ec) return MV_LINK_INTERNAL;
    done += f.size;
    if (progress) progress(ctx, done, t.total);
  }
  if (!write_verified(dir, files)) return MV_LINK_INTERNAL;
  return ready(dir_utf8, files) ? MV_LINK_OK : MV_LINK_INTERNAL;
}


bool wanted(const std::string& generation_dir) {
  const std::string db_path = generation_dir + "/catalog.db";
  std::error_code ec;
  if (!fs::exists(utf8_path(db_path), ec)) return false;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(ai::immutable_uri(db_path).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }
  bool found = false;
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM frames WHERE spec = ?1 LIMIT 1", -1, &q, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(q, 1, kSpec.data(), static_cast<int>(kSpec.size()), SQLITE_STATIC);
    found = sqlite3_step(q) == SQLITE_ROW;
  }
  sqlite3_finalize(q);
  sqlite3_close(db);
  return found;
}

}  // namespace mv::mobile::sound
