// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/wayland_frame_clock.h"

#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "presentation-time-client-protocol.h"

namespace mv::gfx {

namespace {

wl_display* as_display(void* p) noexcept { return static_cast<wl_display*>(p); }
wl_event_queue* as_queue(void* p) noexcept { return static_cast<wl_event_queue*>(p); }

std::int64_t monotonic_ms() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

void registry_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                     std::uint32_t version) {
  static_cast<wayland_frame_clock*>(data)->on_global(registry, name, interface, version);
}
void registry_global_remove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener k_registry_listener = {registry_global, registry_global_remove};

void presentation_clock_id(void* data, wp_presentation*, std::uint32_t clock) {
  static_cast<wayland_frame_clock*>(data)->on_clock_id(clock);
}
const wp_presentation_listener k_presentation_listener = {presentation_clock_id};

void frame_done(void* data, wl_callback* callback, std::uint32_t) {
  static_cast<wayland_frame_clock*>(data)->on_frame_done(callback);
}
const wl_callback_listener k_frame_listener = {frame_done};

void feedback_sync_output(void*, struct wp_presentation_feedback*, wl_output*) {}
void feedback_presented(void* data, struct wp_presentation_feedback* feedback, std::uint32_t tv_sec_hi,
                        std::uint32_t tv_sec_lo, std::uint32_t tv_nsec, std::uint32_t refresh,
                        std::uint32_t seq_hi, std::uint32_t seq_lo, std::uint32_t flags) {
  const std::uint64_t seconds = (static_cast<std::uint64_t>(tv_sec_hi) << 32) | tv_sec_lo;
  const std::uint64_t msc = (static_cast<std::uint64_t>(seq_hi) << 32) | seq_lo;
  static_cast<wayland_frame_clock*>(data)->on_feedback(
      feedback, true, seconds * 1'000'000'000ull + tv_nsec, refresh, msc, flags);
}
void feedback_discarded(void* data, struct wp_presentation_feedback* feedback) {
  static_cast<wayland_frame_clock*>(data)->on_feedback(feedback, false, 0, 0, 0, 0);
}
const wp_presentation_feedback_listener k_feedback_listener = {
    feedback_sync_output, feedback_presented, feedback_discarded};

}  // namespace

wayland_frame_clock::~wayland_frame_clock() { destroy(); }

expected wayland_frame_clock::create(void* wl_display_ptr, void* wl_surface_ptr) noexcept {
  if (!wl_display_ptr || !wl_surface_ptr) return err(status::invalid_arg);
  destroy();
  display_ = wl_display_ptr;
  surface_ = wl_surface_ptr;
  wl_display* display = as_display(display_);

  queue_ = wl_display_create_queue(display);
  if (!queue_) return err(status::out_of_memory);
  display_wrapper_ = wl_proxy_create_wrapper(display);
  surface_wrapper_ = wl_proxy_create_wrapper(surface_);
  if (!display_wrapper_ || !surface_wrapper_) {
    destroy();
    return err(status::out_of_memory);
  }
  wl_proxy_set_queue(static_cast<wl_proxy*>(display_wrapper_), as_queue(queue_));
  wl_proxy_set_queue(static_cast<wl_proxy*>(surface_wrapper_), as_queue(queue_));

  // Our own bind of wp_presentation, on our queue: binding a global twice is
  // legal, and its feedback objects then answer on this queue, not the host's.
  auto* registry = wl_display_get_registry(static_cast<wl_display*>(display_wrapper_));
  registry_ = registry;
  if (!registry || wl_registry_add_listener(registry, &k_registry_listener, this) != 0) {
    destroy();
    return err(status::internal);
  }
  // Globals, then the clock_id event sent on bind.
  if (wl_display_roundtrip_queue(display, as_queue(queue_)) < 0 ||
      wl_display_roundtrip_queue(display, as_queue(queue_)) < 0) {
    destroy();
    return err(status::io);
  }
  return {};
}

void wayland_frame_clock::on_global(void* registry, std::uint32_t name, const char* interface,
                                    std::uint32_t version) noexcept {
  if (presentation_ || std::strcmp(interface, wp_presentation_interface.name) != 0) return;
  (void)version;
  // Version 1 on purpose: under v1 a variable-rate output must report refresh
  // 0, where v2 may report its fastest rate and a VRR run would look paced
  // (presentation-time.xml, `presented`; docs/design/12, 2026-10-23).
  auto* presentation = static_cast<wp_presentation*>(
      wl_registry_bind(static_cast<wl_registry*>(registry), name, &wp_presentation_interface, 1));
  if (!presentation) return;
  wp_presentation_add_listener(presentation, &k_presentation_listener, this);
  presentation_ = presentation;
}

void wayland_frame_clock::request(std::uint64_t frame_id) noexcept {
  if (!queue_) return;
  if (!callback_) {
    // A callback still outstanding (the lab never commits over one) fires at
    // the same repaint as this commit's would; keep that one.
    auto* callback = wl_surface_frame(static_cast<wl_surface*>(surface_wrapper_));
    if (callback) {
      wl_callback_add_listener(callback, &k_frame_listener, this);
      callback_ = callback;
    }
  }
  if (!presentation_) return;
  in_flight* slot = nullptr;
  for (auto& f : in_flight_) {
    if (!f.feedback) {
      slot = &f;
      break;
    }
  }
  if (!slot) {
    // The compositor has not answered max_in_flight commits. Give up on the
    // oldest; the pacer counts that frame unavailable when the window closes.
    slot = &in_flight_[0];
    for (auto& f : in_flight_) {
      if (f.frame_id < slot->frame_id) slot = &f;
    }
    wp_presentation_feedback_destroy(static_cast<struct wp_presentation_feedback*>(slot->feedback));
    slot->feedback = nullptr;
  }
  auto* feedback = wp_presentation_feedback(static_cast<wp_presentation*>(presentation_),
                                            static_cast<wl_surface*>(surface_));
  if (!feedback) return;
  wp_presentation_feedback_add_listener(feedback, &k_feedback_listener, this);
  slot->feedback = feedback;
  slot->frame_id = frame_id;
}

void wayland_frame_clock::on_frame_done(void* callback) noexcept {
  if (callback == callback_) callback_ = nullptr;
  wl_callback_destroy(static_cast<wl_callback*>(callback));
}

void wayland_frame_clock::on_feedback(void* feedback, bool presented, std::uint64_t timestamp_ns,
                                      std::uint32_t refresh_ns, std::uint64_t msc,
                                      std::uint32_t flags) noexcept {
  presentation_sample sample;
  bool known = false;
  for (auto& f : in_flight_) {
    if (f.feedback != feedback) continue;
    sample.frame_id = f.frame_id;
    f.feedback = nullptr;
    known = true;
    break;
  }
  wp_presentation_feedback_destroy(static_cast<struct wp_presentation_feedback*>(feedback));
  if (!known) return;
  sample.presented = presented;
  sample.timestamp_ns = timestamp_ns;
  sample.refresh_ns = refresh_ns;
  sample.msc = msc;
  sample.flags = flags;
  if (presented) last_flags_ = flags;
  push_sample(sample);
}

void wayland_frame_clock::push_sample(const presentation_sample& sample) noexcept {
  if (sample_count_ == sample_capacity) {
    // Nobody drained for 128 answers; the oldest is lost, and its frame
    // counts as unavailable, never as clean.
    sample_head_ = (sample_head_ + 1) % sample_capacity;
    --sample_count_;
  }
  samples_[(sample_head_ + sample_count_) % sample_capacity] = sample;
  ++sample_count_;
}

std::size_t wayland_frame_clock::drain(presentation_sample* out, std::size_t cap) noexcept {
  std::size_t n = 0;
  while (n < cap && sample_count_ > 0) {
    out[n++] = samples_[sample_head_];
    sample_head_ = (sample_head_ + 1) % sample_capacity;
    --sample_count_;
  }
  return n;
}

wayland_frame_clock::wait_result wayland_frame_clock::wait_frame(int timeout_ms,
                                                                 int wake_fd) noexcept {
  return pump(timeout_ms, wake_fd, true);
}

wayland_frame_clock::wait_result wayland_frame_clock::wait(int timeout_ms, int wake_fd) noexcept {
  return pump(timeout_ms, wake_fd, false);
}

wayland_frame_clock::wait_result wayland_frame_clock::pump(int timeout_ms, int wake_fd,
                                                           bool until_frame) noexcept {
  if (!queue_) return wait_result::failed;
  wl_display* display = as_display(display_);
  wl_event_queue* queue = as_queue(queue_);
  const bool had_callback = callback_ != nullptr;
  const auto frame_seen = [&] {
    return until_frame ? callback_ == nullptr : (had_callback && callback_ == nullptr);
  };
  const std::int64_t deadline = timeout_ms < 0 ? -1 : monotonic_ms() + timeout_ms;

  for (;;) {
    if (frame_seen()) return wait_result::frame;
    // Another thread (the host's UI loop, Mesa inside eglSwapBuffers) may read
    // the socket too: prepare, then read or cancel, never a plain read.
    while (wl_display_prepare_read_queue(display, queue) != 0) {
      if (wl_display_dispatch_queue_pending(display, queue) < 0) return wait_result::failed;
    }
    if (frame_seen()) {
      wl_display_cancel_read(display);
      return wait_result::frame;
    }
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      return wait_result::failed;
    }
    int wait_ms = -1;
    if (deadline >= 0) {
      const std::int64_t left = deadline - monotonic_ms();
      wait_ms = left > 0 ? static_cast<int>(left) : 0;
    }
    pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {wake_fd, POLLIN, 0}};
    const nfds_t count = wake_fd >= 0 ? 2 : 1;
    const int ready = ::poll(fds, count, wait_ms);
    if (ready < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) continue;
      return wait_result::failed;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(display) < 0) return wait_result::failed;
    } else {
      wl_display_cancel_read(display);
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return wait_result::failed;
    }
    if (wl_display_dispatch_queue_pending(display, queue) < 0) return wait_result::failed;
    if (frame_seen()) return wait_result::frame;  // a wake at the same time stays set
    if (count == 2 && (fds[1].revents & POLLIN)) {
      std::uint64_t value = 0;
      (void)!::read(wake_fd, &value, sizeof value);
      return wait_result::woken;
    }
    if (deadline >= 0 && monotonic_ms() >= deadline) return wait_result::timeout;
  }
}

void wayland_frame_clock::destroy() noexcept {
  for (auto& f : in_flight_) {
    if (f.feedback) wp_presentation_feedback_destroy(static_cast<struct wp_presentation_feedback*>(f.feedback));
    f = {};
  }
  if (callback_) wl_callback_destroy(static_cast<wl_callback*>(callback_));
  callback_ = nullptr;
  if (presentation_) wp_presentation_destroy(static_cast<wp_presentation*>(presentation_));
  presentation_ = nullptr;
  if (registry_) wl_registry_destroy(static_cast<wl_registry*>(registry_));
  registry_ = nullptr;
  if (surface_wrapper_) wl_proxy_wrapper_destroy(surface_wrapper_);
  surface_wrapper_ = nullptr;
  if (display_wrapper_) wl_proxy_wrapper_destroy(display_wrapper_);
  display_wrapper_ = nullptr;
  if (queue_) wl_event_queue_destroy(as_queue(queue_));
  queue_ = nullptr;
  sample_head_ = 0;
  sample_count_ = 0;
  clock_id_ = -1;
  display_ = nullptr;
  surface_ = nullptr;
}

}  // namespace mv::gfx
