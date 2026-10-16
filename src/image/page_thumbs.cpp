// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/page_thumbs.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "codec/decode.h"
#include "io/file.h"

namespace mv::image {
namespace {

struct render_state {
  thumb_store* store = nullptr;
  const thumb_key* file = nullptr;
  void* user = nullptr;
  page_thumb_ready ready = nullptr;
  status failure = status::ok;
  bool stopped = false;
};

// One rendered page: a document page is opaque sRGB on white (codec/decode.h),
// so it is encoded as it is, with no colour step and no matte.
bool on_page(void* user, codec::raster&& page) {
  auto& st = *static_cast<render_state*>(user);
  auto jpeg = encode_thumb_rgba(page.rgba, page.width, page.height);
  if (!jpeg) return true;  // this page stays empty; the others still come
  const thumb_key key =
      page_thumb_key(st.file->path, page.page, st.file->mtime_unix, st.file->size);
  auto stored = st.store->store(key, jpeg.value());
  if (!stored) {
    st.failure = stored.error();
    return false;  // the cache is unusable: nothing after this would land either
  }
  if (!st.ready(st.user, page.page, stored.value())) {
    st.stopped = true;
    return false;
  }
  return true;
}

}  // namespace

status make_page_thumbs(thumb_store& store, const thumb_key& file,
                        std::span<const std::uint32_t> pages, const job_context* ctx, void* user,
                        page_thumb_ready ready, std::uint32_t* out_page_count) {
  if (out_page_count) *out_page_count = 0;
  if (!ready) return status::invalid_arg;
  std::vector<std::uint32_t> misses;
  misses.reserve(pages.size());
  for (const std::uint32_t page : pages) {
    if (ctx && ctx->cancelled()) return status::cancelled;
    auto hit = store.lookup(page_thumb_key(file.path, page, file.mtime_unix, file.size));
    if (hit && !hit.value().empty()) {
      if (!ready(user, page, hit.value())) return status::cancelled;
    } else {
      misses.push_back(page);
    }
  }
  if (misses.empty()) return status::ok;

  auto bytes = io::read_all(file.path);
  if (!bytes) return bytes.error();
  if (ctx && ctx->cancelled()) return status::cancelled;
  render_state st{&store, &file, user, ready};
  auto count = codec::decode_pages(bytes.value(), misses, kPageThumbLongEdge, ctx, &st, on_page);
  if (!count) return count.error();
  if (out_page_count) *out_page_count = count.value();
  if (st.stopped) return status::cancelled;
  return st.failure;
}

page_thumb_book::request page_thumb_book::want(const std::string& file, std::uint32_t first,
                                                std::uint32_t count) {
  request out;
  count = std::min(count, kMaxWindow);
  std::lock_guard lock(mutex_);
  if (file != file_) {
    file_ = file;
    ++serial_;
    paths_.clear();
    pending_.clear();
  }
  out.serial = serial_;
  if (count == 0) return out;
  want_first_ = first;
  want_last_ = first + std::min(count - 1, UINT32_MAX - first);
  for (std::uint32_t p = first; p <= want_last_; ++p) {
    if (!paths_.contains(p) && pending_.insert(p).second) out.pages.push_back(p);
    if (p == UINT32_MAX) break;
  }
  return out;
}

std::vector<std::uint32_t> page_thumb_book::still_wanted(std::uint32_t serial,
                                                         std::span<const std::uint32_t> pages) {
  std::vector<std::uint32_t> out;
  std::lock_guard lock(mutex_);
  if (serial != serial_) return out;
  const std::uint32_t lo = want_first_ > kSlack ? want_first_ - kSlack : 0;
  const std::uint32_t hi = want_last_ > UINT32_MAX - kSlack ? UINT32_MAX : want_last_ + kSlack;
  for (const std::uint32_t p : pages) {
    if (p >= lo && p <= hi) {
      out.push_back(p);
    } else {
      pending_.erase(p);
    }
  }
  return out;
}

bool page_thumb_book::landed(std::uint32_t serial, std::uint32_t page, std::string_view jpeg_path) {
  std::lock_guard lock(mutex_);
  if (serial != serial_) return false;
  pending_.erase(page);
  paths_[page] = std::string(jpeg_path);
  return true;
}

void page_thumb_book::finished(std::uint32_t serial, std::span<const std::uint32_t> pages) {
  std::lock_guard lock(mutex_);
  if (serial != serial_) return;
  for (const std::uint32_t p : pages) pending_.erase(p);
}

std::string page_thumb_book::path(std::uint32_t page) const {
  std::lock_guard lock(mutex_);
  const auto it = paths_.find(page);
  return it == paths_.end() ? std::string() : it->second;
}

std::uint32_t page_thumb_book::serial() const {
  std::lock_guard lock(mutex_);
  return serial_;
}

void page_thumb_book::clear() {
  std::lock_guard lock(mutex_);
  file_.clear();
  ++serial_;
  paths_.clear();
  pending_.clear();
}

}  // namespace mv::image
