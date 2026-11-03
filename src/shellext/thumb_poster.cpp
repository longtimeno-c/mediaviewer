// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shellext/thumb_poster.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "shellext/thumb_runner.h"

namespace mv::shellext {
namespace {

class poster_work final : public detail::thumb_work {
 public:
  poster_work(std::unique_ptr<player::byte_source>&& source, std::uint32_t cx) noexcept
      : source_(std::move(source)), cx_(cx) {}
  result<bgra_thumb> run(const job_context& ctx) override {
    return render_poster(*source_, cx_, &ctx);
  }

 private:
  std::unique_ptr<player::byte_source> source_;
  std::uint32_t cx_;
};

int seek_to(std::FILE* f, std::uint64_t offset) noexcept {
#if defined(_WIN32)
  return ::_fseeki64(f, static_cast<__int64>(offset), SEEK_SET);
#else
  return ::fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::uint64_t tell(std::FILE* f) noexcept {
#if defined(_WIN32)
  const __int64 at = ::_ftelli64(f);
#else
  const off_t at = ::ftello(f);
#endif
  return at < 0 ? 0 : static_cast<std::uint64_t>(at);
}

class file_source final : public player::byte_source {
 public:
  file_source(std::FILE* f, std::uint64_t size) noexcept : f_(f), size_(size) {}
  ~file_source() override { std::fclose(f_); }

  result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override {
    if (offset >= size_ || out.empty()) return std::size_t{0};
    // FFmpeg reads forward far more than it seeks: skip the seek when the
    // file is already there.
    if (offset != pos_) {
      if (seek_to(f_, offset) != 0) return err(status::io);
      pos_ = offset;
    }
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size_ - offset));
    const std::size_t got = std::fread(out.data(), 1, want, f_);
    pos_ += got;
    if (got == 0 && std::ferror(f_)) {
      std::clearerr(f_);
      pos_ = ~std::uint64_t{0};  // unknown: seek before the next read
      return err(status::io);
    }
    return got;
  }
  std::uint64_t size() const noexcept override { return size_; }

 private:
  std::FILE* f_;
  std::uint64_t size_;
  std::uint64_t pos_ = 0;
};

}  // namespace

result<std::size_t> span_source::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
  if (offset >= bytes_.size()) return std::size_t{0};
  const std::size_t n = std::min<std::size_t>(out.size(), bytes_.size() - static_cast<std::size_t>(offset));
  std::memcpy(out.data(), bytes_.data() + offset, n);
  return n;
}

std::unique_ptr<player::byte_source> open_file_source(const char* utf8_path) {
  if (!utf8_path) return nullptr;
  std::FILE* f = std::fopen(utf8_path, "rb");
  if (!f) return nullptr;
  std::uint64_t size = 0;
#if defined(_WIN32)
  const bool sized = ::_fseeki64(f, 0, SEEK_END) == 0;
#else
  const bool sized = ::fseeko(f, 0, SEEK_END) == 0;
#endif
  if (sized) size = tell(f);
  if (!sized || size == 0 || seek_to(f, 0) != 0) {
    std::fclose(f);
    return nullptr;
  }
  auto* source = new (std::nothrow) file_source(f, size);
  if (!source) {
    std::fclose(f);
    return nullptr;
  }
  return std::unique_ptr<player::byte_source>(source);
}

result<bgra_thumb> render_poster(player::byte_source& source, std::uint32_t cx,
                                 const job_context* ctx) {
  const std::uint32_t edge = std::clamp<std::uint32_t>(cx, 16, kMaxThumbEdge);
  MV_TRY(player::poster_image p, player::poster_frame(source, edge, ctx));
  if (p.width == 0 || p.height == 0 || p.width > kMaxThumbEdge || p.height > kMaxThumbEdge ||
      p.rgba.size() < static_cast<std::size_t>(p.width) * p.height * 4u) {
    return err(status::corrupt);
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return detail::to_bgra(p.width, p.height, std::move(p.rgba));
}

result<bgra_thumb> render_poster_by(std::unique_ptr<player::byte_source> source, std::uint32_t cx,
                                    std::chrono::milliseconds deadline) {
  if (!source) return err(status::invalid_arg);
  std::unique_ptr<detail::thumb_work> work;
  try {
    work = std::make_unique<poster_work>(std::move(source), cx);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  return detail::run_bounded(std::move(work), deadline);
}

}  // namespace mv::shellext
