// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/thumb.h"

#include <cstdio>
#include <cstring>

#include <sqlite3.h>

#include "codec/decode.h"
#include "codec/format.h"
#include "image/pipeline.h"
#include "io/file.h"

namespace mv::image {
namespace {

constexpr char kSchema[] =
    "CREATE TABLE IF NOT EXISTS thumbs ("
    "  path TEXT NOT NULL,"
    "  mtime INTEGER NOT NULL,"
    "  size INTEGER NOT NULL,"
    "  spec TEXT NOT NULL,"
    "  file TEXT NOT NULL,"
    "  PRIMARY KEY (path, mtime, size, spec)"
    ");";

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string join_dir(std::string_view dir, std::string_view file) {
  if (dir.empty()) return std::string(file);
  const char last = dir.back();
  std::string out;
  out.reserve(dir.size() + 1 + file.size());
  out.append(dir);
  if (last != '\\' && last != '/') out.push_back('\\');
  out.append(file);
  return out;
}

std::string cache_name(const thumb_key& key) {
  std::string material;
  material.reserve(key.path.size() + 48);
  material.append(key.path);
  material.push_back('|');
  material.append(std::to_string(key.mtime_unix));
  material.push_back('|');
  material.append(std::to_string(key.size));
  material.push_back('|');
  material.append(kThumbSpec);
  const std::uint64_t h = fnv1a(material);
  char name[32]{};
  std::snprintf(name, sizeof(name), "%016llx.jpg", static_cast<unsigned long long>(h));
  return name;
}

void box_fit_rgba(const display_image& src, std::uint32_t dst_w, std::uint32_t dst_h,
                  std::vector<std::uint8_t>& dst) {
  dst.assign(static_cast<std::size_t>(dst_w) * dst_h * 4u, 0);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    const std::uint32_t y0 = y * src.height / dst_h;
    const std::uint32_t y1 = ((y + 1) * src.height + dst_h - 1) / dst_h;
    const std::uint32_t yb = y1 > y0 ? y1 : y0 + 1;
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const std::uint32_t x0 = x * src.width / dst_w;
      const std::uint32_t x1 = ((x + 1) * src.width + dst_w - 1) / dst_w;
      const std::uint32_t xb = x1 > x0 ? x1 : x0 + 1;
      std::uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
      for (std::uint32_t sy = y0; sy < yb && sy < src.height; ++sy) {
        const std::uint8_t* row = src.rgba.data() + static_cast<std::size_t>(sy) * src.width * 4u;
        for (std::uint32_t sx = x0; sx < xb && sx < src.width; ++sx) {
          r += row[sx * 4u + 0];
          g += row[sx * 4u + 1];
          b += row[sx * 4u + 2];
          a += row[sx * 4u + 3];
          ++n;
        }
      }
      if (n == 0) n = 1;
      std::uint8_t* p = dst.data() + (static_cast<std::size_t>(y) * dst_w + x) * 4u;
      p[0] = static_cast<std::uint8_t>(r / n);
      p[1] = static_cast<std::uint8_t>(g / n);
      p[2] = static_cast<std::uint8_t>(b / n);
      p[3] = static_cast<std::uint8_t>(a / n);
    }
  }
}

}  // namespace

thumb_store::~thumb_store() { close(); }

bool thumb_store::is_open() const noexcept {
  std::lock_guard lock(mutex_);
  return db_ != nullptr;
}

expected thumb_store::open(std::string_view dir_utf8) {
  if (dir_utf8.empty()) return err(status::invalid_arg);

  std::lock_guard lock(mutex_);
  // Already serving this directory. Reopening would close a handle that
  // lookups on other pool threads are about to use.
  if (db_ != nullptr && dir_ == dir_utf8) return {};
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  dir_.assign(dir_utf8);
  const std::string db_path = join_dir(dir_, "thumbs.sqlite");
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return err(status::io);
  }
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return err(status::io);
  }
  db_ = db;
  return {};
}

void thumb_store::close() noexcept {
  std::lock_guard lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  dir_.clear();
}

result<std::string> thumb_store::lookup(const thumb_key& key) {
  std::lock_guard lock(mutex_);
  if (!db_) return err(status::internal);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT file FROM thumbs WHERE path=? AND mtime=? AND size=? AND spec=?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_bind_text(stmt, 1, key.path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, key.mtime_unix);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(key.size));
  sqlite3_bind_text(stmt, 4, kThumbSpec, -1, SQLITE_STATIC);
  std::string file;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(stmt, 0);
    if (t) file.assign(reinterpret_cast<const char*>(t));
  }
  sqlite3_finalize(stmt);
  if (file.empty()) return std::string{};

  // The row is not proof the bytes survived. Clearing the thumbs directory
  // while thumbs.sqlite lives on used to leave every item pointing at a file
  // that is not there, with no path back to regenerating it.
  std::string path = join_dir(dir_, file);
  if (!io::file_exists(path)) {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "DELETE FROM thumbs WHERE path=? AND mtime=? AND size=? AND spec=?;",
                           -1, &del, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(del, 1, key.path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(del, 2, key.mtime_unix);
      sqlite3_bind_int64(del, 3, static_cast<sqlite3_int64>(key.size));
      sqlite3_bind_text(del, 4, kThumbSpec, -1, SQLITE_STATIC);
      sqlite3_step(del);
      sqlite3_finalize(del);
    }
    return std::string{};
  }
  return path;
}

result<std::string> thumb_store::store(const thumb_key& key, std::span<const std::uint8_t> jpeg) {
  if (jpeg.empty()) return err(status::invalid_arg);

  // Snapshot the directory, then write the file with the lock released: every
  // pool thread stores thumbs, and serializing them on one mutex would undo
  // the point of having a pool.
  std::string file = cache_name(key);
  std::string path;
  {
    std::lock_guard lock(mutex_);
    if (!db_) return err(status::invalid_arg);
    path = join_dir(dir_, file);
  }
  auto written = io::write_all(path, jpeg);
  if (!written) return err(written.error());

  std::lock_guard lock(mutex_);
  if (!db_) return err(status::internal);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT OR REPLACE INTO thumbs(path,mtime,size,spec,file) "
                         "VALUES(?,?,?,?,?);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_bind_text(stmt, 1, key.path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, key.mtime_unix);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(key.size));
  sqlite3_bind_text(stmt, 4, kThumbSpec, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, file.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return err(status::io);
  return path;
}

result<std::vector<std::uint8_t>> make_thumb_jpeg(std::span<const std::uint8_t> src_bytes,
                                                  const job_context* ctx) {
  result<display_image> decoded = err(status::unsupported_format);
  if (codec::probe(src_bytes) == codec::format_family::jpeg) {
    // Pick the coarsest DCT scale that still leaves at least kThumbLongEdge to
    // downsample from. The old fixed {8, 4, 1} ladder took the first scale that
    // decoded, so a 1024px JPEG produced a 128px "512" thumb.
    int denom = 1;
    if (auto size = codec::jpeg_dimensions(src_bytes)) {
      const std::uint32_t edge = size.value().width > size.value().height ? size.value().width
                                                                          : size.value().height;
      for (int candidate : {8, 4, 2}) {
        if (edge / static_cast<std::uint32_t>(candidate) >= kThumbLongEdge) {
          denom = candidate;
          break;
        }
      }
    }
    for (int attempt : {denom, 1}) {
      auto raster = codec::decode_jpeg(src_bytes, ctx, attempt);
      if (!raster) {
        if (raster.error() == status::cancelled) return err(status::cancelled);
        continue;
      }
      decoded = to_display(std::move(raster).value(), ctx);
      if (decoded) break;
      if (decoded.error() == status::cancelled) return err(status::cancelled);
    }
  }
  if (!decoded) {
    decoded = decode_bytes(src_bytes, ctx);
    if (!decoded) return err(decoded.error());
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  display_image& img = decoded.value();
  std::uint32_t dw = img.width;
  std::uint32_t dh = img.height;
  if (dw == 0 || dh == 0) return err(status::corrupt);
  const std::uint32_t long_edge = dw > dh ? dw : dh;
  std::vector<std::uint8_t> rgba;
  const std::uint8_t* pixels = img.rgba.data();
  std::uint32_t pw = dw;
  std::uint32_t ph = dh;
  if (long_edge > kThumbLongEdge) {
    dw = dw * kThumbLongEdge / long_edge;
    dh = dh * kThumbLongEdge / long_edge;
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;
    box_fit_rgba(img, dw, dh, rgba);
    pixels = rgba.data();
    pw = dw;
    ph = dh;
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return codec::encode_jpeg_rgba(std::span<const std::uint8_t>(pixels, static_cast<std::size_t>(pw) * ph * 4u),
                                 pw, ph, kThumbJpegQuality);
}

}  // namespace mv::image
