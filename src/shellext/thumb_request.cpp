// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shellext/thumb_request.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

#include "image/thumb.h"

namespace mv::shellext {
namespace {

// Straight RGBA -> BGRA, premultiplied (a DIB with alpha is premultiplied).
bgra_thumb to_bgra(image::thumb_pixels&& t) {
  bgra_thumb out;
  out.width = t.width;
  out.height = t.height;
  out.bgra = std::move(t.rgba);
  std::uint8_t* p = out.bgra.data();
  const std::size_t n = static_cast<std::size_t>(out.width) * out.height;
  for (std::size_t i = 0; i < n; ++i, p += 4) {
    const std::uint32_t a = p[3];
    std::swap(p[0], p[2]);
    if (a == 255) continue;
    out.has_alpha = true;
    p[0] = static_cast<std::uint8_t>((p[0] * a + 127) / 255);
    p[1] = static_cast<std::uint8_t>((p[1] * a + 127) / 255);
    p[2] = static_cast<std::uint8_t>((p[2] * a + 127) / 255);
  }
  return out;
}

// What the waiting caller and the decode thread share. The thread holds a
// reference past the caller's deadline, so it outlives an abandoned request.
struct pending {
  std::mutex mu;
  std::condition_variable done_cv;
  bool done = false;
  std::optional<result<bgra_thumb>> answer;
  std::atomic<generation> current{1};  // bumped to cancel
  std::vector<std::uint8_t> bytes;
};

std::atomic<std::uint32_t> g_in_flight{0};

}  // namespace

std::uint32_t decodes_in_flight() noexcept { return g_in_flight.load(std::memory_order_acquire); }

result<bgra_thumb> render_thumbnail(std::span<const std::uint8_t> bytes, std::uint32_t cx,
                                    const job_context* ctx) {
  if (bytes.empty() || bytes.size() > kMaxSourceBytes) return err(status::invalid_arg);
  const std::uint32_t edge = std::clamp<std::uint32_t>(cx, 16, kMaxThumbEdge);
  MV_TRY(image::thumb_pixels t, image::make_thumb_rgba(bytes, edge, ctx));
  if (t.width == 0 || t.height == 0 ||
      t.rgba.size() < static_cast<std::size_t>(t.width) * t.height * 4u) {
    return err(status::corrupt);
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_bgra(std::move(t));
}

result<bgra_thumb> render_thumbnail_by(std::vector<std::uint8_t> bytes, std::uint32_t cx,
                                       std::chrono::milliseconds deadline) {
  // Reserve a slot before starting the thread; the thread gives it back as
  // the last thing it does, after it has released everything it holds.
  if (g_in_flight.fetch_add(1, std::memory_order_acq_rel) >= kMaxInFlight) {
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return err(status::busy);  // refused, not abandoned: nothing of ours is running
  }
  std::shared_ptr<pending> job;
  try {
    job = std::make_shared<pending>();
    job->bytes = std::move(bytes);
    std::thread([job, cx]() mutable {
      result<bgra_thumb> r = err(status::internal);
      try {
        const job_context ctx(0, 1, &job->current, 0);
        r = render_thumbnail(job->bytes, cx, &ctx);
      } catch (const std::bad_alloc&) {
        r = err(status::out_of_memory);
      } catch (...) {
        r = err(status::internal);
      }
      job->bytes = {};
      {
        std::lock_guard lock(job->mu);
        job->answer.emplace(std::move(r));
        job->done = true;
        job->done_cv.notify_all();
      }
      job.reset();
      g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }).detach();
  } catch (const std::bad_alloc&) {
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return err(status::out_of_memory);
  } catch (...) {
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return err(status::internal);  // no thread could be started
  }
  std::unique_lock lock(job->mu);
  if (!job->done_cv.wait_for(lock, deadline, [&] { return job->done; })) {
    job->current.store(2, std::memory_order_relaxed);  // tell the decode to stop
    return err(status::cancelled);
  }
  return std::move(*job->answer);
}

}  // namespace mv::shellext
