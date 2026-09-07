// SPDX-License-Identifier: GPL-2.0-or-later
// The C ABI implementation. Every exported symbol in this file is `noexcept`
// and does its real work inside mv::abi::guard.

#include "mediaviewer/mediaviewer.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "abi/guard.h"
#include "abi/native.h"
#include "abi/video_session.h"
#include "player/container_probe.h"
#include "player/poster.h"
#include "core/job_system.h"
#include "core/spsc_ring.h"
#include "core/status.h"
#include "core/trace.h"
#include "gfx/device.h"
#include "image/pipeline.h"
#include "image/thumb.h"
#include "image/upload.h"
#include "io/dir.h"
#include "io/file.h"
#include "io/paths.h"

// The enum values on both sides of the line must stay numerically identical.
// If someone reorders mv::status, this stops the build rather than shipping a
// core that reports IO errors as corruption.
static_assert(static_cast<int>(mv::status::ok) == MV_OK, "mv_status drift");
static_assert(static_cast<int>(mv::status::invalid_arg) == MV_ERR_INVALID_ARG, "mv_status drift");
static_assert(static_cast<int>(mv::status::out_of_memory) == MV_ERR_OUT_OF_MEMORY,
              "mv_status drift");
static_assert(static_cast<int>(mv::status::io) == MV_ERR_IO, "mv_status drift");
static_assert(static_cast<int>(mv::status::unsupported_format) == MV_ERR_UNSUPPORTED_FORMAT,
              "mv_status drift");
static_assert(static_cast<int>(mv::status::corrupt) == MV_ERR_CORRUPT, "mv_status drift");
static_assert(static_cast<int>(mv::status::cancelled) == MV_ERR_CANCELLED, "mv_status drift");
static_assert(static_cast<int>(mv::status::device_lost) == MV_ERR_DEVICE_LOST, "mv_status drift");
static_assert(static_cast<int>(mv::status::internal) == MV_ERR_INTERNAL, "mv_status drift");

static_assert(sizeof(mv_completion) == 40, "mv_completion layout is part of the ABI");
static_assert(alignof(mv_completion) == 8, "mv_completion layout is part of the ABI");
static_assert(sizeof(mv_image_info) == 24, "mv_image_info layout is part of the ABI");
static_assert(sizeof(mv_folder_item) == 32, "mv_folder_item layout is part of the ABI");

namespace mv::abi {

namespace {

constexpr std::size_t last_error_capacity = 512;

struct thread_error_state {
  std::uint64_t current_correlation = 0;
  std::uint64_t failed_correlation = 0;
  char message[last_error_capacity] = {0};
};

thread_local thread_error_state t_error;
std::atomic<std::uint64_t> g_next_correlation{1};

}  // namespace

std::uint64_t next_correlation_id() noexcept {
  return g_next_correlation.fetch_add(1, std::memory_order_relaxed);
}

void begin_call(std::uint64_t correlation_id) noexcept {
  t_error.current_correlation = correlation_id;
  t_error.message[0] = '\0';
}

void set_last_error(std::uint64_t correlation_id, const char* message) noexcept {
  t_error.failed_correlation = correlation_id;
  if (message == nullptr) {
    t_error.message[0] = '\0';
    return;
  }
  const std::size_t n = std::strlen(message);
  const std::size_t copy = n < last_error_capacity - 1 ? n : last_error_capacity - 1;
  std::memcpy(t_error.message, message, copy);
  t_error.message[copy] = '\0';
}

const char* last_error_message() noexcept { return t_error.message; }
std::uint64_t last_error_correlation_id() noexcept { return t_error.failed_correlation; }
std::uint64_t current_correlation_id() noexcept { return t_error.current_correlation; }

}  // namespace mv::abi

// ---------------------------------------------------------------------------
// The session object. Opaque to the caller; `mv_session_t` is a pointer to it.
// ---------------------------------------------------------------------------
struct mv_session {
  std::atomic<std::uint32_t> ref_count{1};
  mv::job_system jobs;
  mv::abi::video_session video;

  // Completions are produced by many worker threads and consumed by one
  // draining thread, so the SPSC ring is not the right shape here — this is the
  // one MPMC-ish edge in the design and it takes a short-held mutex rather than
  // pretending otherwise. The lock is never held across a call we do not own,
  // and never touched by the render thread except in the drain.
  std::mutex completion_mutex;
  std::vector<mv_completion> completions;
  HANDLE completion_event = nullptr;

  std::mutex device_mutex;
  mv::gfx::com_ptr<ID3D11Device> device;

  std::mutex image_mutex;
  std::atomic<mv::image::gpu_image*> ready{nullptr};
  std::shared_ptr<mv::image::display_image> cpu;
  mv_image_info info{};
  HANDLE image_ready_event = nullptr;

  struct folder_item {
    std::string name;
    std::string path;
    std::string thumb_path;
    std::uint64_t size = 0;
    std::int64_t mtime_unix = 0;
  };
  struct lru_slot {
    std::string path;
    std::unique_ptr<mv::image::gpu_image> gpu;
    mv_image_info info{};
  };

  std::mutex folder_mutex;
  std::vector<folder_item> folder_items;
  std::string folder_dir;
  std::string folder_select_path;
  std::uint32_t folder_selected = 0;
  std::atomic<std::uint32_t> folder_generation{1};
  mv::io::directory_watcher watcher;
  mv::image::thumb_store thumbs;

  // A decode already queued or running for a path at this generation. Without
  // it, a held arrow key re-submits the same neighbours every step as the LRU
  // churns, and the pool decodes the same JPEG four times over.
  struct inflight_decode {
    std::string path;
    mv::generation gen = 0;
  };

  std::mutex lru_mutex;
  std::vector<lru_slot> lru;
  std::vector<inflight_decode> decode_inflight;

  void push_completion(const mv_completion& c) noexcept {
    {
      std::lock_guard lock(completion_mutex);
      completions.push_back(c);
    }
    if (completion_event) ::SetEvent(completion_event);
  }

  mv::gfx::com_ptr<ID3D11Device> copy_device() {
    std::lock_guard lock(device_mutex);
    return device;
  }
};

namespace {

using mv::abi::guard;
using mv::status;

// `session` is validated by every entry point before use. There is no way to
// tell a stale pointer from a live one across an ABI, so this checks only for
// null — the SafeHandle on the managed side is what actually prevents
// use-after-free, which is why plan/14 makes it non-negotiable.
constexpr bool valid(mv_session_t s) noexcept { return s != nullptr; }

mv_image_info info_from(const mv::image::display_image& cpu) noexcept {
  mv_image_info info{};
  info.width = cpu.width;
  info.height = cpu.height;
  info.format = static_cast<uint32_t>(cpu.format);
  info.icc_tagged = cpu.icc_tagged ? 1u : 0u;
  info.transfer_intent = static_cast<uint32_t>(cpu.intent);
  return info;
}

bool publish_view(mv_session* session, const mv::job_context& ctx, mv_image_info info,
                  std::shared_ptr<mv::image::display_image> cpu,
                  std::unique_ptr<mv::image::gpu_image> gpu) {
  std::lock_guard lock(session->image_mutex);
  if (ctx.gen() != session->jobs.current_generation()) return false;
  session->info = info;
  if (cpu) session->cpu = std::move(cpu);
  if (gpu) {
    mv::image::gpu_image* old = session->ready.exchange(gpu.release(), std::memory_order_acq_rel);
    delete old;
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  }
  return true;
}

void publish_ready(mv_session* session, mv_image_info info,
                   std::shared_ptr<mv::image::display_image> cpu,
                   std::unique_ptr<mv::image::gpu_image> gpu) {
  std::lock_guard lock(session->image_mutex);
  session->info = info;
  if (cpu) session->cpu = std::move(cpu);
  if (gpu) {
    mv::image::gpu_image* old = session->ready.exchange(gpu.release(), std::memory_order_acq_rel);
    delete old;
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  }
}

// Worker-only probe; never read a whole multi-gigabyte clip to identify it.
bool video_path(const std::string& path) {
  auto head = mv::io::read_prefix(path, mv::player::probe_bytes);
  return head && mv::player::is_video(mv::player::probe(head.value()));
}
status open_video_worker(mv_session* session, const std::string& path, const mv::job_context& ctx) {
  auto device = session->copy_device();
  if (!device) return status::device_lost;
  auto result = mv::player::open_media(path.c_str(), device.Get());
  if (!result) return result.error();
  auto* source = result.value();
  if (ctx.cancelled()) { mv::player::close_media(source); return status::cancelled; }
  const auto info = source->info();
  session->video.publish(source, ctx.gen());
  mv_completion c{};
  c.kind = MV_COMPLETION_VIDEO_OPENED; c.status = MV_OK;
  c.generation = ctx.gen(); c.payload = info.duration_ns;
  session->push_completion(c);
  if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  return status::ok;
}

bool path_is_selected(mv_session* session, const std::string& path) {
  std::lock_guard lock(session->folder_mutex);
  if (session->folder_selected >= session->folder_items.size()) return false;
  return session->folder_items[session->folder_selected].path == path;
}

void push_folder_selected(mv_session* session, uint32_t index) {
  mv_completion c{};
  c.kind = MV_COMPLETION_FOLDER_SELECTED;
  c.status = MV_OK;
  c.payload = static_cast<int64_t>(index);
  session->push_completion(c);
}

std::unique_ptr<mv::image::gpu_image> clone_gpu(const mv::image::gpu_image& src) {
  auto p = std::make_unique<mv::image::gpu_image>();
  *p = src;
  return p;
}

// plan/02 sizes the viewer cache in bytes, not entries: five 45 MP stills and
// five phone JPEGs are the same count and a 10x difference in VRAM. A fixed
// count of 5 also meant the +/-2 prefetch window evicted itself, so every step
// re-decoded neighbours it had just paid for.
constexpr std::size_t lru_byte_budget = 512ull * 1024 * 1024;
constexpr std::size_t lru_min_entries = 3;
constexpr std::size_t lru_max_entries = 12;

std::size_t gpu_bytes(const mv::image::gpu_image& gpu) noexcept {
  // RGBA8; a full mip chain adds a third again.
  const std::size_t top = static_cast<std::size_t>(gpu.width) * gpu.height * 4;
  return gpu.mip_levels > 1 ? top + top / 3 : top;
}

void lru_put(mv_session* session, std::string path, const mv::image::gpu_image& gpu,
             mv_image_info info) {
  std::lock_guard lock(session->lru_mutex);
  for (auto it = session->lru.begin(); it != session->lru.end(); ++it) {
    if (it->path == path) {
      session->lru.erase(it);
      break;
    }
  }
  mv_session::lru_slot slot;
  slot.path = std::move(path);
  slot.gpu = clone_gpu(gpu);
  slot.info = info;
  session->lru.push_back(std::move(slot));

  std::size_t total = 0;
  for (const auto& s : session->lru) total += s.gpu ? gpu_bytes(*s.gpu) : 0;
  while (session->lru.size() > lru_max_entries ||
         (session->lru.size() > lru_min_entries && total > lru_byte_budget)) {
    total -= session->lru.front().gpu ? gpu_bytes(*session->lru.front().gpu) : 0;
    session->lru.erase(session->lru.begin());
  }
}

bool lru_publish(mv_session* session, const std::string& path) {
  std::unique_ptr<mv::image::gpu_image> gpu;
  mv_image_info info{};
  {
    std::lock_guard lock(session->lru_mutex);
    for (auto it = session->lru.begin(); it != session->lru.end(); ++it) {
      if (it->path != path || !it->gpu) continue;
      gpu = clone_gpu(*it->gpu);
      info = it->info;
      auto slot = std::move(*it);
      session->lru.erase(it);
      session->lru.push_back(std::move(slot));
      break;
    }
  }
  if (!gpu) return false;
  std::lock_guard image_lock(session->image_mutex);
  session->info = info;
  mv::image::gpu_image* old = session->ready.exchange(gpu.release(), std::memory_order_acq_rel);
  delete old;
  if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  return true;
}

status copy_utf8(const std::string& s, char* buf, uint32_t cap, uint32_t* out_bytes) {
  const auto need = static_cast<uint32_t>(s.size() + 1);
  if (out_bytes) *out_bytes = need;
  if (buf == nullptr || cap == 0) return status::invalid_arg;
  const uint32_t n = need <= cap ? need : cap;
  std::memcpy(buf, s.c_str(), n - 1);
  buf[n - 1] = '\0';
  return status::ok;
}

void submit_thumb_at(mv_session* session, uint32_t index) {
  mv_session::folder_item item;
  std::uint32_t folder_gen = 0;
  {
    std::lock_guard lock(session->folder_mutex);
    if (index >= session->folder_items.size()) return;
    if (!session->folder_items[index].thumb_path.empty()) return;
    item = session->folder_items[index];
    folder_gen = session->folder_generation.load(std::memory_order_relaxed);
  }
  if (!session->thumbs.is_open()) {
    if (auto dir = mv::io::thumb_cache_dir()) {
      (void)session->thumbs.open(dir.value());
    }
  }

  const auto correlation = mv::abi::current_correlation_id();
  (void)session->jobs.submit_at(
      mv::background_generation,
      [session, item, index, folder_gen](const mv::job_context& ctx) -> status {
        if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
          return status::cancelled;
        }
        mv::image::thumb_key key{item.path, item.mtime_unix, item.size};
        if (auto hit = session->thumbs.lookup(key)) {
          if (!hit.value().empty()) {
            std::lock_guard lock(session->folder_mutex);
            if (index < session->folder_items.size() &&
                session->folder_items[index].path == item.path) {
              session->folder_items[index].thumb_path = hit.value();
            }
            return status::ok;
          }
        }
        // A camera dump is photos and clips in one folder. Skipping the clips
        // here is what listed every video as a blank tile: the strip and the
        // gallery both read this path, so "no thumb" is the whole carousel.
        // The poster frame is a one-shot software decode on this pool thread —
        // it never touches the D3D11VA session the playing clip is using.
        std::vector<std::uint8_t> jpeg_bytes;
        if (video_path(item.path)) {
          auto poster = mv::player::poster_frame(item.path.c_str(), mv::image::kThumbLongEdge, &ctx);
          if (!poster) return poster.error();
          if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
            return status::cancelled;
          }
          auto encoded = mv::image::encode_thumb_rgba(poster.value().rgba, poster.value().width,
                                                      poster.value().height);
          if (!encoded) return encoded.error();
          jpeg_bytes = std::move(encoded).value();
        } else {
          auto bytes = mv::io::read_all(item.path);
          if (!bytes) return bytes.error();
          if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
            return status::cancelled;
          }
          auto jpeg = mv::image::make_thumb_jpeg(bytes.value(), &ctx);
          if (!jpeg) return jpeg.error();
          jpeg_bytes = std::move(jpeg).value();
        }
        const auto& jpeg = jpeg_bytes;
        auto stored = session->thumbs.store(key, jpeg);
        if (!stored) return stored.error();
        std::lock_guard lock(session->folder_mutex);
        if (index < session->folder_items.size() &&
            session->folder_items[index].path == item.path) {
          session->folder_items[index].thumb_path = stored.value();
        }
        return status::ok;
      },
      [session, correlation, index, folder_gen](mv::job_id id, mv::generation, status result) {
        if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) return;
        mv_completion c{};
        c.kind = MV_COMPLETION_THUMB_READY;
        c.status = static_cast<uint32_t>(result);
        c.job_id = id;
        c.correlation_id = correlation;
        c.generation = folder_gen;
        c.payload = static_cast<int64_t>(index);
        session->push_completion(c);
      });
}

void submit_thumb_jobs(mv_session* session, uint32_t first, uint32_t count) {
  uint32_t n = 0;
  {
    std::lock_guard lock(session->folder_mutex);
    n = static_cast<uint32_t>(session->folder_items.size());
  }
  if (first >= n || count == 0) return;
  const uint32_t last = first + count > n ? n : first + count;
  for (uint32_t i = first; i < last; ++i) submit_thumb_at(session, i);
}

void push_image_opened(mv_session* session, std::uint64_t correlation, mv::generation gen,
                       status result) {
  mv_completion c{};
  c.kind = MV_COMPLETION_IMAGE_OPENED;
  c.status = static_cast<uint32_t>(result);
  c.correlation_id = correlation;
  c.generation = gen;
  if (result == status::ok) {
    std::lock_guard lock(session->image_mutex);
    c.payload = (static_cast<int64_t>(session->info.width) << 32) |
                static_cast<int64_t>(session->info.height);
  }
  session->push_completion(c);
}

bool claim_decode(mv_session* session, const std::string& path, mv::generation gen) {
  std::lock_guard lock(session->lru_mutex);
  for (const auto& d : session->decode_inflight) {
    // Only a job at *this* generation counts. An older one is already doomed by
    // the navigation that bumped the counter, so it must not suppress the
    // decode of the image the user has actually landed on.
    if (d.gen == gen && d.path == path) return false;
  }
  session->decode_inflight.push_back({path, gen});
  return true;
}

void release_decode(mv_session* session, const std::string& path, mv::generation gen) {
  std::lock_guard lock(session->lru_mutex);
  for (auto it = session->decode_inflight.begin(); it != session->decode_inflight.end(); ++it) {
    if (it->gen == gen && it->path == path) {
      session->decode_inflight.erase(it);
      return;
    }
  }
}

// A folder decode is view-tied work, not background work. plan/02: "Navigating
// away bumps the generation; in-flight decodes check it and abandon. Without
// this, fast arrow-key scrubbing through a folder queues 200 decodes and the
// app feels like it's chewing gum." Submitting these at background_generation
// opted every one of them out of exactly that, so holding an arrow key queued
// five uncancellable full decodes (each with a CPU Mitchell mip pyramid) per
// step and the pool spent a second finishing dead work after the key came up.
void submit_decode_to_lru(mv_session* session, std::string path, mv::generation gen) {
  if (!claim_decode(session, path, gen)) return;
  const std::uint32_t folder_gen = session->folder_generation.load(std::memory_order_relaxed);
  const auto correlation = mv::abi::current_correlation_id();
  (void)session->jobs.submit_at(
      gen,
      [session, path, folder_gen, correlation](const mv::job_context& ctx) -> status {
        if (ctx.cancelled()) return status::cancelled;
        if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
          return status::cancelled;
        }
        if (video_path(path)) {
          if (!path_is_selected(session, path)) return status::ok;
          return open_video_worker(session, path, ctx);
        }
        auto bytes = mv::io::read_all(path);
        if (!bytes) return bytes.error();
        if (ctx.cancelled()) return status::cancelled;

        // First pixel is the DCT 1/4 preview, and only for the image actually
        // on screen — a prefetched neighbour has nothing to show it on.
        if (path_is_selected(session, path)) {
          if (auto preview = mv::image::decode_preview(bytes.value(), &ctx)) {
            if (ctx.cancelled()) return status::cancelled;
            if (auto dev = session->copy_device()) {
              auto uploaded = mv::image::upload(dev.Get(), preview.value(), ctx.gen(), &ctx, 1);
              if (uploaded) {
                auto gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
                if (path_is_selected(session, path)) {
                  publish_ready(session, info_from(preview.value()), nullptr, std::move(gpu));
                  push_image_opened(session, correlation, ctx.gen(), status::ok);
                }
              } else if (uploaded.error() == status::cancelled) {
                return status::cancelled;
              }
            }
          } else if (preview.error() == status::cancelled) {
            return status::cancelled;
          }
        }

        auto decoded = mv::image::decode_bytes(bytes.value(), &ctx);
        if (!decoded) return decoded.error();
        if (ctx.cancelled()) return status::cancelled;
        if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
          return status::cancelled;
        }
        auto cpu = std::make_shared<mv::image::display_image>(std::move(decoded).value());
        const mv_image_info info = info_from(*cpu);

        auto upload_and_publish = [&](std::uint32_t mip_limit) -> status {
          auto dev = session->copy_device();
          if (!dev) {
            if (path_is_selected(session, path)) publish_ready(session, info, cpu, nullptr);
            return status::ok;
          }
          auto uploaded = mv::image::upload(dev.Get(), *cpu, ctx.gen(), &ctx, mip_limit);
          if (!uploaded) return uploaded.error();
          if (ctx.cancelled()) return status::cancelled;
          auto gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
          lru_put(session, path, *gpu, info);
          if (path_is_selected(session, path)) {
            publish_ready(session, info, cpu, std::move(gpu));
            push_image_opened(session, correlation, ctx.gen(), status::ok);
          }
          return status::ok;
        };

        // The CPU mip pyramid of a large still costs more than the decode did.
        // Get the top level on screen first, then pay for the pyramid — same
        // staging mv_image_open already uses (plan/04).
        const bool large =
            static_cast<std::uint64_t>(cpu->width) * cpu->height >= 2048ull * 2048ull;
        if (large && path_is_selected(session, path)) {
          const status first = upload_and_publish(1);
          if (first != status::ok) return first;
          if (ctx.cancelled()) return status::cancelled;
        }
        return upload_and_publish(0);
      },
      [session, path, correlation](mv::job_id, mv::generation gen, status result) {
        release_decode(session, path, gen);
        // A failure on the visible image still has to clear the host's
        // "loading" state; success already reported at publish time.
        if (result != status::ok && result != status::cancelled &&
            path_is_selected(session, path)) {
          push_image_opened(session, correlation, gen, result);
        }
      });
}

void submit_prefetch(mv_session* session, uint32_t index, mv::generation gen) {
  std::vector<std::string> paths;
  {
    std::lock_guard lock(session->folder_mutex);
    const auto n = session->folder_items.size();
    if (index >= n) return;
    const int offsets[] = {1, -1, 2, -2};
    for (int off : offsets) {
      const int i = static_cast<int>(index) + off;
      if (i < 0 || static_cast<std::size_t>(i) >= n) continue;
      paths.push_back(session->folder_items[static_cast<std::size_t>(i)].path);
    }
  }
  for (const auto& p : paths) {
    bool hit = false;
    {
      std::lock_guard lock(session->lru_mutex);
      for (const auto& s : session->lru) {
        if (s.path == p) {
          hit = true;
          break;
        }
      }
    }
    if (!hit) submit_decode_to_lru(session, p, gen);
  }
}

void apply_folder_list(mv_session* session, std::vector<mv::io::dir_entry> listed,
                       bool changed) {
  std::string want;
  {
    std::lock_guard lock(session->folder_mutex);
    want = session->folder_select_path;
    session->folder_items.clear();
    session->folder_items.reserve(listed.size());
    for (auto& e : listed) {
      mv_session::folder_item it;
      it.name = std::move(e.name_utf8);
      it.path = std::move(e.path_utf8);
      it.size = e.size;
      it.mtime_unix = e.mtime_unix;
      session->folder_items.push_back(std::move(it));
    }
    session->folder_selected = 0;
    if (!want.empty()) {
      for (uint32_t i = 0; i < session->folder_items.size(); ++i) {
        if (session->folder_items[i].path == want) {
          session->folder_selected = i;
          break;
        }
      }
    }
  }
  mv_completion c{};
  c.kind = changed ? MV_COMPLETION_FOLDER_CHANGED : MV_COMPLETION_FOLDER_READY;
  c.status = MV_OK;
  c.payload = static_cast<int64_t>(listed.size());
  session->push_completion(c);

  uint32_t selected = 0;
  uint32_t n = 0;
  std::string selected_path;
  {
    std::lock_guard lock(session->folder_mutex);
    selected = session->folder_selected;
    n = static_cast<uint32_t>(session->folder_items.size());
    if (selected < n) selected_path = session->folder_items[selected].path;
  }
  submit_thumb_at(session, selected);
  for (uint32_t i = 0; i < n; ++i) {
    if (i != selected) submit_thumb_at(session, i);
  }
  if (!selected_path.empty()) {
    const mv::generation gen = session->jobs.current_generation();
    submit_decode_to_lru(session, selected_path, gen);
    submit_prefetch(session, selected, gen);
  }
  push_folder_selected(session, selected);
}

void on_folder_watch(void* user) {
  auto* session = static_cast<mv_session*>(user);
  (void)session->jobs.submit_at(mv::background_generation, [session](const mv::job_context&) -> status {
    std::string dir;
    {
      std::lock_guard lock(session->folder_mutex);
      dir = session->folder_dir;
    }
    if (dir.empty()) return status::ok;
    session->folder_generation.fetch_add(1, std::memory_order_relaxed);
    auto listed = mv::io::list_still_files(dir);
    if (!listed) return listed.error();
    apply_folder_list(session, std::move(listed).value(), true);
    return status::ok;
  });
}

}  // namespace

extern "C" {

uint32_t MV_CALL mv_abi_version(void) {
  return (static_cast<uint32_t>(MV_ABI_VERSION_MAJOR) << 16) |
         static_cast<uint32_t>(MV_ABI_VERSION_MINOR);
}

const char* MV_CALL mv_status_name(mv_status s) {
  return mv::status_name(static_cast<mv::status>(s));
}

const char* MV_CALL mv_last_error_message(void) { return mv::abi::last_error_message(); }

uint64_t MV_CALL mv_last_error_correlation_id(void) {
  return mv::abi::last_error_correlation_id();
}

mv_status MV_CALL mv_session_create(const mv_session_config* config, mv_session_t* out_session) {
  return static_cast<mv_status>(guard("mv_session_create", [&]() -> status {
    MV_REQUIRE(out_session != nullptr, "out_session must not be null");
    *out_session = nullptr;

    const uint32_t workers = config ? config->worker_count : 0;
    const bool etw = config ? config->enable_etw != 0 : true;

    if (etw) mv::trace::provider_register();

    auto session = std::make_unique<mv_session>();

    session->completion_event = ::CreateEventW(nullptr, TRUE /*manual reset*/,
                                               FALSE /*non-signalled*/, nullptr);
    if (!session->completion_event) return status::internal;
    session->image_ready_event = ::CreateEventW(nullptr, FALSE /*auto-reset*/,
                                                FALSE /*non-signalled*/, nullptr);
    if (!session->image_ready_event) {
      ::CloseHandle(session->completion_event);
      return status::internal;
    }

    session->completions.reserve(256);

    const status started = session->jobs.start(workers);
    if (started != status::ok) {
      ::CloseHandle(session->image_ready_event);
      ::CloseHandle(session->completion_event);
      return started;
    }

    *out_session = session.release();
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_retain(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_session_retain", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->ref_count.fetch_add(1, std::memory_order_relaxed);
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_release(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_session_release", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    if (session->ref_count.fetch_sub(1, std::memory_order_acq_rel) != 1) return status::ok;

    // Watcher first: its callback submits jobs. Then join the pool.
    session->watcher.stop();
    session->jobs.shutdown();
    session->thumbs.close();
    delete session->ready.exchange(nullptr, std::memory_order_acq_rel);
    if (session->completion_event) ::CloseHandle(session->completion_event);
    if (session->image_ready_event) ::CloseHandle(session->image_ready_event);
    delete session;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_bump_generation(mv_session_t session, uint32_t* out_generation) {
  return static_cast<mv_status>(guard("mv_session_bump_generation", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    const uint32_t gen = session->jobs.bump_generation();
    if (out_generation) *out_generation = gen;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_current_generation(mv_session_t session, uint32_t* out_generation) {
  return static_cast<mv_status>(guard("mv_session_current_generation", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_generation != nullptr, "out_generation must not be null");
    *out_generation = session->jobs.current_generation();
    return status::ok;
  }));
}

void* MV_CALL mv_completion_wait_handle(mv_session_t session) {
  if (!valid(session)) return nullptr;
  return session->completion_event;
}

uint32_t MV_CALL mv_completion_drain(mv_session_t session, mv_completion* out, uint32_t capacity) {
  if (!valid(session) || out == nullptr || capacity == 0) return 0;

  std::lock_guard lock(session->completion_mutex);
  const auto available = static_cast<uint32_t>(session->completions.size());
  const uint32_t count = available < capacity ? available : capacity;
  if (count == 0) {
    ::ResetEvent(session->completion_event);
    return 0;
  }

  std::memcpy(out, session->completions.data(), count * sizeof(mv_completion));
  session->completions.erase(session->completions.begin(),
                             session->completions.begin() + static_cast<std::ptrdiff_t>(count));
  // Reset only when the queue is genuinely empty, so a partial drain leaves the
  // caller a reason to come back.
  if (session->completions.empty()) ::ResetEvent(session->completion_event);
  return count;
}

mv_status MV_CALL mv_session_echo(mv_session_t session, const char* utf8_text,
                                  uint64_t* out_job_id) {
  return static_cast<mv_status>(guard("mv_session_echo", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(utf8_text != nullptr, "utf8_text must not be null");

    // The caller owns the string. Copy it before returning; never retain the
    // pointer past this call (plan/14, ownership table).
    std::string owned(utf8_text);
    const auto correlation = mv::abi::current_correlation_id();

    const auto payload = static_cast<std::int64_t>(owned.size());
    const mv::job_id id = session->jobs.submit(
        [text = std::move(owned)](const mv::job_context& ctx) -> status {
          if (ctx.cancelled()) return status::cancelled;
          // Real work goes here in later PRs. In PR 1 the point is the shape:
          // the answer is produced on a worker and delivered as a completion.
          return text.empty() ? status::invalid_arg : status::ok;
        },
        [session, correlation, payload](mv::job_id id, mv::generation gen, status result) {
          mv_completion c{};
          c.kind = MV_COMPLETION_ECHO;
          c.status = static_cast<uint32_t>(result);
          c.job_id = id;
          c.correlation_id = correlation;
          c.generation = gen;
          c.payload = payload;
          session->push_completion(c);
        });

    if (id == mv::invalid_job) return status::internal;
    if (out_job_id) *out_job_id = id;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_job_stats(mv_session_t session, mv_job_stats* out_stats) {
  return static_cast<mv_status>(guard("mv_session_job_stats", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_stats != nullptr, "out_stats must not be null");
    out_stats->submitted = session->jobs.submitted();
    out_stats->completed = session->jobs.completed();
    out_stats->cancelled = session->jobs.cancelled();
    out_stats->queue_depth = session->jobs.queue_depth();
    out_stats->worker_count = session->jobs.worker_count();
    out_stats->generation = session->jobs.current_generation();
    return status::ok;
  }));
}

mv_status MV_CALL mv_image_open(mv_session_t session, const char* utf8_path, uint64_t* out_job_id) {
  return static_cast<mv_status>(guard("mv_image_open", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(utf8_path != nullptr, "utf8_path must not be null");
    MV_REQUIRE(utf8_path[0] != '\0', "utf8_path must not be empty");

    std::string owned(utf8_path);
    const auto correlation = mv::abi::current_correlation_id();
    const mv::generation gen = session->jobs.bump_generation();

    const mv::job_id id = session->jobs.submit_at(
        gen,
        [session, path = std::move(owned)](const mv::job_context& ctx) -> status {
          if (ctx.cancelled()) return status::cancelled;

          if (video_path(path)) return open_video_worker(session, path, ctx);
          auto bytes = mv::io::read_all(path);
          if (!bytes) return bytes.error();
          if (ctx.cancelled()) return status::cancelled;

          // First pixel: JPEG DCT 1/4. Fit-to-window of the preview fills the
          // same rect as the full image; 100 % during load is briefly small.
          if (auto preview = mv::image::decode_preview(bytes.value(), &ctx)) {
            if (ctx.cancelled()) return status::cancelled;
            std::unique_ptr<mv::image::gpu_image> gpu;
            if (auto dev = session->copy_device()) {
              auto uploaded = mv::image::upload(dev.Get(), preview.value(), ctx.gen(), &ctx);
              if (uploaded) {
                gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
              } else if (uploaded.error() == status::cancelled) {
                return status::cancelled;
              }
            }
            if (gpu) {
              (void)publish_view(session, ctx, info_from(preview.value()), nullptr, std::move(gpu));
            }
          } else if (preview.error() == status::cancelled) {
            return status::cancelled;
          }

          auto decoded = mv::image::decode_bytes(bytes.value(), &ctx);
          if (!decoded) return decoded.error();
          if (ctx.cancelled()) return status::cancelled;

          auto cpu = std::make_shared<mv::image::display_image>(std::move(decoded).value());
          const mv_image_info info = info_from(*cpu);
          // CPU cache first so a device rebuild can re-upload if CreateTexture2D
          // is still in flight against the old device (plan/12).
          if (!publish_view(session, ctx, info, cpu, nullptr)) return status::cancelled;

          const bool large =
              static_cast<std::uint64_t>(cpu->width) * cpu->height >= 2048ull * 2048ull;
          auto upload_and_publish = [&](std::uint32_t mip_limit) -> status {
            auto dev = session->copy_device();
            if (!dev) return status::ok;
            auto uploaded = mv::image::upload(dev.Get(), *cpu, ctx.gen(), &ctx, mip_limit);
            if (!uploaded) return uploaded.error();
            if (ctx.cancelled()) return status::cancelled;
            auto gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
            if (gpu) lru_put(session, path, *gpu, info);
            if (!publish_view(session, ctx, info, nullptr, std::move(gpu))) {
              return status::cancelled;
            }
            return status::ok;
          };

          if (large) {
            const status first = upload_and_publish(1);
            if (first != status::ok) return first;
            if (ctx.cancelled()) return status::cancelled;
          }
          return upload_and_publish(0);
        },
        [session, correlation](mv::job_id id, mv::generation gen, status result) {
          int64_t payload = 0;
          if (result == status::ok) {
            std::lock_guard lock(session->image_mutex);
            payload = (static_cast<int64_t>(session->info.width) << 32) |
                      static_cast<int64_t>(session->info.height);
          }
          mv_completion c{};
          c.kind = MV_COMPLETION_IMAGE_OPENED;
          c.status = static_cast<uint32_t>(result);
          c.job_id = id;
          c.correlation_id = correlation;
          c.generation = gen;
          c.payload = payload;
          session->push_completion(c);
        });

    if (id == mv::invalid_job) return status::internal;
    if (out_job_id) *out_job_id = id;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_image_info(mv_session_t session, mv_image_info* out_info) {
  return static_cast<mv_status>(guard("mv_session_image_info", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_info != nullptr, "out_info must not be null");
    std::lock_guard lock(session->image_mutex);
    *out_info = session->info;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_open(mv_session_t session, const char* utf8_dir,
                                 const char* utf8_select_path, uint64_t* out_job_id) {
  return static_cast<mv_status>(guard("mv_folder_open", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(utf8_dir != nullptr && utf8_dir[0] != '\0', "utf8_dir must not be empty");

    std::string dir(utf8_dir);
    std::string select = utf8_select_path ? std::string(utf8_select_path) : std::string{};
    session->watcher.stop();
    session->folder_generation.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(session->folder_mutex);
      session->folder_dir = dir;
      session->folder_select_path = select;
      session->folder_items.clear();
      session->folder_selected = 0;
    }
    if (!session->thumbs.is_open()) {
      if (auto cache = mv::io::thumb_cache_dir()) (void)session->thumbs.open(cache.value());
    }

    const auto correlation = mv::abi::current_correlation_id();
    const mv::job_id id = session->jobs.submit_at(
        mv::background_generation,
        [session, dir](const mv::job_context&) -> status {
          auto listed = mv::io::list_still_files(dir);
          if (!listed) return listed.error();
          apply_folder_list(session, std::move(listed).value(), false);
          (void)session->watcher.start(dir, &on_folder_watch, session);
          return status::ok;
        },
        [session, correlation](mv::job_id id, mv::generation gen, status result) {
          if (result == status::ok) return;
          mv_completion c{};
          c.kind = MV_COMPLETION_FOLDER_READY;
          c.status = static_cast<uint32_t>(result);
          c.job_id = id;
          c.correlation_id = correlation;
          c.generation = gen;
          session->push_completion(c);
        });
    if (id == mv::invalid_job) return status::internal;
    if (out_job_id) *out_job_id = id;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_count(mv_session_t session, uint32_t* out_count) {
  return static_cast<mv_status>(guard("mv_folder_count", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_count != nullptr, "out_count must not be null");
    std::lock_guard lock(session->folder_mutex);
    *out_count = static_cast<uint32_t>(session->folder_items.size());
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_item_at(mv_session_t session, uint32_t index, mv_folder_item* out_item) {
  return static_cast<mv_status>(guard("mv_folder_item_at", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_item != nullptr, "out_item must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_items.size(), "index out of range");
    const auto& it = session->folder_items[index];
    mv_folder_item item{};
    item.index = index;
    item.flags = index == session->folder_selected ? 1u : 0u;
    item.size_bytes = it.size;
    item.mtime_unix = it.mtime_unix;
    *out_item = item;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_item_name(mv_session_t session, uint32_t index, char* utf8,
                                      uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_item_name", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_items.size(), "index out of range");
    return copy_utf8(session->folder_items[index].name, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_item_path(mv_session_t session, uint32_t index, char* utf8,
                                      uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_item_path", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_items.size(), "index out of range");
    return copy_utf8(session->folder_items[index].path, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_item_thumb_path(mv_session_t session, uint32_t index, char* utf8,
                                            uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_item_thumb_path", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_items.size(), "index out of range");
    return copy_utf8(session->folder_items[index].thumb_path, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_select(mv_session_t session, uint32_t index, uint64_t* out_job_id) {
  return static_cast<mv_status>(guard("mv_folder_select", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::string path;
    {
      std::lock_guard lock(session->folder_mutex);
      MV_REQUIRE(index < session->folder_items.size(), "index out of range");
      session->folder_selected = index;
      path = session->folder_items[index].path;
    }
    const mv::generation gen = session->jobs.bump_generation();
    const auto correlation = mv::abi::current_correlation_id();
    push_folder_selected(session, index);
    if (lru_publish(session, path)) {
      mv_completion c{};
      c.kind = MV_COMPLETION_IMAGE_OPENED;
      c.status = MV_OK;
      c.correlation_id = correlation;
      c.generation = session->jobs.current_generation();
      {
        std::lock_guard lock(session->image_mutex);
        c.payload = (static_cast<int64_t>(session->info.width) << 32) |
                    static_cast<int64_t>(session->info.height);
      }
      session->push_completion(c);
      if (out_job_id) *out_job_id = 0;
      submit_prefetch(session, index, gen);
      return status::ok;
    }
    if (out_job_id) *out_job_id = 0;
    submit_decode_to_lru(session, std::move(path), gen);
    submit_prefetch(session, index, gen);
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_selected(mv_session_t session, uint32_t* out_index) {
  return static_cast<mv_status>(guard("mv_folder_selected", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_index != nullptr, "out_index must not be null");
    std::lock_guard lock(session->folder_mutex);
    *out_index = session->folder_selected;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_thumbs_visible(mv_session_t session, uint32_t first, uint32_t count) {
  return static_cast<mv_status>(guard("mv_folder_thumbs_visible", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    submit_thumb_jobs(session, first, count);
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_close(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_folder_close", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->watcher.stop();
    session->folder_generation.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(session->folder_mutex);
    session->folder_dir.clear();
    session->folder_select_path.clear();
    session->folder_items.clear();
    session->folder_selected = 0;
    return status::ok;
  }));
}

mv_status MV_CALL mv_video_open(mv_session_t session, const char* path, uint64_t* job) {
  return mv_image_open(session, path, job);
}
mv_status MV_CALL mv_video_close(mv_session_t session) {
  if (!session) return MV_ERR_INVALID_ARG;
  session->jobs.bump_generation();
  if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  return MV_OK;
}
mv_status MV_CALL mv_video_play(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_video_play", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.play(); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_pause(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_video_pause", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.pause(); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_seek(mv_session_t session, int64_t position, int32_t exact) {
  return static_cast<mv_status>(guard("mv_video_seek", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.seek(position, exact != 0); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_step(mv_session_t session, int32_t frames) {
  return static_cast<mv_status>(guard("mv_video_step", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.step(frames); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_set_rate(mv_session_t session, double rate) {
  return static_cast<mv_status>(guard("mv_video_set_rate", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.set_rate(rate); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_set_volume(mv_session_t session, float volume) {
  return static_cast<mv_status>(guard("mv_video_set_volume", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.set_volume(volume); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_set_muted(mv_session_t session, int32_t muted) {
  return static_cast<mv_status>(guard("mv_video_set_muted", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.set_muted(muted != 0); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_select_audio_track(mv_session_t session, uint32_t index) {
  return static_cast<mv_status>(guard("mv_video_select_audio_track", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.select_audio_track(index); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_set_loop(mv_session_t session, int64_t a, int64_t b) {
  return static_cast<mv_status>(guard("mv_video_set_loop", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->video.command([=](mv::player::media_source& s) { s.set_loop(a, b); });
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
    return status::ok;
  }));
}
mv_status MV_CALL mv_video_position(mv_session_t session, int64_t* out) {
  if (!session || !out) return MV_ERR_INVALID_ARG;
  mv::player::media_info info; mv::player::clock_stats stats; mv::player::play_state state;
  session->video.snapshot(info, stats, *out, state); return MV_OK;
}
mv_status MV_CALL mv_video_state(mv_session_t session, uint32_t* out) {
  if (!session || !out) return MV_ERR_INVALID_ARG;
  mv::player::media_info info; mv::player::clock_stats stats;
  mv::player::play_state state; mv::player::time_ns position;
  session->video.snapshot(info, stats, position, state); *out = static_cast<uint32_t>(state); return MV_OK;
}
mv_status MV_CALL mv_video_get_info(mv_session_t session, mv_video_info* out) {
  if (!session || !out) return MV_ERR_INVALID_ARG;
  mv::player::media_info info; mv::player::clock_stats stats;
  mv::player::play_state state; mv::player::time_ns position;
  session->video.snapshot(info, stats, position, state);
  *out = {}; out->duration_ns = info.duration_ns; out->width = info.video.width; out->height = info.video.height;
  out->frame_rate = info.video.frame_rate; out->audio_tracks = info.audio_tracks; out->video_tracks = info.video_tracks;
  out->decoder = static_cast<uint32_t>(info.video.decoder);
  out->flags = (info.has_audio ? 1u : 0u) | (info.video.ten_bit ? 2u : 0u);
  std::memcpy(out->codec_name, info.video.codec_name, sizeof(out->codec_name)); return MV_OK;
}
mv_status MV_CALL mv_video_get_stats(mv_session_t session, mv_video_stats* out) {
  if (!session || !out) return MV_ERR_INVALID_ARG;
  mv::player::media_info info; mv::player::clock_stats stats;
  mv::player::play_state state; mv::player::time_ns position;
  session->video.snapshot(info, stats, position, state);
  *out = {}; out->position_ns = position; out->audio_clock_ns = stats.audio_clock_ns;
  out->err_ms_p50 = stats.err_ms_p50; out->err_ms_p99 = stats.err_ms_p99;
  out->drift_slope_ms_per_min = stats.drift_slope_ms_per_min; out->playback_rate = stats.playback_rate;
  out->frames_presented = stats.counters.presented; out->frames_dropped_late = stats.counters.dropped_late;
  out->holds_cadence = stats.counters.held_cadence; out->holds_starved = stats.counters.held_starved;
  out->device_rebuilds = stats.counters.device_rebuilds; out->position_discontinuities = stats.position_discontinuities;
  out->audio_master = stats.audio_master ? 1u : 0u; out->fallback_reason = static_cast<uint32_t>(stats.fallback);
  return MV_OK;
}
mv_status MV_CALL mv_probe_is_video(mv_session_t session, const char* path, int32_t* out) {
  return static_cast<mv_status>(guard("mv_probe_is_video", [&]() -> status {
    MV_REQUIRE(session && path && out, "session, path and output required");
    auto head = mv::io::read_prefix(path, mv::player::probe_bytes);
    if (!head) return head.error();
    *out = mv::player::is_video(mv::player::probe(head.value())) ? 1 : 0; return status::ok;
  }));
}

}  // extern "C"

namespace mv::abi {

status attach_device(mv_session_t session, ID3D11Device* device) {
  if (!session || !device) return status::invalid_arg;
  {
    std::lock_guard lock(session->device_mutex);
    session->device = device;
  }
  delete session->ready.exchange(nullptr, std::memory_order_acq_rel);

  const generation gen = session->jobs.current_generation();
  gfx::com_ptr<ID3D11Device> dev = session->copy_device();
  if (!dev) return status::ok;

  // Re-upload from the CPU cache on a worker, never on the render thread.
  const job_id id = session->jobs.submit_at(
      gen,
      [session, dev](const job_context& ctx) -> status {
        if (ctx.cancelled()) return status::cancelled;
        std::shared_ptr<image::display_image> cpu;
        {
          std::lock_guard lock(session->image_mutex);
          cpu = session->cpu;
        }
        if (!cpu || cpu->width == 0) return status::ok;
        auto uploaded = image::upload(dev.Get(), *cpu, ctx.gen(), &ctx);
        if (!uploaded) return uploaded.error();
        if (ctx.cancelled()) return status::cancelled;
        auto gpu = std::make_unique<image::gpu_image>(std::move(uploaded).value());
        std::lock_guard lock(session->image_mutex);
        if (ctx.gen() != session->jobs.current_generation()) return status::cancelled;
        if (session->cpu != cpu) return status::cancelled;
        image::gpu_image* old = session->ready.exchange(gpu.release(), std::memory_order_acq_rel);
        delete old;
        if (session->image_ready_event) ::SetEvent(session->image_ready_event);
        return status::ok;
      });
  (void)id;
  return status::ok;
}

void detach_device(mv_session_t session) {
  if (!session) return;
  // In-flight CreateTexture2D against this device must not publish onto the
  // next one (plan/12). CPU cache is kept; attach_device re-uploads.
  session->jobs.bump_generation();
  delete session->ready.exchange(nullptr, std::memory_order_acq_rel);
  std::lock_guard lock(session->device_mutex);
  session->device.Reset();
}

image::gpu_image* take_ready_image(mv_session_t session) {
  if (!session) return nullptr;
  return session->ready.exchange(nullptr, std::memory_order_acq_rel);
}

void* image_ready_wait_handle(mv_session_t session) {
  if (!session) return nullptr;
  return session->image_ready_event;
}

void release_gpu_image(image::gpu_image* image) { delete image; }
bool poll_video(mv_session_t session, player::time_ns vblank, player::video_frame& frame, bool& active) {
  if (!session) { active = false; return false; }
  return session->video.tick(session->jobs.current_generation(), vblank, frame, active);
}
bool video_open(mv_session_t session) noexcept {
  return session != nullptr && session->video.open();
}
player::video_frame* take_ready_video_frame(mv_session_t session, std::uint32_t generation) {
  (void)generation;
  player::video_frame frame; bool active = false;
  if (!poll_video(session, 16'666'667, frame, active)) return nullptr;
  return new player::video_frame(std::move(frame));
}
void release_video_frame(player::video_frame* frame) { delete frame; }


}  // namespace mv::abi
