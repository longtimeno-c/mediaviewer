// SPDX-License-Identifier: GPL-2.0-or-later
// Animated GIF / APNG / WebP playback feed (plan/04 "Animation").
//
// One decode thread per session pulls frames from a codec::animation_source
// and uploads each as an immutable texture (plan/02: workers create GPU
// textures with initial data; the render thread never Maps a frame) into a
// small SPSC ring. The render thread takes a frame when it is due. Frame 0
// already went through the still path, so the animation is never the first
// pixel (rule 3).
//
// The loop count is decided here: at the end of a play the source rewinds if
// plays remain, otherwise the feed ends. Generation-tied: navigation retires
// the animation and every queued frame is released. The render thread never
// takes the mutex; it only pops the ring and pokes the decode thread.
//
// Memory: the file's bytes are held by the source for as long as the item is
// selected (a 200 MB GIF sits in RAM while it plays) and released on
// navigation — not on LRU eviction. Frames ahead are bounded by
// animation_ring_depth; one canvas lives in the source.
//
// Templated on the GPU texture type (`Texture`) rather than hardcoded to
// image::gpu_image: this file has no D3D11 in it at all (only
// std::unique_ptr<Texture>/Texture* by pointer, never a member access), so
// the Metal host (present_lab_mac.mm) instantiates the identical ring/
// generation/epoch logic as image::gpu_image_mac rather than a hand-copied
// twin drifting from this one (D9 — the point of a hostable core).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "codec/anim.h"
#include "core/spsc_ring.h"

namespace mv::abi {

// POD over the ring. `texture` is owned by whoever pops the message.
template <typename Texture>
struct animation_frame {
  Texture* texture = nullptr;
  std::uint32_t delay_ms = 0;
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
  std::uint32_t epoch = 0;  // bumped by a seek; older frames are stale
};

// Frames decoded ahead (review note B: a worker CreateTexture2D still counts
// against the pacing budget). Two for large canvases, up to six for small
// ones, and never more than ~256 MB of frames waiting.
[[nodiscard]] constexpr std::uint32_t animation_ring_depth(std::uint32_t width,
                                                           std::uint32_t height) noexcept {
  const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
  if (pixels == 0 || pixels > 8'000'000ull) return 2;
  const std::uint64_t by_budget = (256ull * 1024ull * 1024ull) / (pixels * 4);
  return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(by_budget, 2, 6));
}

template <typename Texture>
class animation_session {
 public:
  using frame_type = animation_frame<Texture>;
  using make_texture_fn = std::function<std::unique_ptr<Texture>(
      const codec::canvas_frame&, const codec::animation_info&, std::uint32_t generation)>;

  explicit animation_session(make_texture_fn make)
      : make_(std::move(make)), thread_([this] { run(); }) {}

  ~animation_session() {
    running_.store(false, std::memory_order_release);
    poke();
    thread_.join();
    drain();
  }

  animation_session(const animation_session&) = delete;
  animation_session& operator=(const animation_session&) = delete;

  // [loader] The selected item is animated. Replaces whatever was playing.
  void publish(std::unique_ptr<codec::animation_source> source, std::uint32_t generation) {
    {
      std::lock_guard lock(mutex_);
      pending_ = std::move(source);
      pending_gen_ = generation;
    }
    open_gen_.store(generation, std::memory_order_release);
    ended_gen_.store(0, std::memory_order_release);
    poke();
  }

  // [render] The view moved to `generation`: anything older stops decoding and
  // its queued frames are released.
  void retire(std::uint32_t generation) noexcept {
    if (wanted_gen_.exchange(generation, std::memory_order_acq_rel) == generation) return;
    drain();
    poke();
  }

  // [render][no-block] An animation was published for `generation`.
  [[nodiscard]] bool open(std::uint32_t generation) const noexcept {
    return generation != 0 && open_gen_.load(std::memory_order_acquire) == generation;
  }

  // [render][no-block] Takes the next frame for `generation`, if one is ready.
  // A caller asking with a retired generation gets nothing; for the current
  // one, frames from an older generation or from before a seek are released
  // here as they are popped.
  [[nodiscard]] bool take(std::uint32_t generation, frame_type& out) noexcept {
    frame_type f;
    // Retirement is authoritative. A frame can still be pushed after retire()
    // drained the ring — the decode thread may already be past its check, one
    // texture in flight — and that frame carries the retired generation. It
    // must not come back to a caller still asking with that generation.
    // Only the render thread retires and takes, so this is never stale.
    // Leave the ring alone: the frames in it belong to the generation that
    // replaced this one, and its own take() releases anything older.
    const std::uint32_t wanted = wanted_gen_.load(std::memory_order_acquire);
    if (wanted != 0 && generation != wanted) return false;
    const std::uint32_t epoch = epoch_.load(std::memory_order_acquire);
    while (ring_.try_pop(f)) {
      if (f.generation != generation || f.epoch != epoch) {
        delete f.texture;
        continue;
      }
      out = f;
      poke();  // room for one more
      return true;
    }
    return false;
  }

  // [render][no-block] Nothing more will come for `generation`: the loop count
  // is exhausted, the file broke, or it was a one-frame still after all.
  [[nodiscard]] bool finished(std::uint32_t generation) const noexcept {
    return ended_gen_.load(std::memory_order_acquire) == generation && ring_.empty();
  }

  // [render][no-block] The next frame produced is `index` (`,` stepping back):
  // the decoder rewinds and decodes forward to it.
  void seek(std::uint32_t index) noexcept {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    seek_to_.store(index + 1, std::memory_order_release);
    drain();
    poke();
  }

  // [any-thread][no-block] F3 figures.
  [[nodiscard]] std::uint32_t depth() const noexcept { return depth_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t queued() const noexcept {
    return static_cast<std::uint32_t>(ring_.size_approx());
  }
  [[nodiscard]] std::uint32_t last_upload_us() const noexcept {
    return last_upload_us_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t frames_made() const noexcept {
    return frames_made_.load(std::memory_order_relaxed);
  }
  // The file loops forever (slideshow: advance on the interval, not the end).
  // A GIF only says so once its first frames are decoded.
  [[nodiscard]] bool loops_forever() const noexcept {
    return loops_.load(std::memory_order_relaxed) == 0;
  }

 private:
  void poke() noexcept {
    signal_.fetch_add(1, std::memory_order_acq_rel);
    signal_.notify_one();
  }

  // Consumer side only (render thread, or the destructor after the join).
  void drain() noexcept {
    frame_type f;
    while (ring_.try_pop(f)) delete f.texture;
  }

  void run() {
    std::unique_ptr<codec::animation_source> current;
    std::uint32_t gen = 0;
    std::uint32_t plays = 0;
    std::uint32_t produced_this_play = 0;
    bool ended = true;
    codec::canvas_frame frame;

    const auto end_feed = [&]() noexcept {
      ended = true;
      ended_gen_.store(gen, std::memory_order_release);
    };

    while (running_.load(std::memory_order_acquire)) {
      const unsigned observed = signal_.load(std::memory_order_acquire);
      {
        std::lock_guard lock(mutex_);
        if (pending_) {
          current = std::move(pending_);
          gen = pending_gen_;
          plays = 0;
          produced_this_play = 0;
          ended = false;
          depth_.store(animation_ring_depth(current->info().width, current->info().height),
                       std::memory_order_relaxed);
        }
      }
      const std::uint32_t wanted = wanted_gen_.load(std::memory_order_acquire);
      if (current && wanted != 0 && wanted != gen) {
        current.reset();  // releases the source, and with it the file's bytes
        ended = true;
      }

      if (current) {
        if (const std::uint32_t seek = seek_to_.exchange(0, std::memory_order_acq_rel); seek != 0) {
          ended = false;
          ended_gen_.store(0, std::memory_order_release);
          produced_this_play = 0;
          if (!current->rewind()) {
            end_feed();
          } else {
            for (std::uint32_t i = 0; i + 1 < seek; ++i) {
              auto more = current->next(frame, nullptr);
              if (!more || !more.value()) break;
              ++produced_this_play;
            }
          }
        }
      }

      const std::uint32_t epoch = epoch_.load(std::memory_order_acquire);
      while (current && !ended && running_.load(std::memory_order_acquire) &&
             seek_to_.load(std::memory_order_acquire) == 0 &&
             (wanted_gen_.load(std::memory_order_acquire) == 0 ||
              wanted_gen_.load(std::memory_order_acquire) == gen) &&
             ring_.size_approx() < depth_.load(std::memory_order_relaxed)) {
        auto more = current->next(frame, nullptr);
        if (!more) {
          end_feed();
          break;
        }
        if (!more.value()) {
          ++plays;
          const std::uint32_t loops = current->info().loops;
          // A one-frame file is a still, however many times it asks to loop.
          if (produced_this_play <= 1 || (loops != 0 && plays >= loops) || !current->rewind()) {
            end_feed();
            break;
          }
          produced_this_play = 0;
          continue;
        }
        ++produced_this_play;
        loops_.store(current->info().loops, std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Texture> texture = make_ ? make_(frame, current->info(), gen) : nullptr;
        last_upload_us_.store(static_cast<std::uint32_t>(
                                  std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count()),
                              std::memory_order_relaxed);
        if (!texture) {
          end_feed();
          break;
        }
        const frame_type msg{texture.get(), frame.delay_ms, frame.index, gen, epoch};
        if (!ring_.try_push(msg)) break;  // full after all: the frame is dropped
        texture.release();
        frames_made_.fetch_add(1, std::memory_order_relaxed);
      }

      if (running_.load(std::memory_order_acquire) &&
          signal_.load(std::memory_order_acquire) == observed) {
        signal_.wait(observed);
      }
    }
  }

  make_texture_fn make_;

  std::mutex mutex_;  // loader <-> decode thread only; never the render thread
  std::unique_ptr<codec::animation_source> pending_;
  std::uint32_t pending_gen_ = 0;

  spsc_ring<frame_type, 8> ring_;
  std::atomic<std::uint32_t> open_gen_{0};
  std::atomic<std::uint32_t> wanted_gen_{0};
  std::atomic<std::uint32_t> ended_gen_{0};
  std::atomic<std::uint32_t> epoch_{0};
  std::atomic<std::uint32_t> seek_to_{0};
  std::atomic<std::uint32_t> depth_{2};
  std::atomic<std::uint32_t> last_upload_us_{0};
  std::atomic<std::uint64_t> frames_made_{0};
  std::atomic<std::uint32_t> loops_{1};
  std::atomic<bool> running_{true};
  std::atomic<unsigned> signal_{0};
  std::thread thread_;  // last: started once everything above exists
};

}  // namespace mv::abi
