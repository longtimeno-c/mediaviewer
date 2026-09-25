// SPDX-License-Identifier: GPL-2.0-or-later
// The C ABI implementation. Every exported symbol in this file is `noexcept`
// and does its real work inside mv::abi::guard.

#include "mediaviewer/mediaviewer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "abi/addon_bridge.h"
#include "abi/animation_session.h"
#include "abi/folder_reselect.h"
#include "codec/decode.h"
#include "image/colour.h"
#include "abi/guard.h"
#include "abi/native.h"
#include "abi/video_session.h"
#include "player/container_probe.h"
#include "player/poster.h"
#include "core/job_system.h"
#include "core/result.h"
#include "core/spsc_ring.h"
#include "core/status.h"
#include "core/trace.h"
#include "gfx/device.h"
#include "image/pipeline.h"
#include "image/thumb.h"
#include "image/tiles.h"
#include "canvas/refinement.h"
#include "image/upload.h"
#include "io/dir.h"
#include "io/file.h"
#include "io/pairing.h"
#include "io/sort_order.h"
#include "meta/meta.h"
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
static_assert(offsetof(mv_folder_item, pair_kind) == 24, "mv_folder_item layout is part of the ABI");
static_assert(sizeof(mv_folder_summary) == 16, "mv_folder_summary layout is part of the ABI");
static_assert(static_cast<int>(mv::io::pair_kind::none) == MV_PAIR_NONE, "mv_pair_kind drift");
static_assert(static_cast<int>(mv::io::pair_kind::raw_jpeg) == MV_PAIR_RAW_JPEG, "mv_pair_kind drift");
static_assert(static_cast<int>(mv::io::pair_kind::live_photo) == MV_PAIR_LIVE_PHOTO,
              "mv_pair_kind drift");

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

  // [render-thread only] The last play state a VIDEO_STATE completion was
  // pushed for. poll_video is the only writer and it runs on the render thread,
  // so this needs no synchronisation of its own.
  mv::player::play_state reported_video_state = mv::player::play_state::stopped;

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
  // The CPU pyramid of `cpu` when it is a tiled image, so a device rebuild
  // re-creates the overview without rebuilding the pyramid.
  std::shared_ptr<const mv::image::tile_source> cpu_tiles;
  std::uint64_t cpu_key = 0;  // item identity of `cpu` (canvas/refinement.h)
  mv_image_info info{};
  HANDLE image_ready_event = nullptr;

  // Started with the first tiled image; creates tiles the render thread asks
  // for. Reset before `jobs` is destroyed (it reads the generation).
  std::mutex tile_service_mutex;
  std::unique_ptr<mv::image::tile_service> tile_service;

  // A navigation stop (PR 7). `path` is the primary — what thumbs, decode,
  // prefetch and video detection use; `secondary_path` is the other half of a
  // RAW+JPEG or Live Photo pair, empty when unpaired.
  struct folder_item {
    std::string name;
    std::string path;
    std::string thumb_path;
    std::string secondary_path;
    std::uint64_t size = 0;
    std::int64_t mtime_unix = 0;
    mv::io::pair_kind pair = mv::io::pair_kind::none;
    bool primary_raw = false;
  };
  struct lru_slot {
    std::string path;
    std::unique_ptr<mv::image::gpu_image> gpu;
    mv_image_info info{};
  };

  // PR 9 sort. `folder_listing` is the paired scan in scan order, kept so a new
  // order (or a batch of date-taken stamps) re-applies without another disk
  // scan. `date_stamps` is checked per (mtime, size): a rewritten file is read
  // again, and a file with no stamp is remembered as such rather than retried.
  struct date_stamp {
    std::int64_t mtime_unix = 0;
    std::uint64_t size = 0;
    std::optional<std::int64_t> key;
  };
  std::atomic<std::int32_t> sort_packed{0};
  std::vector<mv::io::listed_item> folder_listing;
  std::unordered_map<std::string, date_stamp> date_stamps;

  std::mutex folder_mutex;
  std::vector<folder_item> folder_items;
  // PR 26 folder tiles: child directories of folder_dir, natural order.
  struct folder_card {
    std::string name;
    std::string path;
    std::uint32_t media_count = 0;
    std::uint32_t subdir_count = 0;
    bool requested = false;
    bool loaded = false;
    bool photos_inside = false;
    bool search_incomplete = false;
    std::string cover_thumb;
  };
  std::vector<folder_card> folder_subdirs;
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

  // The animation's colour transform, built once per published source (review
  // note 34) instead of per frame. Touched only by the animation decode thread
  // (the texture maker below), so it needs no lock. Keyed by the generation and
  // the profile itself (its size and first 64 bytes, which hold the header's
  // size, class, colour space and date) so a new animation never reuses an old
  // one, not by a buffer address that could be reused.
  static constexpr std::size_t kIccKeyBytes = 64;
  std::unique_ptr<mv::image::display_transform> anim_transform;
  std::size_t anim_icc_size = 0;
  std::array<std::uint8_t, kIccKeyBytes> anim_icc_head{};
  std::uint32_t anim_icc_gen = 0;
  std::atomic<std::uint32_t> anim_icc_us{0};  // F3: last frame's colour conversion

  // PR 6 animation feed. Declared after everything its decode thread uses, so
  // it is destroyed (and its thread joined) first. Each frame is colour
  // managed like a still and uploaded top level only: an animation is not
  // mip-mapped, so far below 100 % it can alias.
  mv::abi::animation_session<mv::image::gpu_image> animation{
      [this](const mv::codec::canvas_frame& frame, const mv::codec::animation_info& info,
             std::uint32_t generation) -> std::unique_ptr<mv::image::gpu_image> {
        auto dev = copy_device();
        if (!dev) return nullptr;
        mv::codec::raster raster;
        raster.width = info.width;
        raster.height = info.height;
        raster.format = info.format;
        raster.intent = mv::codec::transfer_intent::display_referred;
        raster.rgba = frame.rgba;
        raster.tagged_srgb = info.tagged_srgb;
        const auto t0 = std::chrono::steady_clock::now();
        mv::result<mv::image::display_image> display = mv::err(mv::status::internal);
        if (info.icc.empty()) {
          display = mv::image::to_display(std::move(raster));  // copy-through
        } else {
          const std::span<const std::uint8_t> icc(info.icc.data(), info.icc.size());
          const std::span<const std::uint8_t> head =
              icc.first(std::min<std::size_t>(icc.size(), kIccKeyBytes));
          const bool same_profile = anim_transform && anim_icc_gen == generation &&
                                    anim_icc_size == icc.size() &&
                                    std::equal(head.begin(), head.end(), anim_icc_head.begin());
          if (!same_profile) {
            auto made = mv::image::display_transform::create(icc);
            if (!made) return nullptr;  // D6: a broken profile is not untagged sRGB
            anim_transform = std::move(made).value();
            anim_icc_size = icc.size();
            anim_icc_head.fill(0);
            std::copy(head.begin(), head.end(), anim_icc_head.begin());
            anim_icc_gen = generation;
          }
          display = anim_transform->apply(std::move(raster));
        }
        anim_icc_us.store(static_cast<std::uint32_t>(
                              std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count()),
                          std::memory_order_relaxed);
        if (!display) return nullptr;
        auto uploaded = mv::image::upload(dev.Get(), display.value(), generation, nullptr, 1);
        if (!uploaded) return nullptr;
        return std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
      }};

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

std::uint64_t key_for(const std::string& path) noexcept {
  return mv::canvas::item_key_for(path.data(), path.size());
}

// [caller holds image_mutex] Stores the CPU copy and hands the texture to the
// render thread, stamped with who it is (plan/04 step 4): the item key the
// caller decoded for and the view generation at this moment. A second publish
// with the same stamp is a refinement the render thread fades in without
// moving the camera; anything else is navigation.
//
// `key` is the identity of the folder entry, not necessarily of the file that
// was decoded: a pairing that shows a JPEG and then its RAW passes the same
// key for both so the RAW refines the JPEG.
void publish_locked(mv_session* session, std::uint64_t key, mv_image_info info,
                    std::shared_ptr<mv::image::display_image> cpu,
                    std::unique_ptr<mv::image::gpu_image> gpu) {
  session->info = info;
  if (cpu) {
    session->cpu = std::move(cpu);
    session->cpu_tiles.reset();
    session->cpu_key = key;
  }
  if (gpu) {
    gpu->item_key = key;
    gpu->view_generation = session->jobs.current_generation();
    if (gpu->tiles) session->cpu_tiles = gpu->tiles->source();
    mv::image::gpu_image* old = session->ready.exchange(gpu.release(), std::memory_order_acq_rel);
    delete old;
    if (session->image_ready_event) ::SetEvent(session->image_ready_event);
  }
}

bool publish_view(mv_session* session, const mv::job_context& ctx, std::uint64_t key,
                  mv_image_info info, std::shared_ptr<mv::image::display_image> cpu,
                  std::unique_ptr<mv::image::gpu_image> gpu) {
  std::lock_guard lock(session->image_mutex);
  if (ctx.gen() != session->jobs.current_generation()) return false;
  publish_locked(session, key, info, std::move(cpu), std::move(gpu));
  return true;
}

void publish_ready(mv_session* session, std::uint64_t key, mv_image_info info,
                   std::shared_ptr<mv::image::display_image> cpu,
                   std::unique_ptr<mv::image::gpu_image> gpu) {
  std::lock_guard lock(session->image_mutex);
  publish_locked(session, key, info, std::move(cpu), std::move(gpu));
}

void tiles_landed(void* user) noexcept {
  auto* session = static_cast<mv_session*>(user);
  if (session->image_ready_event) ::SetEvent(session->image_ready_event);
}

mv::image::tile_service& tile_service_for(mv_session* session) {
  std::lock_guard lock(session->tile_service_mutex);
  if (!session->tile_service) {
    session->tile_service =
        std::make_unique<mv::image::tile_service>(&session->jobs, &tiles_landed, session);
  }
  return *session->tile_service;
}

// [worker] The full-resolution GPU form of a decoded still: one texture, or -
// above ~64 MP or past the 16384 texture limit - a tiled pyramid with its
// overview (plan/04). `mip_limit` 1 is the top-level-only staging upload of
// the single-texture path; a tiled image has no such stage.
status upload_still(mv_session* session, ID3D11Device* device,
                    const std::shared_ptr<mv::image::display_image>& cpu,
                    const mv::job_context& ctx, std::uint32_t mip_limit,
                    std::unique_ptr<mv::image::gpu_image>& out) {
  if (mv::image::needs_tiles(cpu->width, cpu->height)) {
    std::shared_ptr<const mv::image::tile_source> reuse;
    {
      std::lock_guard lock(session->image_mutex);
      reuse = session->cpu_tiles;
    }
    auto uploaded = mv::image::upload_tiled(device, cpu, ctx.gen(), tile_service_for(session),
                                            &ctx, std::move(reuse));
    if (!uploaded) return uploaded.error();
    out = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
    return status::ok;
  }
  auto uploaded = mv::image::upload(device, *cpu, ctx.gen(), &ctx, mip_limit);
  if (!uploaded) return uploaded.error();
  out = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
  out->quality = mip_limit == 1 ? mv::image::gpu_quality::full_top : mv::image::gpu_quality::full;
  return status::ok;
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
  // A tiled image is not cached across navigation: its tiles are view-tied
  // and its CPU pyramid is hundreds of MB. A revisit decodes again.
  if (gpu.tiles) return;
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
  // The CPU copy belongs to the previous decode, not to this item: keeping it
  // pinned a 400 MB still behind a cached phone JPEG, and a device rebuild
  // would have re-uploaded the wrong picture.
  session->cpu.reset();
  session->cpu_tiles.reset();
  session->cpu_key = 0;
  gpu->quality = mv::image::gpu_quality::full;
  publish_locked(session, key_for(path), info, nullptr, std::move(gpu));
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
// PR 6 animation, only for the item on screen and only after its still is up
// (rule 3: frame 0 is the first pixel). Prefetched neighbours never open one.
// A still (one-frame WebP, PNG without acTL) is refused by open_animation; a
// one-frame GIF is found out by the feed and ends there.
void maybe_publish_animation(mv_session* session, const std::string& path,
                             std::vector<std::uint8_t>&& bytes, const mv::job_context& ctx) {
  if (ctx.cancelled() || !path_is_selected(session, path)) return;
  const auto family = mv::codec::probe(bytes);
  if (family != mv::codec::format_family::gif && family != mv::codec::format_family::webp &&
      family != mv::codec::format_family::png) {
    return;
  }
  auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
  auto opened = mv::codec::open_animation(std::move(shared));
  if (!opened) return;
  if (ctx.cancelled() || !path_is_selected(session, path)) return;
  session->animation.publish(std::move(opened).value(), ctx.gen());
}

// An item served from the LRU (a revisit) never reaches the decode path, so its
// animation is opened here: a worker probes the magic bytes and only reads the
// whole file for an animated family.
void submit_animation_open(mv_session* session, std::string path, mv::generation gen) {
  (void)session->jobs.submit_at(
      gen, [session, path = std::move(path)](const mv::job_context& ctx) -> status {
        if (ctx.cancelled() || !path_is_selected(session, path)) return status::ok;
        auto head = mv::io::read_prefix(path, 16);
        if (!head) return head.error();
        const auto family = mv::codec::probe(head.value());
        if (family != mv::codec::format_family::gif && family != mv::codec::format_family::webp &&
            family != mv::codec::format_family::png) {
          return status::ok;
        }
        auto bytes = mv::io::read_all(path);
        if (!bytes) return bytes.error();
        maybe_publish_animation(session, path, std::move(bytes).value(), ctx);
        return status::ok;
      });
}

void submit_decode_to_lru(mv_session* session, std::string path, mv::generation gen) {
  if (!claim_decode(session, path, gen)) return;
  const std::uint32_t folder_gen = session->folder_generation.load(std::memory_order_relaxed);
  const auto correlation = mv::abi::current_correlation_id();
  (void)session->jobs.submit_at(
      gen,
      [session, path, folder_gen, correlation](const mv::job_context& ctx) -> status {
        const mv::crash_context::correlation_scope crash_cid(correlation);  // plan/13
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
        bool raw_preview_ready = false;
        if (path_is_selected(session, path)) {
          if (auto preview = mv::image::decode_preview(bytes.value(), &ctx)) {
            if (ctx.cancelled()) return status::cancelled;
            if (auto dev = session->copy_device()) {
              auto uploaded = mv::image::upload(dev.Get(), preview.value(), ctx.gen(), &ctx, 1);
              if (uploaded) {
                auto gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
                gpu->quality = mv::image::gpu_quality::preview;
                if (path_is_selected(session, path)) {
                  publish_ready(session, key_for(path), info_from(preview.value()), nullptr,
                                std::move(gpu));
                  raw_preview_ready = preview->format == mv::codec::format_family::raw;
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

        auto decoded = mv::image::decode_bytes(
            bytes.value(), &ctx,
            path_is_selected(session, path) ? mv::codec::raw_foreground_threads() : 1u);
        if (!decoded) return decoded.error();
        if (ctx.cancelled()) return status::cancelled;
        if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
          return status::cancelled;
        }
        auto cpu = std::make_shared<mv::image::display_image>(std::move(decoded).value());
        const mv_image_info info = info_from(*cpu);
        const bool tiled = mv::image::needs_tiles(cpu->width, cpu->height);
        // A tiled neighbour is not prefetched: it would never enter the LRU
        // (lru_put) and its pyramid is the expensive part. Dropped here.
        if (tiled && !path_is_selected(session, path)) return status::ok;

        auto upload_and_publish = [&](std::uint32_t mip_limit) -> status {
          auto dev = session->copy_device();
          if (!dev) {
            if (path_is_selected(session, path)) {
              publish_ready(session, key_for(path), info, cpu, nullptr);
            }
            return status::ok;
          }
          std::unique_ptr<mv::image::gpu_image> gpu;
          const status up = upload_still(session, dev.Get(), cpu, ctx, mip_limit, gpu);
          if (up != status::ok) return up;
          if (ctx.cancelled()) return status::cancelled;
          lru_put(session, path, *gpu, info);
          if (path_is_selected(session, path)) {
            publish_ready(session, key_for(path), info, cpu, std::move(gpu));
            push_image_opened(session, correlation, ctx.gen(), status::ok);
          }
          return status::ok;
        };

        // The CPU mip pyramid of a large still costs more than the decode did.
        // Get the top level on screen first, then pay for the pyramid - same
        // staging mv_image_open already uses (plan/04). A tiled image has no
        // such stage: its overview is small and its tiles come on demand.
        const bool large = !tiled &&
            static_cast<std::uint64_t>(cpu->width) * cpu->height >= 2048ull * 2048ull;
        // A RAW already has a usable preview. Publish its full texture once,
        // with mips, avoiding a second large upload during the cross-fade.
        if (large && !raw_preview_ready && path_is_selected(session, path)) {
          const status first = upload_and_publish(1);
          if (first != status::ok) return first;
          if (ctx.cancelled()) return status::cancelled;
        }
        const status still = upload_and_publish(0);
        if (still != status::ok) return still;
        maybe_publish_animation(session, path, std::move(bytes).value(), ctx);
        return status::ok;
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

void apply_folder_list(mv_session* session, std::vector<mv::io::listed_item> listed,
                       bool changed);

// Child folders for gallery tiles. Additive: a failed subdir scan leaves the
// previous tiles rather than wiping a listing that still has files.
void refresh_subdirs(mv_session* session) {
  std::string dir;
  {
    std::lock_guard lock(session->folder_mutex);
    dir = session->folder_dir;
  }
  if (dir.empty()) return;
  auto scanned = mv::io::list_subfolders(dir);
  if (!scanned) return;
  std::vector<mv::io::subdir_entry> listed = std::move(scanned).value();
  std::lock_guard lock(session->folder_mutex);
  std::vector<mv_session::folder_card> next;
  next.reserve(listed.size());
  for (auto& e : listed) {
    mv_session::folder_card card;
    card.name = std::move(e.name_utf8);
    card.path = std::move(e.path_utf8);
    for (const auto& old : session->folder_subdirs) {
      if (old.path == card.path) {
        card.media_count = old.media_count;
        card.subdir_count = old.subdir_count;
        card.requested = old.requested;
        card.loaded = old.loaded;
        card.cover_thumb = old.cover_thumb;
        break;
      }
    }
    next.push_back(std::move(card));
  }
  session->folder_subdirs = std::move(next);
}

// JPEG-512 for a path that may not be in the current listing (a tile cover
// borrowed from a descendant). Same cache as filmstrip thumbs.
mv::result<std::string> cached_thumb_path(mv_session* session, const std::string& path,
                                          std::int64_t mtime_unix, std::uint64_t size,
                                          const mv::job_context& ctx, std::uint32_t folder_gen) {
  if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
    return mv::err(status::cancelled);
  }
  if (!session->thumbs.is_open()) {
    if (auto dir = mv::io::thumb_cache_dir()) (void)session->thumbs.open(dir.value());
  }
  const mv::image::thumb_key key{path, mtime_unix, size};
  if (auto hit = session->thumbs.lookup(key); hit && !hit.value().empty()) return hit.value();
  std::vector<std::uint8_t> jpeg_bytes;
  if (video_path(path)) {
    auto poster = mv::player::poster_frame(path.c_str(), mv::image::kThumbLongEdge, &ctx);
    if (!poster) return mv::err(poster.error());
    if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
      return mv::err(status::cancelled);
    }
    auto encoded = mv::image::encode_thumb_rgba(poster.value().rgba, poster.value().width,
                                                poster.value().height);
    if (!encoded) return mv::err(encoded.error());
    jpeg_bytes = std::move(encoded).value();
  } else {
    auto bytes = mv::io::read_all(path);
    if (!bytes) return mv::err(bytes.error());
    if (session->folder_generation.load(std::memory_order_relaxed) != folder_gen) {
      return mv::err(status::cancelled);
    }
    auto jpeg = mv::image::make_thumb_jpeg(bytes.value(), &ctx);
    if (!jpeg) return mv::err(jpeg.error());
    jpeg_bytes = std::move(jpeg).value();
  }
  return session->thumbs.store(key, jpeg_bytes);
}

void submit_folder_summary(mv_session* session, uint32_t index) {
  std::string path;
  std::uint32_t folder_gen = 0;
  {
    std::lock_guard lock(session->folder_mutex);
    if (index >= session->folder_subdirs.size()) return;
    auto& card = session->folder_subdirs[index];
    if (card.requested || card.loaded) return;
    card.requested = true;
    path = card.path;
    folder_gen = session->folder_generation.load(std::memory_order_relaxed);
  }
  const auto correlation = mv::abi::current_correlation_id();
  (void)session->jobs.submit_at(
      mv::background_generation,
      [session, path, folder_gen](const mv::job_context& ctx) -> status {
        const auto stale = [&] {
          return session->folder_generation.load(std::memory_order_relaxed) != folder_gen;
        };
        if (stale()) {
          std::lock_guard lock(session->folder_mutex);
          for (auto& card : session->folder_subdirs) {
            if (card.path == path) card.requested = false;
          }
          return status::cancelled;
        }
        auto summary = mv::io::summarize_dir(path);
        if (!summary) {
          std::lock_guard lock(session->folder_mutex);
          for (auto& card : session->folder_subdirs) {
            if (card.path == path) card.requested = false;
          }
          return summary.error();
        }
        const auto value = std::move(summary).value();
        std::string cover_thumb;
        if (value.has_cover) {
          auto thumb = cached_thumb_path(session, value.cover.path_utf8, value.cover.mtime_unix,
                                         value.cover.size, ctx, folder_gen);
          if (thumb) cover_thumb = std::move(thumb).value();
        }
        std::lock_guard lock(session->folder_mutex);
        for (auto& card : session->folder_subdirs) {
          if (card.path != path) continue;
          card.media_count = value.media_count;
          card.subdir_count = value.subdir_count;
          card.photos_inside = value.photos_inside;
          card.search_incomplete = value.search_incomplete;
          card.loaded = true;
          card.cover_thumb = std::move(cover_thumb);
          break;
        }
        return status::ok;
      },
      [session, correlation, index, folder_gen](mv::job_id id, mv::generation, status result) {
        mv_completion c{};
        c.kind = MV_COMPLETION_FOLDER_SUMMARY;
        c.status = static_cast<uint32_t>(result);
        c.job_id = id;
        c.correlation_id = correlation;
        c.generation = folder_gen;
        c.payload = static_cast<int64_t>(index);
        session->push_completion(c);
      });
}

// Orders `listed` by the session's sort. Sorts the primaries with the shared
// comparator and carries each pair along by path. Date taken consults the stamps
// already read; a file without one sorts by its mtime (io/sort_order.h).
void sort_listed(mv_session* session, std::vector<mv::io::listed_item>& listed) {
  const mv::io::sort_order order = mv::io::unpack_sort(session->sort_packed.load());
  std::vector<mv::io::dir_entry> primaries;
  primaries.reserve(listed.size());
  for (const auto& l : listed) primaries.push_back(l.primary);
  mv::io::sort_entries(primaries, order, [session](const mv::io::dir_entry& e) {
    std::lock_guard lock(session->folder_mutex);
    const auto it = session->date_stamps.find(e.path_utf8);
    if (it == session->date_stamps.end() || it->second.mtime_unix != e.mtime_unix ||
        it->second.size != e.size) {
      return std::optional<std::int64_t>{};
    }
    return it->second.key;
  });
  std::unordered_map<std::string, std::size_t> at;
  at.reserve(listed.size());
  for (std::size_t i = 0; i < listed.size(); ++i) at.emplace(listed[i].primary.path_utf8, i);
  std::vector<mv::io::listed_item> sorted;
  sorted.reserve(listed.size());
  for (const auto& e : primaries) sorted.push_back(std::move(listed[at[e.path_utf8]]));
  listed = std::move(sorted);
}

// Date taken needs a bounded read of every file. One background job fills the
// stamps still missing, then re-applies the listing once. A second call for the
// same listing finds nothing missing and does nothing, so this cannot loop.
void resolve_date_stamps(mv_session* session) {
  std::vector<mv::io::dir_entry> missing;
  std::string dir;
  {
    std::lock_guard lock(session->folder_mutex);
    dir = session->folder_dir;
    for (const auto& l : session->folder_listing) {
      const auto it = session->date_stamps.find(l.primary.path_utf8);
      if (it == session->date_stamps.end() || it->second.mtime_unix != l.primary.mtime_unix ||
          it->second.size != l.primary.size) {
        missing.push_back(l.primary);
      }
    }
  }
  if (missing.empty()) return;
  (void)session->jobs.submit_at(
      mv::background_generation,
      [session, dir, missing = std::move(missing)](const mv::job_context&) -> status {
        for (const auto& e : missing) {
          const auto key = mv::meta::read_date_taken(e.path_utf8);
          std::lock_guard lock(session->folder_mutex);
          if (session->folder_dir != dir) return status::ok;  // the user moved on
          session->date_stamps[e.path_utf8] = {e.mtime_unix, e.size, key};
        }
        std::vector<mv::io::listed_item> again;
        {
          std::lock_guard lock(session->folder_mutex);
          if (session->folder_dir != dir) return status::ok;
          again = session->folder_listing;
        }
        if (mv::io::unpack_sort(session->sort_packed.load()).key ==
            mv::io::sort_key::date_taken) {
          apply_folder_list(session, std::move(again), true);
        }
        return status::ok;
      });
}

// `listed` is already paired (plan/16 speed rule 4: pairing happens at scan,
// never per next), so every folder_items entry is one arrow-key stop. It arrives
// in scan order; this puts it in the session's sort order.
void apply_folder_list(mv_session* session, std::vector<mv::io::listed_item> listed,
                       bool changed) {
  {
    std::lock_guard lock(session->folder_mutex);
    session->folder_listing = listed;
  }
  refresh_subdirs(session);
  sort_listed(session, listed);
  if (mv::io::unpack_sort(session->sort_packed.load()).key == mv::io::sort_key::date_taken) {
    resolve_date_stamps(session);
  }
  std::string want;
  std::string previous_path;
  std::string previous_secondary;
  uint32_t previous_index = 0;
  {
    std::lock_guard lock(session->folder_mutex);
    want = session->folder_select_path;
    previous_index = session->folder_selected;
    if (changed && previous_index < session->folder_items.size()) {
      previous_path = session->folder_items[previous_index].path;
      previous_secondary = session->folder_items[previous_index].secondary_path;
    }
    session->folder_items.clear();
    session->folder_items.reserve(listed.size());
    for (auto& e : listed) {
      mv_session::folder_item it;
      it.primary_raw = mv::io::is_raw_name(e.primary.name_utf8);
      it.name = std::move(e.primary.name_utf8);
      it.path = std::move(e.primary.path_utf8);
      it.size = e.primary.size;
      it.mtime_unix = e.primary.mtime_unix;
      it.secondary_path = std::move(e.secondary.path_utf8);
      it.pair = e.kind;
      session->folder_items.push_back(std::move(it));
    }
    // A watcher refresh keeps the stop the user is on (or lets the next one
    // slide in if it was removed); only a fresh open goes to `want`. Either
    // half of a pair names its stop, so opening the .NEF selects JPEG+NEF.
    session->folder_selected = mv::abi::reselect_pair(
        session->folder_items,
        [](const mv_session::folder_item& it) -> const std::string& { return it.path; },
        [](const mv_session::folder_item& it) -> const std::string& { return it.secondary_path; },
        changed, previous_path, previous_secondary, previous_index, want);
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
    // A file appearing beside the current one is not a reason to decode it
    // again: re-publishing would refit the camera under the user.
    if (!(changed && selected_path == previous_path)) {
      submit_decode_to_lru(session, selected_path, gen);
    }
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
    apply_folder_list(session, mv::io::pair_listing(std::move(listed).value()), true);
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
    session->tile_service.reset();  // joins; reads jobs' generation until then
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
    // Opening is submitted at the caller's current view generation. The host
    // bumps first when this is a new view intent (plan/14); doing it again here
    // made the managed OpenImage wrapper advance twice.
    const mv::generation gen = session->jobs.current_generation();

    const mv::job_id id = session->jobs.submit_at(
        gen,
        [session, path = std::move(owned), correlation](const mv::job_context& ctx) -> status {
          const mv::crash_context::correlation_scope crash_cid(correlation);  // plan/13
          if (ctx.cancelled()) return status::cancelled;

          if (video_path(path)) return open_video_worker(session, path, ctx);
          auto bytes = mv::io::read_all(path);
          if (!bytes) return bytes.error();
          if (ctx.cancelled()) return status::cancelled;

          // First pixel: JPEG DCT 1/4. Fit-to-window of the preview fills the
          // same rect as the full image; 100 % during load is briefly small.
          bool raw_preview_ready = false;
          if (auto preview = mv::image::decode_preview(bytes.value(), &ctx)) {
            if (ctx.cancelled()) return status::cancelled;
            std::unique_ptr<mv::image::gpu_image> gpu;
            if (auto dev = session->copy_device()) {
              auto uploaded = mv::image::upload(dev.Get(), preview.value(), ctx.gen(), &ctx);
              if (uploaded) {
                gpu = std::make_unique<mv::image::gpu_image>(std::move(uploaded).value());
                gpu->quality = mv::image::gpu_quality::preview;
              } else if (uploaded.error() == status::cancelled) {
                return status::cancelled;
              }
            }
            if (gpu) {
              raw_preview_ready = publish_view(session, ctx, key_for(path),
                  info_from(preview.value()), nullptr, std::move(gpu)) &&
                  preview->format == mv::codec::format_family::raw;
            }
          } else if (preview.error() == status::cancelled) {
            return status::cancelled;
          }

          auto decoded = mv::image::decode_bytes(bytes.value(), &ctx,
                                                 mv::codec::raw_foreground_threads());
          if (!decoded) return decoded.error();
          if (ctx.cancelled()) return status::cancelled;

          auto cpu = std::make_shared<mv::image::display_image>(std::move(decoded).value());
          const mv_image_info info = info_from(*cpu);
          // CPU cache first so a device rebuild can re-upload if CreateTexture2D
          // is still in flight against the old device (plan/12).
          if (!publish_view(session, ctx, key_for(path), info, cpu, nullptr)) {
            return status::cancelled;
          }

          const bool large = !mv::image::needs_tiles(cpu->width, cpu->height) &&
              static_cast<std::uint64_t>(cpu->width) * cpu->height >= 2048ull * 2048ull;
          auto upload_and_publish = [&](std::uint32_t mip_limit) -> status {
            auto dev = session->copy_device();
            if (!dev) return status::ok;
            std::unique_ptr<mv::image::gpu_image> gpu;
            const status up = upload_still(session, dev.Get(), cpu, ctx, mip_limit, gpu);
            if (up != status::ok) return up;
            if (ctx.cancelled()) return status::cancelled;
            lru_put(session, path, *gpu, info);
            if (!publish_view(session, ctx, key_for(path), info, nullptr, std::move(gpu))) {
              return status::cancelled;
            }
            return status::ok;
          };

          if (large && !raw_preview_ready) {
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
      session->folder_listing.clear();
      session->folder_items.clear();
      session->folder_subdirs.clear();
      session->folder_selected = 0;
    }
    if (!session->thumbs.is_open()) {
      if (auto cache = mv::io::thumb_cache_dir()) (void)session->thumbs.open(cache.value());
    }

    const auto correlation = mv::abi::current_correlation_id();
    const mv::job_id id = session->jobs.submit_at(
        mv::background_generation,
        [session, dir](const mv::job_context&) -> status {
          // Arm the watcher before listing: FOLDER_READY is pushed from inside
          // apply_folder_list, and a file that landed between that completion
          // and a later start() was never seen. A change during the scan now
          // costs one extra refresh instead.
          (void)session->watcher.start(dir, &on_folder_watch, session);
          auto listed = mv::io::list_still_files(dir);
          if (!listed) return listed.error();
          apply_folder_list(session, mv::io::pair_listing(std::move(listed).value()), false);
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

mv_status MV_CALL mv_folder_set_sort(mv_session_t session, int32_t packed) {
  return static_cast<mv_status>(guard("mv_folder_set_sort", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    // Normalise so a stray bit cannot be stored: unknown key -> name.
    const auto order = mv::io::unpack_sort(packed);
    const std::int32_t next = mv::io::pack_sort(order);
    if (session->sort_packed.exchange(next) == next) return status::ok;
    bool have = false;
    {
      std::lock_guard lock(session->folder_mutex);
      have = !session->folder_listing.empty();
    }
    if (!have) return status::ok;  // applies to the next open
    const mv::job_id id = session->jobs.submit_at(
        mv::background_generation, [session](const mv::job_context&) -> status {
          std::vector<mv::io::listed_item> again;
          {
            std::lock_guard lock(session->folder_mutex);
            again = session->folder_listing;
          }
          if (again.empty()) return status::ok;
          apply_folder_list(session, std::move(again), true);
          return status::ok;
        });
    return id == mv::invalid_job ? status::internal : status::ok;
  }));
}

mv_status MV_CALL mv_folder_get_sort(mv_session_t session, int32_t* out_packed) {
  return static_cast<mv_status>(guard("mv_folder_get_sort", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_packed != nullptr, "out_packed must not be null");
    *out_packed = session->sort_packed.load();
    return status::ok;
  }));
}

mv_status MV_CALL mv_list_subdirectories(const char* utf8_dir, char* utf8, uint32_t cap,
                                         uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_list_subdirectories", [&]() -> status {
    MV_REQUIRE(utf8_dir != nullptr && utf8_dir[0] != '\0', "utf8_dir must not be empty");
    auto dirs = mv::io::list_subdirectories(utf8_dir);
    if (!dirs) return dirs.error();
    const auto flat = [](std::string v) {
      for (char& c : v) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
      }
      return v;
    };
    std::string out;
    for (const auto& d : dirs.value()) out += flat(d.name_utf8) + "\t" + d.path_utf8 + "\n";
    return copy_utf8(out, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_forget(mv_session_t session, const char* utf8_path) {
  return static_cast<mv_status>(guard("mv_folder_forget", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(utf8_path != nullptr && utf8_path[0] != '\0', "utf8_path must not be empty");
    const std::string path(utf8_path);
    std::lock_guard lock(session->lru_mutex);
    std::erase_if(session->lru, [&](const mv_session::lru_slot& s) { return s.path == path; });
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
    item.flags = (index == session->folder_selected ? 1u : 0u) | (it.primary_raw ? 2u : 0u);
    item.size_bytes = it.size;
    item.mtime_unix = it.mtime_unix;
    item.pair_kind = static_cast<uint32_t>(it.pair);
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

mv_status MV_CALL mv_folder_item_pair_path(mv_session_t session, uint32_t index, char* utf8,
                                           uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_item_pair_path", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_items.size(), "index out of range");
    return copy_utf8(session->folder_items[index].secondary_path, utf8, cap, out_bytes);
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
      submit_animation_open(session, path, gen);
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
    session->folder_subdirs.clear();
    session->folder_selected = 0;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_directory(mv_session_t session, char* utf8, uint32_t cap,
                                      uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_directory", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    return copy_utf8(session->folder_dir, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_subfolder_count(mv_session_t session, uint32_t* out_count) {
  return static_cast<mv_status>(guard("mv_folder_subfolder_count", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_count != nullptr, "out_count must not be null");
    std::lock_guard lock(session->folder_mutex);
    *out_count = static_cast<uint32_t>(session->folder_subdirs.size());
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_subfolder_name(mv_session_t session, uint32_t index, char* utf8,
                                           uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_subfolder_name", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_subdirs.size(), "index out of range");
    return copy_utf8(session->folder_subdirs[index].name, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_subfolder_path(mv_session_t session, uint32_t index, char* utf8,
                                           uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_subfolder_path", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_subdirs.size(), "index out of range");
    return copy_utf8(session->folder_subdirs[index].path, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_summary_at(mv_session_t session, uint32_t index,
                                       mv_folder_summary* out_summary) {
  return static_cast<mv_status>(guard("mv_folder_summary_at", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_summary != nullptr, "out_summary must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_subdirs.size(), "index out of range");
    const auto& card = session->folder_subdirs[index];
    mv_folder_summary s{};
    s.media_count = card.media_count;
    s.subdir_count = card.subdir_count;
    if (card.loaded) s.flags |= 1u;
    if (!card.cover_thumb.empty()) s.flags |= 2u;
    if (card.photos_inside) s.flags |= 4u;
    if (card.search_incomplete) s.flags |= 8u;
    *out_summary = s;
    return status::ok;
  }));
}

mv_status MV_CALL mv_folder_summary_cover_thumb_path(mv_session_t session, uint32_t index,
                                                     char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(guard("mv_folder_summary_cover_thumb_path", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    std::lock_guard lock(session->folder_mutex);
    MV_REQUIRE(index < session->folder_subdirs.size(), "index out of range");
    return copy_utf8(session->folder_subdirs[index].cover_thumb, utf8, cap, out_bytes);
  }));
}

mv_status MV_CALL mv_folder_request_summary(mv_session_t session, uint32_t index) {
  return static_cast<mv_status>(guard("mv_folder_request_summary", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    submit_folder_summary(session, index);
    return status::ok;
  }));
}

mv_status MV_CALL mv_video_open(mv_session_t session, const char* path, uint64_t* job) {
  // Unlike mv_image_open, the legacy video entry point owns the view-intent
  // bump (its public contract promises that it does).
  if (!valid(session) || !path || path[0] == '\0') return MV_ERR_INVALID_ARG;
  const mv_status bumped = mv_session_bump_generation(session, nullptr);
  if (bumped != MV_OK) return bumped;
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
        std::uint64_t key = 0;
        {
          std::lock_guard lock(session->image_mutex);
          cpu = session->cpu;
          key = session->cpu_key;
        }
        if (!cpu || cpu->width == 0) return status::ok;
        std::unique_ptr<image::gpu_image> gpu;
        const status up = upload_still(session, dev.Get(), cpu, ctx, 0, gpu);
        if (up != status::ok) return up;
        if (ctx.cancelled()) return status::cancelled;
        {
          std::lock_guard lock(session->image_mutex);
          if (ctx.gen() != session->jobs.current_generation()) return status::cancelled;
          if (session->cpu != cpu) return status::cancelled;
          publish_locked(session, key, session->info, nullptr, std::move(gpu));
        }
        // Review note 37: the device rebuild bumped the generation, which
        // retired the animation. The still is back; bring the animation back
        // with it (a probe job, animated families only). Not under
        // image_mutex: the folder lock is taken on its own.
        std::string selected;
        {
          std::lock_guard folder_lock(session->folder_mutex);
          if (session->folder_selected < session->folder_items.size()) {
            selected = session->folder_items[session->folder_selected].path;
          }
        }
        if (!selected.empty()) submit_animation_open(session, std::move(selected), ctx.gen());
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

void release_gpu_image(image::gpu_image* image) {
  // A tiled image's set is freed by its service thread, not here: poke it so
  // a retired pyramid does not wait for the next tile request.
  image::tile_service* service = image && image->tiles ? image->tiles->service() : nullptr;
  delete image;
  if (service) service->poke();
}

std::span<const gfx::tile_quad> tiles_frame(const image::gpu_image& image,
                                            const image::tile_view& view) noexcept {
  if (!image.tiles) return {};
  return image.tiles->frame(view);
}

image::tile_stats tiles_stats(const image::gpu_image& image) noexcept {
  return image.tiles ? image.tiles->stats() : image::tile_stats{};
}

bool take_animation_frame(mv_session_t session, std::uint32_t generation,
                          image::gpu_image*& texture, std::uint32_t& delay_ms,
                          std::uint32_t& index) noexcept {
  if (!session) return false;
  mv::abi::animation_frame<image::gpu_image> frame;
  if (!session->animation.take(generation, frame)) return false;
  texture = frame.texture;
  delay_ms = frame.delay_ms;
  index = frame.index;
  return true;
}

bool animation_open(mv_session_t session, std::uint32_t generation) noexcept {
  return session && session->animation.open(generation);
}

bool animation_finished(mv_session_t session, std::uint32_t generation) noexcept {
  return session && session->animation.finished(generation);
}

bool animation_loops_forever(mv_session_t session) noexcept {
  return session && session->animation.loops_forever();
}

void animation_retire(mv_session_t session, std::uint32_t generation) noexcept {
  if (session) session->animation.retire(generation);
}

void animation_seek(mv_session_t session, std::uint32_t index) noexcept {
  if (session) session->animation.seek(index);
}

animation_stats animation_stats_now(mv_session_t session) noexcept {
  animation_stats stats{};
  if (!session) return stats;
  stats.depth = session->animation.depth();
  stats.queued = session->animation.queued();
  stats.last_upload_us = session->animation.last_upload_us();
  stats.last_icc_us = session->anim_icc_us.load(std::memory_order_relaxed);
  stats.frames_made = session->animation.frames_made();
  return stats;
}
bool poll_video(mv_session_t session, player::time_ns vblank, player::video_frame& frame, bool& active) {
  if (!session) { active = false; return false; }
  const bool changed =
      session->video.tick(session->jobs.current_generation(), vblank, frame, active);

  // MV_COMPLETION_VIDEO_STATE / _VIDEO_ENDED have been declared since ABI 0.4
  // and were never pushed, so the chrome had no way to learn about a state the
  // core changes on its own — reaching the end of a clip, most of all — and
  // could only find out on its next poll. plan/14: the ABI is designed, not
  // retrofitted, and declared surface that nothing sends is not a design.
  //
  // Every transition is pushed, not only the self-initiated ones: telling the
  // host about a change it asked for is redundant, never wrong, and the
  // alternative is threading "who caused this" through the command queue for
  // no gain. The host must therefore treat these as notifications, not as
  // acknowledgements of its own calls.
  // The payload is documented as an mv_play_state, so the two enums have to
  // agree rung for rung — the cast below is the whole contract.
  static_assert(static_cast<int>(player::play_state::stopped) == MV_PLAY_STOPPED);
  static_assert(static_cast<int>(player::play_state::playing) == MV_PLAY_PLAYING);
  static_assert(static_cast<int>(player::play_state::paused) == MV_PLAY_PAUSED);
  static_assert(static_cast<int>(player::play_state::ended) == MV_PLAY_ENDED);
  const auto state = session->video.play_state_now();
  if (state != session->reported_video_state) {
    session->reported_video_state = state;
    mv_completion c{};
    c.kind = MV_COMPLETION_VIDEO_STATE;
    c.status = MV_OK;
    c.generation = session->jobs.current_generation();
    c.payload = static_cast<int64_t>(state);
    session->push_completion(c);
    if (state == player::play_state::ended) {
      mv_completion ended{};
      ended.kind = MV_COMPLETION_VIDEO_ENDED;
      ended.status = MV_OK;
      ended.generation = c.generation;
      session->push_completion(ended);
    }
  }
  return changed;
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


// Milestone G: add-on events ride the session's completion queue
// (abi/addon_abi.cpp, mediaviewer_addon.h).
void push_addon_completion(mv_session_t session, const mv_completion& c) noexcept {
  if (session) session->push_completion(c);
}

}  // namespace mv::abi
