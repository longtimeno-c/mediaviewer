// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/present_lab_mac.h"
#include "shell/present_busy.h"
#include "shell/video_report.h"
#include "shell/edit_view.h"
#include "shell/dino_draw.h"
#include "shell/welcome_screen.h"

#include "image/pipeline.h"
#include "shell/media_kind.h"
#include "image/colour.h"
#include "codec/decode.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_metal.h>

#include "core/crash_context.h"
#include "core/trace.h"
#include "gfx/pace_json.h"
#include "image/pipeline_mac.h"
#include "image/upload_mac.h"
#include "io/file.h"

using mv::gfx::k_input_tail_seconds;
using mv::gfx::k_occlusion_poll_ms;
using mv::gfx::k_warmup_seconds;

namespace {

double monotonic_seconds() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

double process_cpu_seconds() noexcept {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) * 1e-6 +
         static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) * 1e-6;
}

// PR 18: the height of the canvas rect between the SwiftUI command bar and
// the filmstrip strip. The swapchain itself still spans the full backing
// size (main_mac.mm's MvMetalView is never resized) — only fit/pan and the
// blit's origin_y see this, same as Windows' chrome_height_px/chrome_bottom_px
// (gfx/blit.h). chrome_bottom_px is 0 when the filmstrip is hidden (`T`),
// same "canvas reclaims the space" behaviour Windows' filmstrip toggle has.
float usable_window_h(const mv::shell::input_snapshot& s) noexcept {
  return std::max(1.0f, static_cast<float>(s.height) - static_cast<float>(s.chrome_height_px) -
                            static_cast<float>(s.chrome_bottom_px));
}

// PR 29 (plan/20): the same for width. A docked pane (chrome_right_px, the
// Edit workspace) narrows the rect the picture is framed in; the swapchain
// still spans the window and the blit's origin_x moves the frame, so docking
// is a refit, never a resize.
float usable_window_w(const mv::shell::input_snapshot& s) noexcept {
  return std::max(1.0f, static_cast<float>(s.width) - static_cast<float>(s.chrome_left_px) -
                            static_cast<float>(s.chrome_right_px));
}
float canvas_origin_x(const mv::shell::input_snapshot& s) noexcept {
  return static_cast<float>(s.chrome_left_px);
}

void feed_imgui(const mv::shell::input_snapshot& s, float delta_seconds, float wheel) noexcept {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(s.width), static_cast<float>(s.height));
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  io.DeltaTime = delta_seconds > 0.0f ? delta_seconds : 1.0f / 60.0f;
  if (s.mouse_in_client) {
    io.AddMousePosEvent(s.mouse_x, s.mouse_y);
  } else {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  for (int i = 0; i < 3; ++i) io.AddMouseButtonEvent(i, s.mouse_down[i]);
  if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
}

void try_load_font() noexcept {
  char exe[PATH_MAX]{};
  std::uint32_t size = sizeof(exe);
  if (_NSGetExecutablePath(exe, &size) != 0) return;
  char resolved[PATH_MAX]{};
  if (!realpath(exe, resolved)) return;
  char* slash = std::strrchr(resolved, '/');
  if (!slash) return;
  *slash = '\0';
  // MediaViewer.app keeps the font in Contents/Resources (codesign refuses
  // data files in Contents/MacOS); the bare lab keeps it beside the binary.
  for (const char* rel : {"/../Resources/CozetteVector.ttf", "/CozetteVector.ttf"}) {
    char font_path[PATH_MAX]{};
    if (std::snprintf(font_path, sizeof(font_path), "%s%s", resolved, rel) <= 0) continue;
    if (::access(font_path, R_OK) != 0) continue;
    if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 16.0f)) {
      ImGui::GetIO().FontDefault = font;
    }
    return;
  }
}

}  // namespace

@interface MvMetalLinkTarget : NSObject <CAMetalDisplayLinkDelegate> {
  std::atomic<void*>* _slot;
  std::condition_variable* _cv;
}
- (instancetype)initWithSlot:(std::atomic<void*>*)slot cv:(std::condition_variable*)cv;
@end

// The display-link callback is the waitable object: it fires when a drawable
// is ready, and only then does the lab encode. Sleep is not used.
@implementation MvMetalLinkTarget
- (instancetype)initWithSlot:(std::atomic<void*>*)slot cv:(std::condition_variable*)cv {
  self = [super init];
  if (self) {
    _slot = slot;
    _cv = cv;
  }
  return self;
}
- (void)metalDisplayLink:(CAMetalDisplayLink*)link
             needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  (void)link;
  void* previous = _slot->exchange((__bridge_retained void*)update);
  if (previous) (void)(__bridge_transfer CAMetalDisplayLinkUpdate*)previous;
  _cv->notify_one();
}
@end

namespace mv::shell {

namespace {
std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
std::atomic<void*> g_pending_update{nullptr};

CAMetalDisplayLinkUpdate* take_link_update() noexcept {
  void* p = g_pending_update.exchange(nullptr);
  return p ? (__bridge_transfer CAMetalDisplayLinkUpdate*)p : nil;
}
}  // namespace

present_lab_mac::~present_lab_mac() { stop(); }

namespace {

// The worker's own look at the file, for the cache key (never on the UI thread).
bool stat_stamp(const std::string& path, std::int64_t* mtime, std::uint64_t* size) noexcept {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return false;
  *mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
  *size = static_cast<std::uint64_t>(st.st_size);
  return true;
}

// GIF, WebP and PNG may be animated: their first frame is not the whole item,
// and the animation feed wants the file's bytes. They always load.
bool cacheable_still(const std::string& path) noexcept {
  if (is_video_name(path)) return false;
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return ext != "gif" && ext != "webp" && ext != "png" && ext != "apng";
}

// A sixteenth of memory, 256 MB to 1 GB: the item on screen and about four
// 24 MP neighbours on a 16 GB Mac. Metal textures are unified memory.
std::uint64_t still_cache_budget() noexcept {
  std::uint64_t mem = 0;
  std::size_t len = sizeof mem;
  if (::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) != 0 || mem == 0) mem = 8ull << 30;
  return std::clamp<std::uint64_t>(mem / 16, 256ull << 20, 1ull << 30);
}

constexpr std::size_t kStillCacheMax = 8;

}  // namespace

std::uint64_t present_lab_mac::mark_navigation() noexcept {
  const std::uint64_t seq = nav_seq_.load(std::memory_order_relaxed) + 1;
  if (seq > 64) return 0;
  nav_mark_seconds_.store(monotonic_seconds(), std::memory_order_relaxed);
  nav_seq_.store(seq, std::memory_order_release);
  return seq;
}

bool present_lab_mac::navigation_done(std::uint64_t seq) const noexcept {
  if (seq == 0 || seq > 64) return false;
  return nav_done_seq_.load(std::memory_order_acquire) >= seq && nav_samples_[seq - 1].valid != 0;
}

present_lab_mac::nav_sample present_lab_mac::navigation_sample(std::uint64_t seq) const noexcept {
  if (seq == 0 || seq > 64 || nav_done_seq_.load(std::memory_order_acquire) < seq) return {};
  return nav_samples_[seq - 1];
}

void present_lab_mac::note_nav_image(const image::gpu_image_mac& ready) noexcept {
  const std::uint64_t seq = nav_seq_.load(std::memory_order_acquire);
  if (seq == 0 || seq > 64 || nav_latched_seq_ == seq) return;
  nav_latched_seq_ = seq;
  nav_latch_seconds_ = nav_mark_seconds_.load(std::memory_order_relaxed);
  nav_ready_ms_ = (monotonic_seconds() - nav_latch_seconds_) * 1000.0;
  nav_cached_ = ready.preview ? 0 : 1;
  nav_item_ = ready.item_id;
  nav_have_ready_ = true;
}

void present_lab_mac::commit_nav_present() noexcept {
  if (!nav_have_ready_ || nav_latched_seq_ == 0 || nav_latched_seq_ > 64) return;
  if (!current_image_ || current_image_->item_id != nav_item_) return;
  nav_sample& sample = nav_samples_[nav_latched_seq_ - 1];
  sample.ready_ms = nav_ready_ms_;
  sample.present_ms = (monotonic_seconds() - nav_latch_seconds_) * 1000.0;
  sample.refresh_ms = layer_.refresh_interval_seconds() * 1000.0;
  sample.cached = nav_cached_;
  sample.valid = 1;
  nav_have_ready_ = false;
  nav_done_seq_.store(nav_latched_seq_, std::memory_order_release);
}

bool present_lab_mac::cache_has(const std::string& path) const noexcept {
  std::lock_guard lock(still_cache_mutex_);
  for (const auto& c : still_cache_) {
    if (c.path == path) return true;
  }
  return false;
}

bool present_lab_mac::cache_publish(const std::string& path, std::int64_t mtime,
                                    std::uint64_t size, std::uint64_t item_id) noexcept {
  if (mtime == kNoStamp) return false;
  image::gpu_image_mac* hit = nullptr;
  {
    std::lock_guard lock(still_cache_mutex_);
    for (std::size_t i = 0; i < still_cache_.size(); ++i) {
      cached_still& c = still_cache_[i];
      if (c.path != path) continue;
      if (c.mtime != mtime || c.size != size) return false;  // changed on disk: reload
      hit = new (std::nothrow) image::gpu_image_mac(c.image.share());
      std::rotate(still_cache_.begin() + static_cast<std::ptrdiff_t>(i),
                  still_cache_.begin() + static_cast<std::ptrdiff_t>(i) + 1, still_cache_.end());
      break;
    }
  }
  if (!hit) return false;
  hit->item_id = item_id;
  hit->preview = false;
  delete pending_image_.exchange(hit);
  wake();
  return true;
}

void present_lab_mac::cache_put(const std::string& path, std::int64_t mtime, std::uint64_t size,
                                const image::gpu_image_mac& image) noexcept {
  if (!image.valid() || image.preview) return;
  const std::uint64_t budget = still_cache_budget();
  const std::uint64_t bytes = image.approx_bytes();
  if (bytes > budget / 2) return;  // one huge still would evict everything else
  try {
    std::lock_guard lock(still_cache_mutex_);
    for (auto it = still_cache_.begin(); it != still_cache_.end(); ++it) {
      if (it->path != path) continue;
      still_cache_bytes_ -= it->image.approx_bytes();
      still_cache_.erase(it);
      break;
    }
    while (!still_cache_.empty() &&
           (still_cache_.size() >= kStillCacheMax || still_cache_bytes_ + bytes > budget)) {
      still_cache_bytes_ -= still_cache_.front().image.approx_bytes();
      still_cache_.erase(still_cache_.begin());
    }
    still_cache_.push_back(cached_still{path, mtime, size, image.share()});
    still_cache_bytes_ += bytes;
  } catch (...) {
    // No room for the entry: the still is simply not cached.
  }
}

void present_lab_mac::prefetch(const std::vector<std::string>& paths_utf8) noexcept {
  const std::uint64_t current = item_counter_.load(std::memory_order_acquire);
  {
    std::lock_guard lock(prefetch_mutex_);
    if (loading_item_.load(std::memory_order_acquire) == current) {
      try {
        prefetch_parked_ = paths_utf8;
        prefetch_parked_for_ = current;
      } catch (...) {
      }
      return;
    }
  }
  submit_prefetch(paths_utf8);
}

void present_lab_mac::release_prefetch(std::uint64_t item_id) noexcept {
  loading_item_.compare_exchange_strong(item_id, 0, std::memory_order_acq_rel);
  std::vector<std::string> parked;
  {
    std::lock_guard lock(prefetch_mutex_);
    if (prefetch_parked_for_ != item_id) return;
    parked.swap(prefetch_parked_);
    prefetch_parked_for_ = 0;
  }
  submit_prefetch(parked);
}

void present_lab_mac::submit_prefetch(const std::vector<std::string>& paths_utf8) noexcept {
  if (!options_.jobs) return;
  void* mtl_device = device_.native_device();
  for (const std::string& path : paths_utf8) {
    if (!cacheable_still(path) || cache_has(path)) continue;
    // Current generation: queued behind the open that bumped it (FIFO), and
    // abandoned by the next navigation, as Windows' neighbour decodes are.
    options_.jobs->submit([this, path, mtl_device](const job_context& ctx) -> status {
      std::int64_t mtime = 0;
      std::uint64_t size = 0;
      if (!stat_stamp(path, &mtime, &size)) return status::io;
      auto bytes = io::read_all(path);
      if (!bytes) return bytes.error();
      if (ctx.cancelled()) return status::cancelled;
      // One LibRaw thread: the item on screen keeps the cores (Windows does the same).
      auto decoded = image::decode_bytes_mac(bytes.value(), &ctx, 1);
      if (!decoded) return decoded.error();
      if (ctx.cancelled()) return status::cancelled;
      auto uploaded = image::upload(mtl_device, decoded.value(), &ctx);
      if (!uploaded) return uploaded.error();
      cache_put(path, mtime, size, uploaded.value());
      return status::ok;
    });
  }
}

// [any-thread]. Runs on the job pool: reads and decodes a file, then uploads
// an immutable MTLTexture. Never on the render thread (rule 1, CLAUDE.md /
// plan/02) -- device_.native_device() is safe to use from any thread. The
// file read is a plain blocking std::ifstream on the worker rather than
// io/file.h's async path: this is one folder-navigation stop, not the
// directory scan itself (folder_model_mac already uses io/file.h for that).
//
// Called only from open_item(), which has already bumped job_system's view
// generation, so `ctx` carries that new generation. If a newer open_item()
// call bumps it again before this finishes -- the user arrowed past this
// item before it loaded -- ctx.cancelled() catches it below and the result
// is discarded instead of clobbering the newer selection.
void present_lab_mac::submit_image_load(std::string path_utf8, std::uint64_t item_id) noexcept {
  if (path_utf8.empty() || !options_.jobs) return;

  void* mtl_device = device_.native_device();
  std::atomic<image::gpu_image_mac*>* pending = &pending_image_;
  // PR 11 (plan/13): the decode carries the id of the call that opened it, so a
  // crash in it names that call in its mv_decode_N slot (Windows: the ABI).
  const std::uint64_t cid = *crash_context::last_call_address();

  options_.jobs->submit(
      [this, path = std::move(path_utf8), mtl_device, pending, item_id,
       cid](const job_context& ctx) -> status {
        const crash_context::correlation_scope correlation(cid);
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return status::io;
        // One sized read: a byte-at-a-time istreambuf_iterator copy of a
        // 40 MB RAW cost over a second before decoding even started.
        const std::streamoff size = f.tellg();
        if (size <= 0) return status::io;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return status::io;

        // Rule 3: first pixel is never the full decode. A JPEG's DCT 1/4, a
        // RAW's embedded JPEG or a HEIC's thumbnail item goes up first; the full decode then replaces it
        // as a refinement of the same item (item_id), keeping the view.
        const double t_start = monotonic_seconds();
        if (auto preview = image::decode_first_pixel(bytes, &ctx)) {
          if (ctx.cancelled()) return status::cancelled;
          if (auto up = image::upload(mtl_device, preview.value(), &ctx)) {
            auto* first = new image::gpu_image_mac(std::move(up).value());
            first->item_id = item_id;
            first->preview = true;
            delete pending->exchange(first);
            // The render thread parks when idle (0 % GPU on a still); a load
            // that outlasts the post-navigation activity window would
            // otherwise sit in `pending` until the next input.
            wake();
            MV_LOG_INFO("open: preview %ux%u ready in %.0f ms", preview.value().width,
                        preview.value().height, (monotonic_seconds() - t_start) * 1000.0);
          } else if (up.error() == status::cancelled) {
            return status::cancelled;
          } else {
            MV_LOG_WARN("open: preview upload failed (%s)", status_name(up.error()));
          }
        } else if (preview.error() == status::cancelled) {
          return status::cancelled;
        } else {
          // Expected for formats with no cheap first pixel (PNG, HEIC, ...).
        }

        auto decoded = image::decode_bytes_mac(bytes, &ctx);
        if (!decoded) {
          if (decoded.error() != status::cancelled)
            MV_LOG_WARN("open: decode failed (%s)", status_name(decoded.error()));
          return decoded.error();
        }
        auto uploaded = image::upload(mtl_device, decoded.value(), &ctx);
        if (!uploaded) return uploaded.error();
        MV_LOG_INFO("open: full %ux%u ready in %.0f ms", decoded.value().width,
                    decoded.value().height, (monotonic_seconds() - t_start) * 1000.0);

        if (ctx.cancelled()) return status::cancelled;

        if (cacheable_still(path)) {
          std::int64_t mtime = 0;
          std::uint64_t size = 0;
          if (stat_stamp(path, &mtime, &size)) cache_put(path, mtime, size, uploaded.value());
        }
        auto* img = new image::gpu_image_mac(std::move(uploaded).value());
        img->item_id = item_id;
        image::gpu_image_mac* old = pending->exchange(img);
        delete old;  // a load superseded before the render thread took it over
        wake();
        // The full image is up: its neighbours may have the cores now. Still
        // the current generation, so a navigation since abandons them.
        release_prefetch(item_id);

        // Animated GIF/APNG/WebP (plan/04, folded into PR 18): frame 0 is
        // already up via the still path above (rule 3); if the file turns out
        // to have more frames, start the feed for frame 1 onward. `bytes` is
        // free to move now -- nothing above referenced it after decode_bytes_mac.
        // A still (one-frame WebP, PNG without acTL) is refused by
        // open_animation; a one-frame GIF is found out by the feed itself.
        const auto family = codec::probe(bytes);
        if (this->anim_session_ &&
            (family == codec::format_family::gif || family == codec::format_family::webp ||
             family == codec::format_family::png)) {
          auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
          if (auto opened = codec::open_animation(std::move(shared))) {
            this->anim_session_->publish(std::move(opened).value(), ctx.gen());
            wake();
          }
        }
        return status::ok;
      });
}

// PR 19. open_media() blocks on I/O and probing, so it runs here on a worker;
// the render thread only ever sees the finished media_source. A clip the user
// navigated away from before it opened is closed here, never posted.
void present_lab_mac::submit_video_open(std::string path_utf8, std::uint64_t item_id,
                                        std::int64_t moment_ms) noexcept {
  if (path_utf8.empty() || !options_.jobs) return;
  void* mtl_device = device_.native_device();
  video_opening_.store(item_id, std::memory_order_release);
  wake();  // present at vblank while it loads (present_request::video_loading)

  options_.jobs->submit(
      [this, path = std::move(path_utf8), mtl_device, item_id, moment_ms](const job_context& ctx) -> status {
        const double t0 = monotonic_seconds();
        auto opened = player::open_media(path.c_str(), mtl_device);
        std::uint64_t expected = item_id;
        video_opening_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        if (!opened) {
          MV_LOG_WARN("open: video open failed (%s)", status_name(opened.error()));
          wake();
          return opened.error();
        }
        if (ctx.cancelled()) {
          player::close_media(opened.value());
          return status::cancelled;
        }
        MV_LOG_INFO("open: video ready in %.0f ms", (monotonic_seconds() - t0) * 1000.0);
        auto* pm = new pending_media{opened.value(), item_id, moment_ms};
        if (pending_media* old = pending_media_.exchange(pm)) {
          player::close_media(old->source);  // a clip superseded before it was shown
          delete old;
        }
        wake();
        return status::ok;
      });
}

// A lookup and a 512-px JPEG decode on a worker, at the view generation
// open_item() just set: navigating on abandons it like any open.
void present_lab_mac::submit_clip_placeholder(std::string path_utf8, std::int64_t mtime_unix,
                                              std::uint64_t size, std::int64_t moment_ms,
                                              std::uint64_t item_id) noexcept {
  if (!options_.jobs || !options_.clip_thumb) return;
  void* mtl_device = device_.native_device();
  options_.jobs->submit(
      [this, path = std::move(path_utf8), mtime_unix, size, moment_ms, item_id,
       mtl_device](const job_context& ctx) -> status {
        const std::string jpeg = options_.clip_thumb(path, mtime_unix, size, moment_ms);
        if (jpeg.empty()) return status::ok;  // nothing cached: the sought frame is first
        if (ctx.cancelled()) return status::cancelled;
        std::ifstream f(jpeg, std::ios::binary | std::ios::ate);
        if (!f) return status::ok;
        const std::streamoff bytes_size = f.tellg();
        if (bytes_size <= 0) return status::ok;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(bytes_size));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(bytes.data()), bytes_size)) return status::ok;
        auto decoded = image::decode_bytes_mac(bytes, &ctx);
        if (!decoded) return decoded.error() == status::cancelled ? status::cancelled : status::ok;
        auto uploaded = image::upload(mtl_device, decoded.value(), &ctx);
        if (!uploaded) return uploaded.error() == status::cancelled ? status::cancelled : status::ok;
        if (ctx.cancelled()) return status::cancelled;
        auto* img = new image::gpu_image_mac(std::move(uploaded).value());
        img->item_id = item_id;
        img->preview = true;
        delete pending_image_.exchange(img);
        wake();
        return status::ok;
      });
}

void present_lab_mac::update_video_status() noexcept {
  if (!media_) {
    vs_active_.store(false, std::memory_order_release);
    return;
  }
  static constexpr int kRateX100[] = {25, 50, 100, 150, 200, 400};
  {
    const player::time_ns pos = media_->position_ns();
    vs_pos_ms_.store(pos / 1'000'000, std::memory_order_relaxed);
    vs_pos_ns_.store(pos, std::memory_order_relaxed);
  }
  vs_dur_ms_.store(media_->info().duration_ns / 1'000'000, std::memory_order_relaxed);
  vs_playing_.store(media_->state() == player::play_state::playing, std::memory_order_relaxed);
  vs_muted_.store(video_muted_, std::memory_order_relaxed);
  vs_rate_x100_.store(kRateX100[std::clamp(speed_rung_, 0, 5)], std::memory_order_relaxed);
  vs_volume_.store(video_volume_, std::memory_order_relaxed);
  vs_item_.store(media_item_, std::memory_order_relaxed);
  vs_active_.store(true, std::memory_order_release);
}

bool present_lab_mac::picture_size(float* w, float* h) const noexcept {
  if (video_frame_) {
    // The picture as displayed: a clip with a 90 / 270 degree matrix is
    // coded landscape and shown portrait (video_source.h rotation).
    const bool turned = (video_frame_->rotation & 1u) != 0;
    *w = static_cast<float>(turned ? video_frame_->height : video_frame_->width);
    *h = static_cast<float>(turned ? video_frame_->width : video_frame_->height);
    return true;
  }
  if (current_image_) {
    // PR 10: the camera frames the *edited* picture (a quarter turn swaps it,
    // a crop shrinks it).
    const edit::placement p = place_image(*current_image_);
    *w = static_cast<float>(p.cropped.w);
    *h = static_cast<float>(p.cropped.h);
    return true;
  }
  return false;
}

const edit_view* present_lab_mac::edit_for(std::uint64_t item) const noexcept {
  return match_edit(edit_slots_, item, 0);
}

edit::placement present_lab_mac::place_image(const image::gpu_image_mac& img) const noexcept {
  return place_through(edit_for(img.item_id), img.width, img.height);
}

// Crop mode (plan/16): the frame outside the draft rect is dimmed, the rect
// has a border and thirds. ImGui draws in the same present as the picture.
void present_lab_mac::draw_crop_overlay(const input_snapshot& snapshot) noexcept {
  if (!current_image_ || video_frame_) return;
  const edit_view* v = edit_for(current_image_->item_id);
  if (!v || !v->crop_overlay) return;
  float pw = 0.0f, ph = 0.0f;
  if (!picture_size(&pw, &ph)) return;
  const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;
  const float win_w = usable_window_w(snapshot);
  const float win_h = usable_window_h(snapshot);
  const float origin_x = canvas_origin_x(snapshot);
  const float origin_y = static_cast<float>(snapshot.chrome_height_px);
  const float zoom = camera_.zoom();
  const auto to_screen = [&](float ix, float iy) {
    return ImVec2(origin_x + win_w * 0.5f + (ix - camera_.pan_x()) * zoom,
                  origin_y + win_h * 0.5f + (iy - camera_.pan_y()) * zoom);
  };
  const ImVec2 f0 = to_screen(0.0f, 0.0f);
  const ImVec2 f1 = to_screen(pw, ph);
  const ImVec2 a = to_screen(v->overlay[0] * pw, v->overlay[1] * ph);
  const ImVec2 b = to_screen((v->overlay[0] + v->overlay[2]) * pw, (v->overlay[1] + v->overlay[3]) * ph);
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  const ImU32 shade = IM_COL32(0, 0, 0, 140);
  fg->AddRectFilled(f0, ImVec2(f1.x, a.y), shade);
  fg->AddRectFilled(ImVec2(f0.x, b.y), f1, shade);
  fg->AddRectFilled(ImVec2(f0.x, a.y), ImVec2(a.x, b.y), shade);
  fg->AddRectFilled(ImVec2(b.x, a.y), ImVec2(f1.x, b.y), shade);
  const ImU32 line = IM_COL32(255, 255, 255, 110);
  for (int i = 1; i < 3; ++i) {
    const float x = a.x + (b.x - a.x) * static_cast<float>(i) / 3.0f;
    const float y = a.y + (b.y - a.y) * static_cast<float>(i) / 3.0f;
    fg->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), line, scale);
    fg->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), line, scale);
  }
  fg->AddRect(a, b, IM_COL32(255, 255, 255, 235), 0.0f, 0, 1.5f * scale);
  char label[96];
  std::snprintf(label, sizeof(label), "%u x %u   %+.1f\xC2\xB0   Enter apply   Esc cancel",
                static_cast<unsigned>(std::lround(v->overlay[2] * pw)),
                static_cast<unsigned>(std::lround(v->overlay[3] * ph)),
                static_cast<double>(v->straighten));
  const float fs = 16.0f * scale;
  fg->AddText(ImGui::GetFont(), fs, ImVec2(a.x + scale, a.y - fs - 4.0f * scale + scale),
              IM_COL32(0, 0, 0, 200), label);
  fg->AddText(ImGui::GetFont(), fs, ImVec2(a.x, a.y - fs - 4.0f * scale),
              IM_COL32(235, 235, 240, 255), label);
}

void present_lab_mac::retire_media() noexcept {
  player::media_source* m = media_;
  if (m) {
    for (auto*& f : retired_frames_) {
      if (f) m->release_frame(f);
      f = nullptr;
    }
    if (video_frame_) m->release_frame(video_frame_);
  }
  video_frame_ = nullptr;
  media_ = nullptr;
  media_item_ = 0;
  media_fitted_ = false;
  if (!m) return;
  // close_media() stops and joins the demux/decode/audio threads: never on the
  // render thread (rule 1). The shared_ptr's deleter is what guarantees the
  // close runs exactly once even if the job is dropped by a shutdown.
  auto holder = std::shared_ptr<player::media_source>(
      m, [](player::media_source* p) { player::close_media(p); });
  if (!options_.jobs ||
      options_.jobs->submit_at(background_generation,
                               [holder](const job_context&) -> status { return status::ok; }) ==
          invalid_job) {
    holder.reset();  // pool gone (shutting down): close here, we are exiting
  }
}

// Latched transport from the UI thread (plan/16 "Video"). Space/,/. are
// shared between a real clip and an animated still (key_router.cpp maps both
// item_kind::clip and item_kind::animation to mode::video on Windows for the
// same reason); everything else here only ever applies to a clip. Returns
// true when it changed what should be on screen, so the caller redraws.
bool present_lab_mac::apply_playback_input(const input_snapshot& s) noexcept {
  static constexpr double kLadder[] = {0.25, 0.5, 1.0, 1.5, 2.0, 4.0};
  constexpr int kRungs = 6;

  // Navigation retires the previous item's animation feed exactly the way it
  // already retires a superseded still/clip load: compare against the live
  // job_system generation, same signal open_item()'s bump_generation() drives
  // everywhere else (plan/02).
  const std::uint32_t live_gen =
      options_.jobs ? options_.jobs->current_generation() : anim_generation_;
  if (live_gen != anim_generation_) {
    if (anim_session_) anim_session_->retire(live_gen);
    anim_generation_ = live_gen;
    anim_frame_.reset();
    anim_schedule_.reset();
    anim_finished_ = false;
    anim_seeking_ = false;
  }
  const bool anim_open = current_image_ && !video_frame_ && anim_session_ &&
                         anim_session_->open(anim_generation_);
  const auto now_ms = static_cast<std::uint64_t>(monotonic_seconds() * 1000.0);
  // A frame more than two refreshes late restarts the cadence (and counts as
  // late), the same slack the D3D11 lab gives itself.
  const auto slack_ms =
      static_cast<std::uint32_t>(layer_.refresh_interval_seconds() * 2000.0) + 1;

  bool changed = false;
  if (s.anim_toggle_seq != seen_anim_toggle_) {
    seen_anim_toggle_ = s.anim_toggle_seq;
    if (anim_open) {
      if (anim_finished_) {
        // Space on a played-out animation plays it again from the start.
        anim_session_->seek(0);
        anim_schedule_.reset();
        anim_finished_ = false;
      } else if (anim_schedule_.paused()) {
        anim_schedule_.resume(now_ms);
      } else {
        anim_schedule_.pause(now_ms);
      }
      changed = true;
    } else if (media_) {
      if (media_->state() == player::play_state::playing) media_->pause();
      else media_->play();
      hold_.user_transport();
      changed = true;
    }
  }
  if (s.anim_steps != seen_anim_steps_) {
    const std::int64_t d = s.anim_steps - seen_anim_steps_;
    seen_anim_steps_ = s.anim_steps;
    if (anim_open) {
      anim_schedule_.pause(now_ms);
      if (d < 0) {
        anim_session_->seek(anim_index_ > 0 ? anim_index_ - 1 : 0);
      } else if (anim_finished_) {
        anim_session_->seek(0);
      }
      anim_finished_ = false;
      anim_seeking_ = true;
      changed = true;
    } else if (media_) {
      const int n = static_cast<int>(std::min<std::int64_t>(std::llabs(d), 8));
      for (int i = 0; i < n; ++i) media_->step(d > 0 ? 1 : -1);
      changed = true;
    }
  }

  // A ready frame, if one is due. Checked every tick regardless of whether
  // toggle/step just fired, the same as the D3D11 lab: cadence is timer-driven,
  // not input-driven.
  if (anim_open && (anim_seeking_ || anim_schedule_.due(now_ms))) {
    mv::abi::animation_frame<image::gpu_image_mac> frame;
    if (anim_session_->take(anim_generation_, frame)) {
      anim_frame_.reset(frame.texture);
      anim_index_ = frame.index;
      if (anim_seeking_) {
        anim_schedule_.stepped(frame.delay_ms);
        anim_seeking_ = false;
      } else {
        anim_schedule_.shown(frame.delay_ms, now_ms, slack_ms);
      }
      changed = true;
    }
  }
  if (anim_open && !anim_seeking_) anim_finished_ = anim_session_->finished(anim_generation_);
  // Presents while it plays (or while a step is on its way); paused or played
  // out, the canvas idles like any still (plan/03 rule 4).
  anim_live_ = anim_open && (anim_seeking_ || (!anim_schedule_.paused() && !anim_finished_));
  anim_active_.store(anim_open, std::memory_order_release);

  if (!media_) {
    // No clip open: consume the video-only edges anyway, so a key pressed
    // before one opens is not replayed onto it later.
    seen_video_skip_ = s.video_skip_ms;
    seen_video_speed_ = s.video_speed_steps;
    seen_video_mute_ = s.video_mute_seq;
    seen_video_volume_ = s.video_volume_steps;
    seen_video_volume_set_ = s.video_volume_set_seq;
    seen_video_seek_ = s.video_seek_seq;
    seen_video_loop_ = s.video_loop_seq;
    return changed;
  }
  if (s.video_skip_ms != seen_video_skip_) {
    const std::int64_t d = s.video_skip_ms - seen_video_skip_;
    seen_video_skip_ = s.video_skip_ms;
    const player::time_ns duration = media_->info().duration_ns;
    player::time_ns target = media_->position_ns() + d * 1'000'000;
    target = std::max<player::time_ns>(0, duration > 0 ? std::min(target, duration - 1) : target);
    media_->seek(target, /*exact=*/true);
    changed = true;
  }
  if (s.video_speed_steps != seen_video_speed_) {
    const int d = s.video_speed_steps - seen_video_speed_;
    seen_video_speed_ = s.video_speed_steps;
    speed_rung_ = std::clamp(speed_rung_ + d, 0, kRungs - 1);
    media_->set_rate(kLadder[speed_rung_]);
    changed = true;
  }
  if (s.video_seek_seq != seen_video_seek_) {
    seen_video_seek_ = s.video_seek_seq;
    const player::time_ns duration = media_->info().duration_ns;
    player::time_ns target = s.video_seek_ns >= 0 ? s.video_seek_ns : s.video_seek_ms * 1'000'000;
    target = std::max<player::time_ns>(0, duration > 0 ? std::min(target, duration - 1) : target);
    media_->seek(target, s.video_seek_exact);
    changed = true;
  }
  // PR 13: trim's A-B preview (b < 0 clears), the Windows mv_video_set_loop twin.
  if (s.video_loop_seq != seen_video_loop_) {
    seen_video_loop_ = s.video_loop_seq;
    media_->set_loop(s.video_loop_a_ns, s.video_loop_b_ns);
    changed = true;
  }
  if (s.video_mute_seq != seen_video_mute_) {
    if ((s.video_mute_seq - seen_video_mute_) & 1u) {
      video_muted_ = !video_muted_;
      media_->set_muted(video_muted_);
    }
    seen_video_mute_ = s.video_mute_seq;
  }
  if (s.video_volume_steps != seen_video_volume_) {
    const int d = s.video_volume_steps - seen_video_volume_;
    seen_video_volume_ = s.video_volume_steps;
    video_volume_ = std::clamp(video_volume_ + 0.1f * static_cast<float>(d), 0.0f, 1.0f);
    media_->set_volume(video_volume_);
  }
  if (s.video_volume_set_seq != seen_video_volume_set_) {
    seen_video_volume_set_ = s.video_volume_set_seq;
    video_volume_ = std::clamp(s.video_volume_set_value, 0.0f, 1.0f);
    media_->set_volume(video_volume_);
  }
  return changed;
}

std::uint64_t present_lab_mac::open_item(std::string path_utf8, std::int64_t mtime_unix,
                                         std::uint64_t size, std::int64_t moment_ms) noexcept {
  if (path_utf8.empty() || !options_.jobs) return 0;
  // Abandons whatever the previous open_item() call had in flight (folder
  // navigation is a new view intent) without touching folder_model_mac's own
  // relist/thumb jobs, which stay pinned to background_generation and are
  // never cancelled by this.
  options_.jobs->bump_generation();
  const std::uint64_t item = ++item_counter_;
  if (is_video_name(path_utf8)) {
    clip_item_.store(item, std::memory_order_release);
    // Rule 3 for a clip landing on a moment: its cached thumbnail goes up
    // while the clip opens and decodes forward to the sought frame.
    if (moment_ms >= 0 && mtime_unix != kNoStamp && options_.clip_thumb) {
      submit_clip_placeholder(path_utf8, mtime_unix, size, moment_ms, item);
    }
    submit_video_open(std::move(path_utf8), item, moment_ms);
    return item;
  }
  clip_item_.store(0, std::memory_order_release);
  if (!cacheable_still(path_utf8) || !cache_publish(path_utf8, mtime_unix, size, item)) {
    loading_item_.store(item, std::memory_order_release);
    submit_image_load(std::move(path_utf8), item);
  }
  return item;
}

expected present_lab_mac::start(void* nsview, const mac_lab_options& options) noexcept {
  if (!nsview) return err(status::invalid_arg);
  view_ = nsview;
  options_ = options;
  overlay_visible_ = options.overlay_visible;
  animating_ = options.start_animating;
  sweep_mode_ = options.soak_seconds > 0.0;

  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this] { render_thread_main(); });
  while (running_.load(std::memory_order_acquire) &&
         start_error_.load(std::memory_order_acquire) == 0 &&
         !ready_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (start_error_.load(std::memory_order_acquire) != 0) return err(status::internal);
  return {};
}

void present_lab_mac::stop() noexcept {
  running_.store(false, std::memory_order_release);
  wake();
  if (render_thread_.joinable()) render_thread_.join();
}

void present_lab_mac::wake() noexcept {
  // Set the flag under the wait mutex: the idle wait checks it under that
  // mutex, so a store between its check and its block cannot be missed (the
  // idle timeout is an hour; a lost wake left a finished load off screen).
  {
    std::lock_guard lock(g_wait_mutex);
    wake_flag_.store(true, std::memory_order_release);
  }
  g_wait_cv.notify_all();
}

bool present_lab_mac::upload_working(const image::linear_image& img, std::uint64_t item) noexcept {
  if (item == 0 || !img.valid()) return false;
  void* mtl_device = device_.native_device();
  if (!mtl_device) return false;
  auto up = image::upload_linear(mtl_device, img);
  if (!up) return false;
  auto* made = new (std::nothrow) working_texture_mac{};
  if (!made) return false;
  made->image = std::move(up).value();
  made->item = item;
  delete pending_working_.exchange(made);
  wake();
  return true;
}

void present_lab_mac::drop_working() noexcept {
  delete pending_working_.exchange(nullptr);
  drop_working_.store(true, std::memory_order_release);
  wake();
}

bool present_lab_mac::take_working() noexcept {
  bool changed = false;
  if (drop_working_.exchange(false, std::memory_order_acq_rel)) {
    changed = working_ != nullptr;
    working_.reset();
  }
  if (working_texture_mac* w = pending_working_.exchange(nullptr)) {
    working_.reset(w);
    changed = true;
  }
  return changed;
}

// PR 9. Everything here reads `snapshot.meta` (published by the UI thread when
// the selection's metadata arrives) plus the camera; toggling an overlay is one
// more draw in the present that was already going out, never a file read.
void present_lab_mac::draw_photo_overlays(const input_snapshot& snapshot) noexcept {
  if (!snapshot.info_overlay && !snapshot.af_points && !snapshot.eyedropper) return;
  float pw = 0.0f, ph = 0.0f;
  if (!picture_size(&pw, &ph) || pw <= 0.0f || ph <= 0.0f) return;

  const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;
  const float win_w = usable_window_w(snapshot);
  const float win_h = usable_window_h(snapshot);
  const float origin_x = canvas_origin_x(snapshot);
  const float origin_y = static_cast<float>(snapshot.chrome_height_px);
  const float zoom = camera_.zoom();
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float fs = 16.0f * scale;
  const float pad = 12.0f * scale;
  const ImU32 text = IM_COL32(230, 230, 235, 255);
  const ImU32 shadow = IM_COL32(0, 0, 0, 200);
  const auto label = [&](float x, float y, const char* s) {
    fg->AddText(font, fs, ImVec2(x + scale, y + scale), shadow, s);
    fg->AddText(font, fs, ImVec2(x, y), text, s);
  };
  // Image pixel <-> screen pixel, the inverse of the blit's own mapping.
  const auto to_screen = [&](float ix, float iy) {
    return ImVec2(origin_x + win_w * 0.5f + (ix - camera_.pan_x()) * zoom,
                  origin_y + win_h * 0.5f + (iy - camera_.pan_y()) * zoom);
  };

  // A toggle that draws nothing looks broken, so say why there is nothing to see.
  float note_y = origin_y + pad;
  const auto note = [&](const char* text_line) {
    label(pad, note_y, text_line);
    note_y += fs * 1.35f;
  };
  // AF quads are in the unedited frame; with an edit they would point at the
  // wrong place, so they wait until the edit is reset (PR 10).
  const edit::placement edited = current_image_ && !video_frame_ ? place_image(*current_image_)
                                                                 : edit::placement{};
  if (snapshot.af_points && snapshot.meta.af_count == 0) {
    note("AF points: none recorded in this file");
  } else if (snapshot.af_points && !edited.map.identity()) {
    note("AF points: hidden while the image is edited");
  }
  if (snapshot.eyedropper) {
    if (video_frame_) note("Eyedropper: stills only");
    else if (!snapshot.mouse_in_client) note("Eyedropper: move the cursor over the image");
  }

  if (snapshot.af_points && edited.map.identity()) {
    for (int i = 0; i < snapshot.meta.af_count && i < meta_overlay::kMaxAf; ++i) {
      const float* q = snapshot.meta.af[i];
      const ImVec2 a = to_screen(q[0] * pw, q[1] * ph);
      const ImVec2 b = to_screen((q[0] + q[2]) * pw, (q[1] + q[3]) * ph);
      const ImU32 col = q[4] > 0.5f ? IM_COL32(80, 255, 120, 255) : IM_COL32(255, 210, 60, 255);
      fg->AddRect(ImVec2(a.x - scale, a.y - scale), ImVec2(b.x + scale, b.y + scale),
                  IM_COL32(0, 0, 0, 200), 0.0f, 0, 3.0f * scale);
      fg->AddRect(a, b, col, 0.0f, 0, 1.5f * scale);
    }
  }

  if (snapshot.info_overlay) {
    // Bottom-left, stacked upward: the item line, then whatever the property
    // model could fill. An empty field simply has no line (plan/06).
    const float line_h = fs * 1.35f;
    float y = origin_y + win_h - fs - pad;
    char line[400];
    const int zoom_pct = static_cast<int>(std::lround(zoom * 100.0f));
    if (snapshot.item_count > 0) {
      std::snprintf(line, sizeof(line), "%s  -  %u / %u  -  %ux%u  -  %d %%", snapshot.item_name,
                    snapshot.item_index + 1, snapshot.item_count, static_cast<unsigned>(pw),
                    static_cast<unsigned>(ph), zoom_pct);
    } else {
      std::snprintf(line, sizeof(line), "%ux%u  -  %d %%", static_cast<unsigned>(pw),
                    static_cast<unsigned>(ph), zoom_pct);
    }
    label(pad, y, line);
    for (const char* extra : {snapshot.meta.exposure_line, snapshot.meta.camera_line,
                              snapshot.meta.date_line}) {
      if (!extra[0]) continue;
      y -= line_h;
      label(pad, y, extra);
    }
  }

  std::string copy_text;  // what Cmd+C puts on the clipboard; empty = nothing under the cursor
  if (snapshot.eyedropper && snapshot.mouse_in_client) {
    // One texel, on demand: the source texture is CPU-visible (shared) so this
    // is a 4-byte read, never a download of the picture. Video frames are
    // planar YUV and are not sampled.
    const float ix = camera_.pan_x() + (snapshot.mouse_x - (origin_x + win_w * 0.5f)) / zoom;
    const float iy = camera_.pan_y() + (snapshot.mouse_y - (origin_y + win_h * 0.5f)) / zoom;
    if (current_image_ && !video_frame_ && ix >= 0.0f && iy >= 0.0f && ix < pw && iy < ph) {
      const void* native = anim_frame_ ? anim_frame_->texture : current_image_->texture;
      id<MTLTexture> tex = (__bridge id<MTLTexture>)native;
      const bool readable = tex && tex.storageMode != MTLStorageModePrivate &&
                            (tex.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ||
                             tex.pixelFormat == MTLPixelFormatRGBA8Unorm);
      if (readable) {
        // Through the edit map (PR 10): output pixel -> source uv -> texel.
        const float ou = ix / pw, ov = iy / ph;
        const float* em = edited.map.m;
        const float su = std::clamp(em[0] * ou + em[1] * ov + em[2], 0.0f, 1.0f);
        const float sv = std::clamp(em[3] * ou + em[4] * ov + em[5], 0.0f, 1.0f);
        const auto tx = static_cast<std::uint32_t>(
            std::min<float>(su * static_cast<float>(tex.width), static_cast<float>(tex.width - 1)));
        const auto ty = static_cast<std::uint32_t>(
            std::min<float>(sv * static_cast<float>(tex.height), static_cast<float>(tex.height - 1)));
        if (!eye_.valid || eye_.texture != native || eye_.x != tx || eye_.y != ty) {
          [tex getBytes:eye_.rgba
                 bytesPerRow:4
                  fromRegion:MTLRegionMake2D(tx, ty, 1, 1)
                 mipmapLevel:0];
          eye_.texture = native;
          eye_.x = tx;
          eye_.y = ty;
          eye_.valid = true;
        }
        char readout[96];
        std::snprintf(readout, sizeof(readout), "#%02X%02X%02X   %u %u %u   x%d y%d", eye_.rgba[0],
                      eye_.rgba[1], eye_.rgba[2], eye_.rgba[0], eye_.rgba[1], eye_.rgba[2],
                      static_cast<int>(ix), static_cast<int>(iy));
        char clip[96];
        std::snprintf(clip, sizeof(clip), "#%02X%02X%02X  rgb(%u, %u, %u)  x%d y%d", eye_.rgba[0],
                      eye_.rgba[1], eye_.rgba[2], eye_.rgba[0], eye_.rgba[1], eye_.rgba[2],
                      static_cast<int>(ix), static_cast<int>(iy));
        copy_text = clip;
        const ImVec2 size = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, readout);
        const float box = fs;
        float x = snapshot.mouse_x + 18.0f * scale;
        float y = snapshot.mouse_y + 18.0f * scale;
        if (x + box + 8.0f * scale + size.x + pad > origin_x + win_w) x = snapshot.mouse_x - (box + 8.0f * scale + size.x + 18.0f * scale);
        if (y + fs + pad > origin_y + win_h) y = snapshot.mouse_y - (fs + 18.0f * scale);
        fg->AddRectFilled(ImVec2(x - 6.0f * scale, y - 4.0f * scale),
                          ImVec2(x + box + 8.0f * scale + size.x + 6.0f * scale, y + fs + 4.0f * scale),
                          IM_COL32(0, 0, 0, 190), 4.0f * scale);
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + box, y + fs),
                          IM_COL32(eye_.rgba[0], eye_.rgba[1], eye_.rgba[2], 255));
        fg->AddRect(ImVec2(x, y), ImVec2(x + box, y + fs), IM_COL32(255, 255, 255, 220));
        label(x + box + 8.0f * scale, y, readout);
      }
    }
  }
  if (snapshot.eyedropper) {
    std::lock_guard<std::mutex> lock(eye_mutex_);
    eye_text_ = std::move(copy_text);
  }
}

void present_lab_mac::render_thread_main() noexcept {
  pthread_setname_np("mv.render");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

  @autoreleasepool {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    try_load_font();

    if (auto built = device_.create(); !built) {
      MV_LOG_ERROR("present_lab_mac: device creation failed (%s)", status_name(built.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }

    NSView* view = (__bridge NSView*)view_;
    const NSSize backing = [view convertSizeToBacking:view.bounds.size];
    const double scale = view.window.backingScaleFactor > 0.0 ? view.window.backingScaleFactor : 1.0;
    const auto bw = static_cast<std::uint32_t>(std::max(1.0, backing.width));
    const auto bh = static_cast<std::uint32_t>(std::max(1.0, backing.height));
    if (auto attached = layer_.attach(view_, device_, bw, bh, scale); !attached) {
      MV_LOG_ERROR("present_lab_mac: layer attach failed (%s)", status_name(attached.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      device_.destroy();
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }

    id<MTLDevice> mtl = (__bridge id<MTLDevice>)device_.native_device();
    ImGui_ImplMetal_Init(mtl);
    imgui_ready_ = true;

    // PR 17: the drawable's own pixel format, matching MvMetalView's
    // makeBackingLayer (main_mac.mm).
    if (auto built = blitter_.create(device_.native_device(),
                                     static_cast<std::uint64_t>(MTLPixelFormatBGRA8Unorm_sRGB));
        !built) {
      MV_LOG_ERROR("present_lab_mac: blitter create failed (%s)", status_name(built.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      ImGui_ImplMetal_Shutdown();
      layer_.destroy();
      device_.destroy();
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }
    if (auto built = video_blitter_.create(device_.native_device(),
                                           static_cast<std::uint64_t>(MTLPixelFormatBGRA8Unorm_sRGB));
        !built) {
      // Not fatal: stills still work; a clip then shows nothing rather than the app dying.
      MV_LOG_ERROR("present_lab_mac: video blitter create failed (%s)", status_name(built.error()));
    }

    // Animated GIF/APNG/WebP feed (plan/04). Each frame is colour managed like
    // a still and uploaded top level only (an animation is never mip-mapped),
    // the same recipe abi.cpp's Windows instantiation uses -- this is the same
    // template, just image::gpu_image_mac instead of image::gpu_image.
    {
      void* mtl_device = device_.native_device();
      anim_session_ = std::make_unique<mv::abi::animation_session<image::gpu_image_mac>>(
          [mtl_device](const codec::canvas_frame& frame, const codec::animation_info& info,
                      std::uint32_t generation) -> std::unique_ptr<image::gpu_image_mac> {
            (void)generation;
            codec::raster raster;
            raster.width = info.width;
            raster.height = info.height;
            raster.format = info.format;
            raster.intent = codec::transfer_intent::display_referred;
            raster.rgba = frame.rgba;
            raster.tagged_srgb = info.tagged_srgb;
            result<image::display_image> display = err(status::internal);
            if (info.icc.empty()) {
              display = image::to_display(std::move(raster));
            } else {
              const std::span<const std::uint8_t> icc(info.icc.data(), info.icc.size());
              auto transform = image::display_transform::create(icc);
              if (!transform) return nullptr;  // D6: a broken profile is not untagged sRGB
              display = transform.value()->apply(std::move(raster));
            }
            if (!display) return nullptr;
            auto uploaded = image::upload(mtl_device, display.value());
            if (!uploaded) return nullptr;
            return std::make_unique<image::gpu_image_mac>(std::move(uploaded).value());
          });
    }
    // No longer auto-loads options_.open_path here: MvLabApp now resolves
    // --open (and a bare argv path, and a drop) through folder_model_mac --
    // "a file opens its folder with that file selected" (plan/16-commands.md)
    // -- and calls open_item() itself once the async folder listing
    // resolves the selected index. See main_mac.mm's -openEntryPath:.
    ready_.store(true, std::memory_order_release);

    MvMetalLinkTarget* target =
        [[MvMetalLinkTarget alloc] initWithSlot:&g_pending_update cv:&g_wait_cv];
    CAMetalDisplayLink* link =
        [[CAMetalDisplayLink alloc] initWithMetalLayer:(__bridge CAMetalLayer*)layer_.native_layer()];
    const double refresh = layer_.refresh_interval_seconds();
    if (refresh > 0.0) {
      const float fps = static_cast<float>(1.0 / refresh);
      link.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
    }
    link.delegate = target;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    display_link_ = (__bridge_retained void*)link;
    link_target_ = (__bridge_retained void*)target;

    pacer_.begin_session(refresh);

    const double start = monotonic_seconds();
    double last_frame = start;

    while (running_.load(std::memory_order_acquire)) {
      @autoreleasepool {
        const input_snapshot snapshot = input_.acquire();
        edit_slots_[0] = snapshot.edit[0];
        edit_slots_[1] = snapshot.edit[1];
        const double elapsed = monotonic_seconds() - start;
        if (!warmed_up_ && elapsed >= k_warmup_seconds) {
          warmed_up_ = true;
          if (total_presents_ == 0 || !snapshot.window_visible) measurement_valid_ = false;
          pacer_.reset_window();
          measurement_start_seconds_ = monotonic_seconds();
          idle_start_cpu_seconds_ = process_cpu_seconds();
        }
        if (warmed_up_ && options_.soak_seconds > 0.0 &&
            monotonic_seconds() - measurement_start_seconds_ >= options_.soak_seconds) {
          soak_complete_ = true;
          break;
        }
        if (warmed_up_ && !snapshot.window_visible) measurement_valid_ = false;

        const bool input_activity = input_cursor_.consume_activity(snapshot);
        if (input_activity && warmed_up_ && !options_.start_animating) ++idle_stats_.input_events;
        bool redraw = input_activity;
        if (snapshot.background != seen_background_ ||
            snapshot.home_background_rgb != seen_home_background_rgb_) {
          seen_background_ = snapshot.background;
          seen_home_background_rgb_ = snapshot.home_background_rgb;
          redraw = true;
        }

        if (snapshot.toggle_overlay_seq != seen_overlay_seq_) {
          if ((snapshot.toggle_overlay_seq - seen_overlay_seq_) & 1u)
            overlay_visible_ = !overlay_visible_;
          seen_overlay_seq_ = snapshot.toggle_overlay_seq;
          redraw = true;
        }
        if (snapshot.game_exit_seq != seen_game_exit_seq_) {
          seen_game_exit_seq_ = snapshot.game_exit_seq;
          if (!sweep_mode_ && game_.active()) {
            // The outro presents until the welcome card is back; from game over
            // that restarts the loop, so the idle gap is not scored as a stall.
            if (game_.state() == dino_game::phase::over && options_.soak_seconds == 0.0)
              pacer_.reset_window();
            last_game_elapsed_ = 0.0;  // no catch-up step for the time spent dead
            game_.leave();
            animating_ = game_.state() == dino_game::phase::outro;
            redraw = true;
          }
        }
        if (snapshot.toggle_animation_seq != seen_animation_seq_) {
          const std::uint32_t presses = snapshot.toggle_animation_seq - seen_animation_seq_;
          if (sweep_mode_) {
            if (presses & 1u) animating_ = !animating_;
          } else {
            for (std::uint32_t i = 0; i < presses && i < 4u; ++i) game_.press();
            animating_ = game_.state() == dino_game::phase::intro ||
                         game_.state() == dino_game::phase::playing;
          }
          seen_animation_seq_ = snapshot.toggle_animation_seq;
          redraw = true;
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
          if (options_.soak_seconds == 0.0) pacer_.reset_window();
        }
        if (snapshot.game_view_seq != seen_game_view_seq_) {
          if (!sweep_mode_ && ((snapshot.game_view_seq - seen_game_view_seq_) & 1u)) game_.toggle_3d();
          seen_game_view_seq_ = snapshot.game_view_seq;
          redraw = true;
        }
        if (snapshot.reset_stats_seq != seen_reset_seq_) {
          seen_reset_seq_ = snapshot.reset_stats_seq;
          redraw = true;
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
          if (options_.soak_seconds == 0.0) pacer_.reset_window();
        }
        if (snapshot.display_change_seq != seen_display_seq_) {
          seen_display_seq_ = snapshot.display_change_seq;
          const double previous = layer_.refresh_interval_seconds();
          layer_.refresh_output_info();
          if (warmed_up_ && previous != layer_.refresh_interval_seconds())
            measurement_valid_ = false;
          pacer_.set_refresh(layer_.refresh_interval_seconds());
          redraw = true;
        }
        if (snapshot.resize_seq != seen_resize_seq_) {
          seen_resize_seq_ = snapshot.resize_seq;
          const double sc =
              snapshot.dpi_scale > 0.0f ? static_cast<double>(snapshot.dpi_scale) : 1.0;
          if (auto r = layer_.resize(snapshot.width, snapshot.height, sc); !r) {
            MV_LOG_WARN("present_lab_mac: resize failed (%s)", status_name(r.error()));
            measurement_valid_ = false;
          }
          pacer_.set_refresh(layer_.refresh_interval_seconds());
          if (float pw = 0, ph = 0; picture_size(&pw, &ph) && camera_.fit_mode()) {
            camera_.fit(pw, ph, usable_window_w(snapshot), usable_window_h(snapshot),
                        /*immediate=*/false);
          }
          redraw = true;
        }

        // PR 17: a background job finished decoding --open. Take it over and
        // fit it once; a resize while in fit mode re-fits below.
        image::gpu_image_mac* loaded = pending_image_.exchange(nullptr);
        // Milestone H: a clip's placeholder (submit_clip_placeholder) stands in
        // until the clip's first frame; one that lands after it is too late.
        const bool clip_placeholder = loaded && loaded->preview && loaded->item_id != 0 &&
                                      loaded->item_id == clip_item_.load(std::memory_order_acquire);
        if (clip_placeholder && media_ && media_item_ == loaded->item_id && video_frame_) {
          delete loaded;
          loaded = nullptr;
        }
        if (loaded) {
          // Same item, better pixels (preview -> full): keep the view as a
          // fraction of the image so nothing pops, refits or snaps. A new
          // item resets and fits.
          // A still for a newer item ends the clip on screen.
          if (media_ && media_item_ != loaded->item_id) retire_media();
          const bool refinement = current_image_ && loaded->item_id != 0 &&
                                  current_image_->item_id == loaded->item_id;
          // PR 10: sizes through each image's own edit geometry.
          float old_w = 0.0f, old_h = 0.0f;
          if (current_image_ && !video_frame_) (void)picture_size(&old_w, &old_h);
          if (refinement && current_image_->preview && !loaded->preview) {
            fade_.begin(elapsed, canvas::refine_fade_seconds(current_image_->mean_luma,
                                                            loaded->mean_luma));
            fade_from_ = std::move(current_image_);
          } else if (refinement && fade_.active(elapsed) &&
                     current_image_->width == loaded->width &&
                     current_image_->height == loaded->height) {
            // A second full-size publish (top level, then the mip chain) used
            // to cancel the fade and cut the preview out. Keep the fade that
            // is already running and swap the incoming texture under it.
          } else {
            fade_from_.reset();
            fade_.cancel();
          }
          if (!refinement) {
            note_nav_image(*loaded);
            first_pixel_seconds_ = elapsed;
            full_seconds_ = loaded->preview ? -1.0 : elapsed;
          } else if (!loaded->preview && full_seconds_ < 0.0) {
            full_seconds_ = elapsed;
          }
          current_image_.reset(loaded);
          {
            const edit_view* ev = edit_for(current_image_->item_id);
            applied_edit_ = ev ? *ev : edit_view{};
            // A clip's placeholder is not a still on screen: no crop bounds,
            // no browse-soak "still shown", no default-viewer prompt.
            if (!clip_placeholder) {
              shown_w_.store(current_image_->width, std::memory_order_relaxed);
              shown_h_.store(current_image_->height, std::memory_order_relaxed);
              shown_item_.store(current_image_->item_id, std::memory_order_release);
            }
          }
          const edit::placement landed = place_image(*current_image_);
          if (refinement) {
            camera_.refine(old_w, old_h, static_cast<float>(landed.cropped.w),
                           static_cast<float>(landed.cropped.h),
                           usable_window_w(snapshot), usable_window_h(snapshot));
          } else {
            if (!clip_placeholder) stills_shown_.fetch_add(1, std::memory_order_acq_rel);
            // plan/16 sticky zoom: off (default) fits every item; on keeps the
            // mode, or the zoom and pan fraction (same as the Windows lab).
            const auto new_w = static_cast<float>(landed.cropped.w);
            const auto new_h = static_cast<float>(landed.cropped.h);
            const auto win_w = usable_window_w(snapshot);
            const float win_h = usable_window_h(snapshot);
            const bool had_media = old_w > 0.0f;
            if (snapshot.sticky_zoom && had_media && camera_.fill_mode()) {
              camera_.fill(new_w, new_h, win_w, win_h, /*immediate=*/true);
            } else if (snapshot.sticky_zoom && had_media && !camera_.fit_mode()) {
              camera_.carry(old_w, old_h, new_w, new_h, win_w, win_h);
            } else {
              camera_.reset();
              camera_.fit(new_w, new_h, win_w, win_h, /*immediate=*/true);
            }
          }
          redraw = true;
          // A still popping in mid-measurement changes what's on screen just
          // like a resize/toggle/reset does -- same invalidation those
          // branches already apply.
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
        }

        // PR 11: a working texture handed over (or dropped) is one more frame.
        if (take_working()) redraw = true;
        // PR 10: the edit geometry of the still on screen changed (a turn, a
        // committed crop, crop mode's draft). A new picture size refits, as a
        // new item would; an overlay-only change just redraws — and so does a
        // colour change (PR 11): new uniforms, no decode, no refit.
        if (current_image_ && !video_frame_) {
          const edit_view* ev = edit_for(current_image_->item_id);
          const edit_view now = ev ? *ev : edit_view{};
          const bool geometry_changed = !same_geometry(now, applied_edit_);
          if (geometry_changed || !same_overlay(now, applied_edit_) ||
              !same_adjust(now, applied_edit_)) {
            float before_w = 0.0f, before_h = 0.0f;
            {
              const edit::geometry g = geometry_of(applied_edit_);
              const edit::placement p = edit::place(
                  applied_edit_.item == current_image_->item_id ? g : edit::geometry{},
                  edit::size2{current_image_->width, current_image_->height}, {},
                  applied_edit_.keep_frame);
              before_w = static_cast<float>(p.cropped.w);
              before_h = static_cast<float>(p.cropped.h);
            }
            applied_edit_ = now;
            float pw = 0.0f, ph = 0.0f;
            if (geometry_changed && picture_size(&pw, &ph) && (pw != before_w || ph != before_h)) {
              camera_.reset();
              camera_.fit(pw, ph, usable_window_w(snapshot), usable_window_h(snapshot),
                          /*immediate=*/true);
            }
            redraw = true;
            if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
          }
        }

        // Issue #44: the gallery opened or closed over the canvas.
        if (snapshot.video_hold != hold_.held()) {
          const std::uint64_t live = item_counter_.load(std::memory_order_acquire);
          const player::held_clip clip{media_ != nullptr, media_item_,
                                       media_ ? media_->state() : player::play_state::stopped};
          const auto action = snapshot.video_hold
                                  ? hold_.hold(live, clip)
                                  : hold_.release(live, clip, snapshot.video_hold_resume);
          if (action == player::hold_action::pause) media_->pause();
          if (action == player::hold_action::play) media_->play();
          if (action != player::hold_action::none) redraw = true;
        }

        // PR 19: a clip finished opening on a worker. The previous picture stays
        // up until the clip's first frame is ready.
        if (pending_media* pm = pending_media_.exchange(nullptr)) {
          retire_media();
          media_ = pm->source;
          media_item_ = pm->item;
          const std::int64_t moment_ms = pm->moment_ms;
          delete pm;
          speed_rung_ = 2;
          video_muted_ = false;
          media_->set_volume(video_volume_);
          if (moment_ms >= 0) {
            // Milestone H: a search result lands paused on its moment. The
            // exact seek decodes forward from the keyframe before it and
            // presents that one frame (media_source preview), then idles.
            // Opened under the gallery (G while it was still opening), the
            // hold took it for a clip about to autoplay and would start it on
            // release: it never was, so it stays paused as an explicit Pause
            // would leave it. Only for the live item; a stale adoption must
            // not cancel the resume of the one still opening.
            if (media_item_ == item_counter_.load(std::memory_order_acquire)) {
              hold_.user_transport();
            }
            media_->pause();
            media_->seek(static_cast<player::time_ns>(moment_ms) * 1'000'000, /*exact=*/true);
          } else if (hold_.autoplay(media_item_)) {
            // Under the gallery it stays paused on its first frame (issue #44).
            media_->play();
          }
          redraw = true;
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
        }
        if (apply_playback_input(snapshot)) redraw = true;
        update_video_status();
        const float wheel = input_cursor_.consume_wheel(snapshot);
        if (wheel != 0.0f) redraw = true;

        if (float image_w = 0, image_h = 0; picture_size(&image_w, &image_h)) {
          const auto window_w = usable_window_w(snapshot);
          // PR 18: the SwiftUI command bar covers the top chrome_height_px of
          // the canvas, the same "swapchain spans the client area, chrome is
          // composited over it" shape as blit.h's origin_x/origin_y on
          // Windows (gfx/blit.h) -- fit/pan only see the rect below it.
          const float window_h = usable_window_h(snapshot);

          if (snapshot.fit_seq != seen_fit_seq_) {
            seen_fit_seq_ = snapshot.fit_seq;
            camera_.fit(image_w, image_h, window_w, window_h, /*immediate=*/false);
            redraw = true;
          }
          if (snapshot.one_to_one_seq != seen_one_to_one_seq_) {
            seen_one_to_one_seq_ = snapshot.one_to_one_seq;
            camera_.one_to_one();
            redraw = true;
          }

          // A drag that started inside the view keeps tracking on
          // mouse_down[0] alone once under way, even past the view's edge
          // (panning toward an edge is the common case this covers) --
          // mouse_in_client only gates *starting* a new drag.
          if (snapshot.mouse_down[0] && (was_dragging_ || snapshot.mouse_in_client)) {
            if (!was_dragging_) {
              camera_.drag_begin();
              was_dragging_ = true;
            } else {
              camera_.drag_delta(snapshot.mouse_x - last_mouse_x_, snapshot.mouse_y - last_mouse_y_);
            }
          } else if (was_dragging_) {
            camera_.drag_end();
            was_dragging_ = false;
          }
          last_mouse_x_ = snapshot.mouse_x;
          last_mouse_y_ = snapshot.mouse_y;

          if (wheel != 0.0f && snapshot.mouse_in_client) {
            camera_.wheel_toward(snapshot.mouse_x - canvas_origin_x(snapshot), snapshot.mouse_y, wheel,
                                 window_w, window_h,
                                 image_w, image_h);
          }
        }
        if (redraw) last_input_time_ = elapsed;
        if (fade_from_ && !fade_.active(elapsed)) {
          fade_from_.reset();
          fade_.cancel();
          redraw = true; // final fully opaque frame, then return to idle
        }

        gfx::present_request req;
        req.window_visible = snapshot.window_visible;
        req.window_active = snapshot.window_active;
        req.occluded = occluded_;
        req.soak = options_.soak_seconds > 0.0 || options_.harness;
        req.animating = animating_;
        {
          float pw = 0, ph = 0;
          const bool has_picture = picture_size(&pw, &ph);
          req.camera_moving = has_picture && camera_.moving();
          req.has_still = has_picture;
        }
        req.video_active = (media_ && media_->needs_present()) || anim_live_;
        req.video_loading = video_opening_.load(std::memory_order_acquire) != 0;
        req.redraw = redraw || redraw_owed_ || fade_.active(elapsed);
        req.painted_static = painted_static_;
        req.elapsed_seconds = elapsed;
        req.last_input_time = last_input_time_;
        const auto decision = gfx::decide_present(req);
        if (const std::uint64_t drops = pacer_.dropped_frames_so_far(); drops > busy_drops_seen_) {
          busy_drops_seen_ = drops;
          busy_drop_at_ = elapsed;
        }
        const bool frame_pressure = elapsed - busy_drop_at_ < 2.0;
        mv::shell::g_present_busy.store(gfx::present_busy(decision, frame_pressure),
                                        std::memory_order_relaxed);

        CAMetalDisplayLink* live_link = (__bridge CAMetalDisplayLink*)display_link_;
        if (!decision.wants_frame) {
          // Presenting allowed, not live and not asked: nothing was owed. Not
          // allowed (the window not key): what changed is owed for when it is.
          redraw_owed_ = req.redraw;
          if (was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
          was_presenting_ = false;
          live_link.paused = YES;
          double timeout = occluded_ ? k_occlusion_poll_ms / 1000.0 : 3600.0;
          if (options_.soak_seconds > 0.0) {
            const double remaining = warmed_up_
                                         ? options_.soak_seconds -
                                               (monotonic_seconds() - measurement_start_seconds_)
                                         : k_warmup_seconds - elapsed;
            timeout = std::min(timeout, std::max(0.0, remaining));
          }
          std::unique_lock lock(g_wait_mutex);
          g_wait_cv.wait_for(lock, std::chrono::duration<double>(timeout), [&] {
            return !running_.load(std::memory_order_acquire) ||
                   wake_flag_.load(std::memory_order_acquire);
          });
          wake_flag_.store(false, std::memory_order_release);
          occluded_ = view.window.occlusionState & NSWindowOcclusionStateVisible ? false : true;
          // As on Windows: the idle time is not a spring step. Without this the
          // first frame after rest stepped the full 1/15 s cap and popped.
          last_frame = monotonic_seconds();
          continue;
        }

        if (!was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
        was_presenting_ = true;
        live_link.paused = NO;

        // Wait BEFORE encode. The display-link callback is the waitable object.
        // Pump this thread's run loop so the link, which is attached here, can
        // fire; the callback stores the update and wakes the condvar.
        CAMetalDisplayLinkUpdate* update = take_link_update();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!update && running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
          [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                   beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
          update = take_link_update();
        }
        if (!update) continue;

        // Choose the video frame AFTER the display-link wait, as Windows does
        // after its latency wait: chosen before it, the clock was read 0 to
        // one refresh early, which shows as cadence jitter on 24/30 fps clips.
        if (media_ && media_->needs_present()) {
          const auto vblank_ns =
              static_cast<player::time_ns>(layer_.refresh_interval_seconds() * 1e9);
          if (player::video_frame* frame = media_->acquire_frame(1, vblank_ns)) {
            // The frame we are replacing may still be read by a command buffer
            // in flight, and the decode thread reuses a released slot at once,
            // so it is held back kRetiredFrames presents before release.
            if (video_frame_) {
              if (retired_frames_[kRetiredFrames - 1]) {
                media_->release_frame(retired_frames_[kRetiredFrames - 1]);
              }
              for (std::size_t i = kRetiredFrames - 1; i > 0; --i) {
                retired_frames_[i] = retired_frames_[i - 1];
              }
              retired_frames_[0] = video_frame_;
            }
            video_frame_ = frame;
            if (!media_fitted_) {
              current_image_.reset();
              fade_from_.reset();
              fade_.cancel();
              camera_.reset();
              camera_.fit(static_cast<float>(frame->width), static_cast<float>(frame->height),
                          usable_window_w(snapshot), usable_window_h(snapshot),
                          /*immediate=*/true);
              media_fitted_ = true;
            }
            redraw = true;
          }
        }

        pacer_.frame_begin();
        const double now = monotonic_seconds();
        const float delta = static_cast<float>(now - last_frame);
        last_frame = now;
        if (float pw = 0, ph = 0; picture_size(&pw, &ph)) camera_.step(delta);

        id<CAMetalDrawable> drawable = update.drawable;
        if (!drawable) continue;
        const float theme_rgb[3] = {
            home_linear_channel(snapshot.home_background_rgb, 16),
            home_linear_channel(snapshot.home_background_rgb, 8),
            home_linear_channel(snapshot.home_background_rgb, 0)};

        gfx::display_link_tick tick;
        tick.valid = true;
        tick.target_seconds = update.targetTimestamp;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        // Settings' canvas background applies behind media. The empty welcome
        // and runner use AppKit's window colour from the UI-thread snapshot.
        {
          const bool have_picture = current_image_ != nullptr || video_frame_ != nullptr;
          const double lvl = gfx::background_clear_mac(snapshot.background);
          if (!snapshot.blackout &&
              ((!have_picture && !media_ && !(sweep_mode_ && animating_)) ||
               snapshot.background == 0)) {
            pass.colorAttachments[0].clearColor = MTLClearColorMake(
                theme_rgb[0], theme_rgb[1], theme_rgb[2], 1.0);
          } else {
            pass.colorAttachments[0].clearColor =
                snapshot.background == 4 ? MTLClearColorMake(0.016, 0.018, 0.024, 1.0)
                                               : MTLClearColorMake(lvl, lvl, lvl, 1.0);
          }
        }

        ImGui_ImplMetal_NewFrame(pass);
        feed_imgui(snapshot, delta, wheel);
        ImGui::NewFrame();

        // The lab sweep/idle text is the PR 16 instrument; once --open has
        // loaded a still, the image (drawn below, same render pass) replaces
        // it rather than drawing both.
        const bool have_picture = current_image_ != nullptr || video_frame_ != nullptr;
        if (have_picture && !sweep_mode_ && game_.active()) {  // a file opened over the runner
          game_.leave_now();
          animating_ = false;
        }
        if (!have_picture && sweep_mode_ && animating_) {
          const auto w = static_cast<float>(snapshot.width);
          const auto h = static_cast<float>(snapshot.height);
          animation_phase_ = std::fmod(elapsed * 0.35, 1.0);
          const float bar_width = 6.0f * (snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f);
          const float x = static_cast<float>(animation_phase_) * (w - bar_width);
          ImDrawList* bg = ImGui::GetBackgroundDrawList();
          bg->AddRectFilled(ImVec2(x, 0.0f), ImVec2(x + bar_width, h),
                            IM_COL32(230, 230, 235, 255));
        } else if (!have_picture) {
          const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;
          const float w = static_cast<float>(snapshot.width);
          const float h = static_cast<float>(snapshot.height);
          const float chrome = static_cast<float>(snapshot.chrome_height_px);
          const welcome_text text{
              .open_hint = "or press Cmd+O to choose a file or a folder",
              .keys = "0 fit    1 100%    + / -  zoom    Space  play/pause    F  fullscreen    ?  shortcuts"};
          const home_palette theme(snapshot.home_background_rgb);
          ImDrawList* bg = ImGui::GetBackgroundDrawList();
          if (!sweep_mode_ && game_.active()) {
            const float dt = last_game_elapsed_ > 0.0
                                 ? static_cast<float>(elapsed - last_game_elapsed_)
                                 : 0.0f;
            last_game_elapsed_ = elapsed;
            game_.set_view_width(w / (3.0f * scale));
            game_.update(dt);
            // Game over or the outro finished: idle again, nothing moves.
            if (game_.state() == dino_game::phase::over || !game_.active()) animating_ = false;
            draw_welcome(bg, ImGui::GetFont(), w, h, chrome, scale, text, theme, welcome_alpha(game_),
                         &snapshot.recents);
            draw_dino(bg, ImGui::GetFont(), game_, w, h, chrome, scale, theme);
          } else {
            last_game_elapsed_ = 0.0;
            draw_welcome(bg, ImGui::GetFont(), w, h, chrome, scale, text, theme, 1.0f, &snapshot.recents);
          }
        }

        if (overlay_visible_) {
          const auto stats = pacer_.stats();
          // Below the SwiftUI command bar, which covers the canvas's top
          // chrome_height_px (both are backing pixels); a fixed (12, 12)
          // hid the first line under it (found on real hardware, 2026-09-19).
          ImGui::SetNextWindowPos(ImVec2(12.0f, static_cast<float>(snapshot.chrome_height_px) + 12.0f),
                                  ImGuiCond_Always);
          ImGui::SetNextWindowBgAlpha(0.72f);
          ImGui::Begin("##f3", nullptr,
                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoMove);
          ImGui::Text("Metal present lab   F3 overlay   space sweep   R reset");
          ImGui::Text("refresh   %.3f ms", stats.refresh_interval_ms);
          ImGui::Text("p50/p99   %.3f / %.3f ms", stats.p50_ms, stats.p99_ms);
          ImGui::Text("dropped   %llu  missed %llu",
                      static_cast<unsigned long long>(stats.dropped_frames),
                      static_cast<unsigned long long>(stats.missed_refreshes));
          ImGui::Text("source    %s", gfx::metal_drop_source_label(stats.source));
          ImGui::Text("%s", animating_ ? "animating (lab sweep)" : "idle-capable");
          if (media_) {
            const auto ms = media_->stats();
            const auto mi = media_->info();
            const char* decoder = ms.decoder == player::decoder_kind::videotoolbox ? "VideoToolbox"
                                  : ms.decoder == player::decoder_kind::software   ? "SOFTWARE"
                                                                                   : "none";
            ImGui::Separator();
            ImGui::Text("video     %s  %ux%u %s  %s %.2fx", decoder, mi.video.width, mi.video.height,
                        mi.video.ten_bit ? "10-bit" : "8-bit", mi.video.codec_name,
                        ms.playback_rate);
            ImGui::Text("position  %.2f / %.2f s  %s", media_->position_ns() / 1e9,
                        mi.duration_ns / 1e9,
                        media_->state() == player::play_state::playing ? "playing"
                        : media_->state() == player::play_state::ended ? "ended" : "paused");
            ImGui::Text("clock     %s   err p50/p99 %.1f / %.1f ms   slope %.2f ms/min",
                        ms.audio_master ? "audio" : "HOST (no audio)", ms.err_ms_p50, ms.err_ms_p99,
                        ms.drift_slope_ms_per_min);
            ImGui::Text("frames    shown %llu  late %llu  starved %llu  silence %llu",
                        static_cast<unsigned long long>(ms.counters.presented),
                        static_cast<unsigned long long>(ms.counters.dropped_late),
                        static_cast<unsigned long long>(ms.counters.held_starved),
                        static_cast<unsigned long long>(ms.counters.silence_fills));
          }
          ImGui::End();
        }

        draw_photo_overlays(snapshot);
        draw_crop_overlay(snapshot);

        ImGui::Render();

        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device_.native_queue();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        if (video_frame_) {
          gfx::video_blit_params_mac vp;
          vp.pan_x = camera_.pan_x();
          vp.pan_y = camera_.pan_y();
          vp.zoom = camera_.zoom();
          vp.window_w = usable_window_w(snapshot);
          vp.window_h = usable_window_h(snapshot);
          vp.origin_x = canvas_origin_x(snapshot);
          vp.origin_y = static_cast<float>(snapshot.chrome_height_px);
          vp.image_w = static_cast<float>(video_frame_->width);
          vp.image_h = static_cast<float>(video_frame_->height);
          vp.rotation = video_frame_->rotation;
          id<MTLTexture> luma_tex = (__bridge id<MTLTexture>)video_frame_->luma;
          vp.texture_w = static_cast<float>(luma_tex.width);
          vp.texture_h = static_cast<float>(luma_tex.height);
          video_blitter_.draw((__bridge void*)enc, video_frame_->luma, video_frame_->chroma,
                              video_frame_->colour, vp);
        } else if (current_image_) {
          gfx::blit_params_mac bp;
          bp.pan_x = camera_.pan_x();
          bp.pan_y = camera_.pan_y();
          bp.zoom = camera_.zoom();
          bp.window_w = usable_window_w(snapshot);
          bp.window_h = usable_window_h(snapshot);
          bp.origin_x = canvas_origin_x(snapshot);
          bp.origin_y = static_cast<float>(snapshot.chrome_height_px);
          // PR 10: the edited picture is what the camera frames; the texture is
          // sampled through the output -> source map.
          const edit::placement pl = place_image(*current_image_);
          const edit_view* ev = edit_for(current_image_->item_id);
          bp.image_w = static_cast<float>(pl.cropped.w);
          bp.image_h = static_cast<float>(pl.cropped.h);
          bp.texture_w = static_cast<float>(current_image_->texture_width ? current_image_->texture_width
                                                                           : current_image_->width);
          bp.texture_h = static_cast<float>(current_image_->texture_height ? current_image_->texture_height
                                                                            : current_image_->height);
          for (int i = 0; i < 6; ++i) bp.uv_map[i] = pl.map.m[i];
          bp.clip_to_source = ev && ev->keep_frame;
          bp.background = snapshot.background;
          for (int i = 0; i < 3; ++i) bp.theme_background[i] = theme_rgb[i];
          bp.time_seconds = static_cast<float>(elapsed);
          // PR 11: the colour kernel's uniforms, and the FP16 working texture
          // in place of the 8-bit one once it has landed for this still; until
          // then the same kernel runs on the 8-bit texture.
          apply_adjust(ev, bp);
          const bool use_working = !anim_frame_ && working_ && ev && ev->item != 0 &&
                                   working_->item == ev->item && working_->image.texture;
          if (use_working) {
            bp.texture_w = static_cast<float>(working_->image.texture_width);
            bp.texture_h = static_cast<float>(working_->image.texture_height);
          }
          if (fade_from_ && !anim_frame_) {
            auto base = bp;
            // The preview covers the full image's output rectangle and uses
            // the same normalized edit map, even if its raster size differs.
            base.texture_w = static_cast<float>(fade_from_->width);
            base.texture_h = static_cast<float>(fade_from_->height);
            blitter_.draw((__bridge void*)enc, fade_from_->texture, base);
            bp.opacity = fade_.alpha(elapsed);
          }
          blitter_.draw((__bridge void*)enc,
                        use_working ? working_->image.texture
                                    : (anim_frame_ ? anim_frame_->texture : current_image_->texture),
                        bp);
        }
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];

        if (warmed_up_ && !options_.start_animating) ++idle_stats_.presents;
        ++total_presents_;
        pacer_.frame_end(tick);
        if (!decision.live) painted_static_ = true;
        redraw_owed_ = false;
        commit_nav_present();
      }
    }

    // submit_image_load()'s job holds a raw (non-retaining) id<MTLDevice>
    // pointer, so it must be finished before device_.destroy() below, on
    // every exit path (soak completing here, not just an external stop()).
    // job_system::shutdown() is documented safe to call twice, so this does
    // not conflict with a caller's own shutdown (main_mac.mm's windowWillClose).
    if (options_.jobs) options_.jobs->shutdown();

    if (warmed_up_) {
      idle_stats_.elapsed_seconds = monotonic_seconds() - measurement_start_seconds_;
      const double end_cpu = process_cpu_seconds();
      if (idle_start_cpu_seconds_ >= 0.0 && end_cpu >= idle_start_cpu_seconds_ &&
          idle_stats_.elapsed_seconds > 0.0) {
        idle_stats_.cpu_percent =
            (end_cpu - idle_start_cpu_seconds_) / idle_stats_.elapsed_seconds * 100.0;
      }
    }

    const auto final_stats = pacer_.stats();
    if (options_.soak_seconds > 0.0) {
      MV_LOG_INFO("soak: %llu frames over %.1f s, %llu dropped, source: %s",
                  static_cast<unsigned long long>(final_stats.frames), final_stats.elapsed_seconds,
                  static_cast<unsigned long long>(final_stats.dropped_frames),
                  gfx::metal_drop_source_label(final_stats.source));
      if (!write_json_report()) exit_code_ = 2;
      const bool passed = options_.start_animating ? final_stats.meets_pr16_gate()
                                                   : idle_stats_.meets_pr16_gate();
      if (options_.gate_exit_code && !(passed && soak_complete_ && measurement_valid_))
        exit_code_ = 1;
    }

    if (display_link_) {
      CAMetalDisplayLink* link = (__bridge_transfer CAMetalDisplayLink*)display_link_;
      link.paused = YES;
      [link removeFromRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
      display_link_ = nullptr;
    }
    if (link_target_) {
      (void)(__bridge_transfer MvMetalLinkTarget*)link_target_;
      link_target_ = nullptr;
    }
    if (imgui_ready_) {
      ImGui_ImplMetal_Shutdown();
      imgui_ready_ = false;
    }
    current_image_.reset();
    fade_from_.reset();
    fade_.cancel();
    delete pending_image_.exchange(nullptr);
    working_.reset();  // PR 11: before device_.destroy()
    delete pending_working_.exchange(nullptr);
    // Joins the animation decode thread before device_.destroy() below, the
    // same ordering reason submit_image_load's job must finish first: its
    // make_texture_fn closure holds the raw MTLDevice pointer.
    anim_session_.reset();
    anim_frame_.reset();
    // Clips: the pool is already shut down here, so retire_media() closes on
    // this thread (we are exiting; there is no render loop left to protect).
    retire_media();
    if (pending_media* pm = pending_media_.exchange(nullptr)) {
      player::close_media(pm->source);
      delete pm;
    }
    video_blitter_.destroy();
    blitter_.destroy();
    layer_.destroy();
    device_.destroy();
    ImGui::DestroyContext();
  }

  // The loop above exits two ways: running_ went false (an external stop()
  // call — main_mac.mm's applicationShouldTerminate: already owns quitting
  // the app once this call returns) or a soak's `break` above completed on
  // its own with running_ still true (nothing else is going to ask the app
  // to quit, so the tail below must). Nothing between here and the loop
  // touches running_, so reading it now is equivalent to reading it right
  // after the loop exited, before teardown — just without the scoping
  // problem of declaring it inside the block above and using it after.
  const bool self_initiated_exit = running_.load(std::memory_order_acquire);

  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  // Only self-terminate when nothing external asked us to stop: an
  // external stop() (main_mac.mm's applicationShouldTerminate:, mid its own
  // background-queue shutdown) already owns the one NSTerminateLater /
  // replyToApplicationShouldTerminate: cycle for this quit. Calling
  // [NSApp terminate:nil] again here would re-enter
  // applicationShouldTerminate: for a termination already in flight.
  if (self_initiated_exit) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApp terminate:nil];
    });
  }
}

bool present_lab_mac::write_json_report() const noexcept {
  if (options_.json_report_path.empty()) return true;
  FILE* f = std::fopen(options_.json_report_path.c_str(), "wb");
  if (!f) {
    MV_LOG_ERROR("present_lab_mac: cannot write report");
    return false;
  }
  gfx::pace_json r;
  r.pace = pacer_.stats();
  r.idle = idle_stats_;
  r.static_run = !options_.start_animating;
  r.still_first_pixel_s = first_pixel_seconds_;
  r.still_full_s = full_seconds_;
  r.measurement_complete = soak_complete_ && measurement_valid_ && exit_code_ == 0;
  r.meets_gate = r.measurement_complete &&
                 (options_.start_animating ? r.pace.meets_pr16_gate() : r.idle.meets_pr16_gate());
  bool ok = gfx::write_pace_json(f, r);
  if (media_) {
    const auto v = media_->stats();
    ok = write_video_report(options_.json_report_path,
        {v.counters.presented, v.counters.dropped_late, v.counters.held_cadence,
         v.counters.held_starved, v.err_ms_p99, v.audio_master}) && ok;
  }
  return std::fclose(f) == 0 && ok;
}

}  // namespace mv::shell
