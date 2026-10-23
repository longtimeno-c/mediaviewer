// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/wayland_window_linux.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>

#include "core/spsc_ring.h"
#include "core/trace.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace mv::shell {

namespace {

// The highest version of each interface whose events the listeners below
// handle, given the wayland-client headers this was built against.
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
constexpr std::uint32_t k_compositor_version = 6;
#else
constexpr std::uint32_t k_compositor_version = 4;
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
constexpr std::uint32_t k_seat_version = 9;
#elif defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
constexpr std::uint32_t k_seat_version = 8;
#else
constexpr std::uint32_t k_seat_version = 7;
#endif
#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
constexpr std::uint32_t k_output_version = 4;
#else
constexpr std::uint32_t k_output_version = 3;
#endif
// v6 sends `suspended`; v7's listener has nothing more the lab reads.
constexpr std::uint32_t k_wm_base_version = 6;

constexpr int k_max_outputs = 8;
constexpr int k_first_configure_timeout_ms = 5000;

std::int64_t monotonic_ms() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

// What the render thread needs to make a commit match a configure: published
// by the UI thread on every configure and scale change, read in
// before_commit().
struct surface_config {
  std::uint32_t serial = 0;  // the newest configure; 0 = none to ack
  std::uint32_t physical_w = 0;
  std::uint32_t physical_h = 0;
  std::uint32_t logical_w = 0;
  std::uint32_t logical_h = 0;
  std::int32_t buffer_scale = 1;  // used only without wp_viewporter
};

struct output_info {
  wl_output* proxy = nullptr;
  std::uint32_t name = 0;
  std::uint32_t refresh_mhz = 0;
  std::int32_t scale = 1;
  bool entered = false;
};

std::uint32_t clamp_version(std::uint32_t offered, std::uint32_t wanted) noexcept {
  return offered < wanted ? offered : wanted;
}

}  // namespace

struct wayland_window_linux::impl {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  std::uint32_t compositor_version = 0;
  xdg_wm_base* wm_base = nullptr;
  wl_seat* seat = nullptr;
  std::uint32_t seat_version = 0;
  wl_pointer* pointer = nullptr;
  wl_keyboard* keyboard = nullptr;
  wp_viewporter* viewporter = nullptr;
  wp_fractional_scale_manager_v1* fractional_manager = nullptr;
  zxdg_decoration_manager_v1* decoration_manager = nullptr;
  output_info outputs[k_max_outputs];

  wl_surface* surface = nullptr;
  xdg_surface* xdg = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wp_viewport* viewport = nullptr;
  wp_fractional_scale_v1* fractional = nullptr;
  zxdg_toplevel_decoration_v1* decoration = nullptr;

  xkb_context* xkb = nullptr;
  xkb_keymap* keymap = nullptr;
  xkb_state* keys = nullptr;

  // UI thread state.
  std::uint32_t logical_w = 0;
  std::uint32_t logical_h = 0;
  std::int32_t pending_w = 0;
  std::int32_t pending_h = 0;
  bool pending_activated = false;
  bool pending_suspended = false;
  bool configured = false;
  std::uint32_t scale_120 = 0;      // wp_fractional_scale_v1, 0 = not offered
  std::int32_t preferred_scale = 0; // wl_surface.preferred_buffer_scale, 0 = not sent
  std::uint32_t last_serial = 0;
  bool close_requested = false;
  bool changed = false;
  input_snapshot snap;
  // Pointer events of one wl_pointer.frame.
  std::int32_t frame_v120 = 0;
  std::int32_t frame_discrete = 0;
  double frame_value = 0.0;
  bool frame_has_v120 = false;
  bool frame_has_discrete = false;
  bool frame_has_value = false;
  float pointer_logical_x = 0.0f;
  float pointer_logical_y = 0.0f;

  std::atomic<std::uint32_t> refresh{0};
  publish_slot<surface_config> config;

  // Render thread state (before_commit).
  std::uint32_t acked_serial = 0;
  std::uint32_t dest_w = 0;
  std::uint32_t dest_h = 0;
  std::int32_t committed_scale = 1;

  [[nodiscard]] double scale() const noexcept {
    if (viewporter && scale_120 > 0) return static_cast<double>(scale_120) / 120.0;
    if (preferred_scale > 0) return static_cast<double>(preferred_scale);
    std::int32_t best = 1;
    for (const auto& o : outputs) {
      if (o.proxy && o.entered && o.scale > best) best = o.scale;
    }
    return static_cast<double>(best);
  }

  // Logical size and scale -> the snapshot's physical size, and the config
  // the render thread will commit against. A resize bumps resize_seq.
  void publish_size() noexcept {
    const double s = scale();
    surface_config c;
    c.serial = last_serial;
    c.logical_w = logical_w;
    c.logical_h = logical_h;
    if (viewporter) {
      // Buffer scale 1 and a viewport onto the logical size: fractional
      // scales need it, integer ones work the same way.
      c.buffer_scale = 1;
      c.physical_w = static_cast<std::uint32_t>(std::lround(logical_w * s));
      c.physical_h = static_cast<std::uint32_t>(std::lround(logical_h * s));
    } else {
      c.buffer_scale = static_cast<std::int32_t>(s);
      c.physical_w = logical_w * static_cast<std::uint32_t>(c.buffer_scale);
      c.physical_h = logical_h * static_cast<std::uint32_t>(c.buffer_scale);
    }
    if (c.physical_w == 0) c.physical_w = 1;
    if (c.physical_h == 0) c.physical_h = 1;
    config.publish(c);
    snap.width = c.physical_w;
    snap.height = c.physical_h;
    snap.dpi_scale = static_cast<float>(s);
    // Every configure is a frame: the size, the scale and its ack ride on the
    // next commit, and an idle lab has none coming otherwise.
    ++snap.resize_seq;
    changed = true;
  }

  void update_refresh() noexcept {
    std::uint32_t value = 0;
    bool any = false;
    bool mixed = false;
    for (const auto& o : outputs) {
      if (!o.proxy || !o.entered) continue;
      if (any && o.refresh_mhz != value) mixed = true;
      value = o.refresh_mhz;
      any = true;
    }
    if (!any) {
      // Not on an output yet: one output is the only one it can be on.
      int known = 0;
      for (const auto& o : outputs) {
        if (o.proxy) {
          value = o.refresh_mhz;
          ++known;
        }
      }
      if (known != 1) value = 0;
    }
    if (mixed) value = 0;
    if (refresh.exchange(value, std::memory_order_acq_rel) != value) {
      ++snap.display_change_seq;
      changed = true;
    }
  }

  output_info* output_for(wl_output* proxy) noexcept {
    for (auto& o : outputs) {
      if (o.proxy == proxy) return &o;
    }
    return nullptr;
  }

  void bump_activity() noexcept {
    ++snap.activity_seq;
    changed = true;
  }
};

namespace {

using impl = wayland_window_linux::impl;

// --- xdg-shell ----------------------------------------------------------------

void wm_ping(void*, xdg_wm_base* base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener k_wm_listener = {wm_ping};

void toplevel_configure(void* data, xdg_toplevel*, std::int32_t width, std::int32_t height,
                        wl_array* states) {
  auto* w = static_cast<impl*>(data);
  w->pending_w = width;
  w->pending_h = height;
  w->pending_activated = false;
  w->pending_suspended = false;
  const auto* begin = static_cast<const std::uint32_t*>(states->data);
  const std::size_t count = states->size / sizeof(std::uint32_t);
  for (std::size_t i = 0; i < count; ++i) {
    if (begin[i] == XDG_TOPLEVEL_STATE_ACTIVATED) w->pending_activated = true;
#if defined(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
    if (begin[i] == XDG_TOPLEVEL_STATE_SUSPENDED) w->pending_suspended = true;
#endif
  }
}
void toplevel_close(void* data, xdg_toplevel*) { static_cast<impl*>(data)->close_requested = true; }
void toplevel_bounds(void*, xdg_toplevel*, std::int32_t, std::int32_t) {}
void toplevel_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener k_toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

void xdg_configure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* w = static_cast<impl*>(data);
  if (w->pending_w > 0 && w->pending_h > 0) {
    w->logical_w = static_cast<std::uint32_t>(w->pending_w);
    w->logical_h = static_cast<std::uint32_t>(w->pending_h);
  }
  w->snap.window_active = w->pending_activated;
  // Suspended (xdg-shell v6): minimised, or wholly covered on a compositor
  // that says so. The render thread stops presenting, as on occlusion.
  w->snap.window_visible = !w->pending_suspended;
  if (!w->configured) {
    // Nothing committed yet: ack here. Later acks ride on the render thread's
    // commit of the matching size (before_commit).
    xdg_surface_ack_configure(surface, serial);
    w->acked_serial = serial;
    w->last_serial = 0;
    w->configured = true;
  } else {
    w->last_serial = serial;
  }
  w->publish_size();
}
const xdg_surface_listener k_xdg_surface_listener = {xdg_configure};

void decoration_configure(void*, zxdg_toplevel_decoration_v1*, std::uint32_t) {}
const zxdg_toplevel_decoration_v1_listener k_decoration_listener = {decoration_configure};

// --- scale and outputs -------------------------------------------------------------

void fractional_preferred(void* data, wp_fractional_scale_v1*, std::uint32_t scale) {
  auto* w = static_cast<impl*>(data);
  if (scale == w->scale_120) return;
  w->scale_120 = scale;
  if (w->configured) w->publish_size();
}
const wp_fractional_scale_v1_listener k_fractional_listener = {fractional_preferred};

void surface_enter(void* data, wl_surface*, wl_output* output) {
  auto* w = static_cast<impl*>(data);
  if (auto* o = w->output_for(output)) o->entered = true;
  w->update_refresh();
  if (w->configured && w->scale_120 == 0 && w->preferred_scale == 0) w->publish_size();
}
void surface_leave(void* data, wl_surface*, wl_output* output) {
  auto* w = static_cast<impl*>(data);
  if (auto* o = w->output_for(output)) o->entered = false;
  w->update_refresh();
  if (w->configured && w->scale_120 == 0 && w->preferred_scale == 0) w->publish_size();
}
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
void surface_preferred_scale(void* data, wl_surface*, std::int32_t factor) {
  auto* w = static_cast<impl*>(data);
  if (factor == w->preferred_scale) return;
  w->preferred_scale = factor;
  if (w->configured && w->scale_120 == 0) w->publish_size();
}
void surface_preferred_transform(void*, wl_surface*, std::uint32_t) {}
#endif
const wl_surface_listener k_surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = surface_preferred_transform,
#endif
};

void output_geometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t, const char*, const char*, std::int32_t) {}
void output_mode(void* data, wl_output* output, std::uint32_t flags, std::int32_t, std::int32_t,
                 std::int32_t refresh) {
  auto* w = static_cast<impl*>(data);
  if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
  if (auto* o = w->output_for(output)) o->refresh_mhz = refresh > 0 ? static_cast<std::uint32_t>(refresh) : 0;
}
void output_done(void* data, wl_output*) { static_cast<impl*>(data)->update_refresh(); }
void output_scale(void* data, wl_output* output, std::int32_t factor) {
  auto* w = static_cast<impl*>(data);
  if (auto* o = w->output_for(output)) o->scale = factor > 0 ? factor : 1;
}
#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
void output_name(void*, wl_output*, const char*) {}
void output_description(void*, wl_output*, const char*) {}
#endif
const wl_output_listener k_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
    .name = output_name,
    .description = output_description,
#endif
};

// --- pointer -------------------------------------------------------------------

void set_pointer(impl* w, wl_fixed_t x, wl_fixed_t y) noexcept {
  const auto lx = static_cast<float>(wl_fixed_to_double(x));
  const auto ly = static_cast<float>(wl_fixed_to_double(y));
  // Physical pixels, as the snapshot carries on every host. Only real
  // movement is activity: a parked cursor must not keep the loop live.
  if (lx != w->pointer_logical_x || ly != w->pointer_logical_y || !w->snap.mouse_in_client) {
    w->pointer_logical_x = lx;
    w->pointer_logical_y = ly;
    const auto s = static_cast<float>(w->scale());
    w->snap.mouse_x = lx * s;
    w->snap.mouse_y = ly * s;
    w->bump_activity();
  }
}

void pointer_enter(void* data, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
  auto* w = static_cast<impl*>(data);
  set_pointer(w, x, y);
  w->snap.mouse_in_client = true;
}
void pointer_leave(void* data, wl_pointer*, std::uint32_t, wl_surface*) {
  auto* w = static_cast<impl*>(data);
  w->snap.mouse_in_client = false;
  for (bool& b : w->snap.mouse_down) b = false;
  w->bump_activity();
}
void pointer_motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
  set_pointer(static_cast<impl*>(data), x, y);
}
void pointer_button(void* data, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t button,
                    std::uint32_t state) {
  auto* w = static_cast<impl*>(data);
  const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
  int index = -1;
  if (button == BTN_LEFT) index = static_cast<int>(mouse_button::left);
  if (button == BTN_RIGHT) index = static_cast<int>(mouse_button::right);
  if (button == BTN_MIDDLE) index = static_cast<int>(mouse_button::middle);
  if (index < 0) return;
  w->snap.mouse_down[index] = down;
  w->bump_activity();
}
// The wheel in the snapshot's units: 120 per notch, positive up (Windows'
// WHEEL_DELTA). Wayland's vertical axis is positive down.
void apply_wheel(impl* w) noexcept {
  std::int64_t delta = 0;
  if (w->frame_has_v120) {
    delta = -static_cast<std::int64_t>(w->frame_v120);
  } else if (w->frame_has_discrete) {
    delta = -static_cast<std::int64_t>(w->frame_discrete) * 120;
  } else if (w->frame_has_value) {
    // A touchpad's continuous scroll: about 10 units a notch on most compositors.
    delta = -static_cast<std::int64_t>(std::lround(w->frame_value * 12.0));
  }
  w->frame_v120 = w->frame_discrete = 0;
  w->frame_value = 0.0;
  w->frame_has_v120 = w->frame_has_discrete = w->frame_has_value = false;
  if (delta == 0) return;
  w->snap.wheel_total += delta;
  w->bump_activity();
}
void pointer_axis(void* data, wl_pointer*, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
  auto* w = static_cast<impl*>(data);
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  w->frame_value += wl_fixed_to_double(value);
  w->frame_has_value = true;
  if (w->seat_version < 5) apply_wheel(w);  // no frame event to wait for
}
void pointer_frame(void* data, wl_pointer*) { apply_wheel(static_cast<impl*>(data)); }
void pointer_axis_source(void*, wl_pointer*, std::uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
void pointer_axis_discrete(void* data, wl_pointer*, std::uint32_t axis, std::int32_t discrete) {
  auto* w = static_cast<impl*>(data);
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  w->frame_discrete += discrete;
  w->frame_has_discrete = true;
}
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
void pointer_axis_value120(void* data, wl_pointer*, std::uint32_t axis, std::int32_t value120) {
  auto* w = static_cast<impl*>(data);
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  w->frame_v120 += value120;
  w->frame_has_v120 = true;
}
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
void pointer_axis_direction(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
#endif
const wl_pointer_listener k_pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
    .axis_value120 = pointer_axis_value120,
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
    .axis_relative_direction = pointer_axis_direction,
#endif
};

// --- keyboard ------------------------------------------------------------------

void keyboard_keymap(void* data, wl_keyboard*, std::uint32_t format, std::int32_t fd,
                     std::uint32_t size) {
  auto* w = static_cast<impl*>(data);
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    ::close(fd);
    return;
  }
  void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) return;
  xkb_keymap* keymap = xkb_keymap_new_from_buffer(w->xkb, static_cast<const char*>(map),
                                                  ::strnlen(static_cast<const char*>(map), size),
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
  ::munmap(map, size);
  if (!keymap) return;
  xkb_state* keys = xkb_state_new(keymap);
  if (!keys) {
    xkb_keymap_unref(keymap);
    return;
  }
  if (w->keys) xkb_state_unref(w->keys);
  if (w->keymap) xkb_keymap_unref(w->keymap);
  w->keymap = keymap;
  w->keys = keys;
}
void keyboard_enter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) {}
void keyboard_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*) {}
void keyboard_key(void* data, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key,
                  std::uint32_t state) {
  auto* w = static_cast<impl*>(data);
  if (!w->keys || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
  // evdev code + 8 is the XKB keycode.
  const xkb_keysym_t sym = xkb_state_key_get_one_sym(w->keys, key + 8);
  switch (sym) {
    case XKB_KEY_F3:
    case XKB_KEY_f:
    case XKB_KEY_F:
      ++w->snap.toggle_overlay_seq;
      break;
    case XKB_KEY_space:
      ++w->snap.toggle_animation_seq;
      break;
    case XKB_KEY_r:
    case XKB_KEY_R:
      ++w->snap.reset_stats_seq;
      break;
    case XKB_KEY_Escape:
      w->close_requested = true;
      break;
    default:
      break;
  }
  w->bump_activity();
}
void keyboard_modifiers(void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed,
                        std::uint32_t latched, std::uint32_t locked, std::uint32_t group) {
  auto* w = static_cast<impl*>(data);
  if (w->keys) xkb_state_update_mask(w->keys, depressed, latched, locked, 0, 0, group);
}
void keyboard_repeat(void*, wl_keyboard*, std::int32_t, std::int32_t) {}
const wl_keyboard_listener k_keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat,
};

// --- seat and registry -----------------------------------------------------------

void seat_capabilities(void* data, wl_seat* seat, std::uint32_t caps) {
  auto* w = static_cast<impl*>(data);
  const bool has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
  const bool has_keyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
  if (has_pointer && !w->pointer) {
    w->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(w->pointer, &k_pointer_listener, w);
  } else if (!has_pointer && w->pointer) {
    wl_pointer_destroy(w->pointer);
    w->pointer = nullptr;
  }
  if (has_keyboard && !w->keyboard) {
    w->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(w->keyboard, &k_keyboard_listener, w);
  } else if (!has_keyboard && w->keyboard) {
    wl_keyboard_destroy(w->keyboard);
    w->keyboard = nullptr;
  }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener k_seat_listener = {seat_capabilities, seat_name};

void registry_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                     std::uint32_t version) {
  auto* w = static_cast<impl*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0 && !w->compositor) {
    w->compositor_version = clamp_version(version, k_compositor_version);
    w->compositor = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, w->compositor_version));
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0 && !w->wm_base) {
    w->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(
        registry, name, &xdg_wm_base_interface, clamp_version(version, k_wm_base_version)));
    xdg_wm_base_add_listener(w->wm_base, &k_wm_listener, w);
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !w->seat) {
    w->seat_version = clamp_version(version, k_seat_version);
    w->seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface, w->seat_version));
    wl_seat_add_listener(w->seat, &k_seat_listener, w);
  } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
    for (auto& o : w->outputs) {
      if (o.proxy) continue;
      o = {};
      o.name = name;
      o.proxy = static_cast<wl_output*>(wl_registry_bind(
          registry, name, &wl_output_interface, clamp_version(version, k_output_version)));
      wl_output_add_listener(o.proxy, &k_output_listener, w);
      break;
    }
  } else if (std::strcmp(interface, wp_viewporter_interface.name) == 0 && !w->viewporter) {
    w->viewporter = static_cast<wp_viewporter*>(
        wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  } else if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 &&
             !w->fractional_manager) {
    w->fractional_manager = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
  } else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 &&
             !w->decoration_manager) {
    w->decoration_manager = static_cast<zxdg_decoration_manager_v1*>(
        wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
  }
  // wp_tearing_control_v1 is never bound: the canvas does not tear
  // (docs/design/03). wp_fifo_v1 and wp_commit_timing_v1 belong to whoever
  // attaches buffers, which is EGL (NVIDIA's egl-wayland2 uses them itself).
}
void registry_global_remove(void* data, wl_registry*, std::uint32_t name) {
  auto* w = static_cast<impl*>(data);
  for (auto& o : w->outputs) {
    if (o.proxy && o.name == name) {
      wl_output_destroy(o.proxy);
      o = {};
      w->update_refresh();
    }
  }
}
const wl_registry_listener k_registry_listener = {registry_global, registry_global_remove};

}  // namespace

wayland_window_linux::wayland_window_linux() : impl_(std::make_unique<impl>()) {}
wayland_window_linux::~wayland_window_linux() { close(); }

void* wayland_window_linux::display() const noexcept { return impl_->display; }
void* wayland_window_linux::surface() const noexcept { return impl_->surface; }
bool wayland_window_linux::close_requested() const noexcept { return impl_->close_requested; }
const input_snapshot& wayland_window_linux::snapshot() const noexcept { return impl_->snap; }
std::uint32_t wayland_window_linux::refresh_mhz() const noexcept {
  return impl_->refresh.load(std::memory_order_acquire);
}

bool wayland_window_linux::take_changed() noexcept {
  const bool was = impl_->changed;
  impl_->changed = false;
  return was;
}

expected wayland_window_linux::open(const char* title, std::uint32_t logical_width,
                                    std::uint32_t logical_height) noexcept {
  impl& w = *impl_;
  if (w.display) return err(status::invalid_arg);
  w.display = wl_display_connect(nullptr);
  if (!w.display) {
    MV_LOG_ERROR("wayland_window: no Wayland compositor (WAYLAND_DISPLAY)");
    return err(status::unsupported_format);
  }
  w.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  w.registry = wl_display_get_registry(w.display);
  wl_registry_add_listener(w.registry, &k_registry_listener, &w);
  // Globals, then what binding them sent (seat capabilities, output modes).
  if (wl_display_roundtrip(w.display) < 0 || wl_display_roundtrip(w.display) < 0) {
    close();
    return err(status::io);
  }
  if (!w.compositor || !w.wm_base) {
    MV_LOG_ERROR("wayland_window: the compositor offers no %s",
                 w.compositor ? "xdg_wm_base" : "wl_compositor");
    close();
    return err(status::unsupported_format);
  }

  w.logical_w = logical_width;
  w.logical_h = logical_height;
  w.surface = wl_compositor_create_surface(w.compositor);
  wl_surface_add_listener(w.surface, &k_surface_listener, &w);
  if (w.viewporter) w.viewport = wp_viewporter_get_viewport(w.viewporter, w.surface);
  if (w.viewporter && w.fractional_manager) {
    w.fractional =
        wp_fractional_scale_manager_v1_get_fractional_scale(w.fractional_manager, w.surface);
    wp_fractional_scale_v1_add_listener(w.fractional, &k_fractional_listener, &w);
  }
  w.xdg = xdg_wm_base_get_xdg_surface(w.wm_base, w.surface);
  xdg_surface_add_listener(w.xdg, &k_xdg_surface_listener, &w);
  w.toplevel = xdg_surface_get_toplevel(w.xdg);
  xdg_toplevel_add_listener(w.toplevel, &k_toplevel_listener, &w);
  xdg_toplevel_set_title(w.toplevel, title);
  xdg_toplevel_set_app_id(w.toplevel, "io.github.longtimeno_c.MediaViewer.Lab");
  if (w.decoration_manager) {
    w.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(w.decoration_manager, w.toplevel);
    zxdg_toplevel_decoration_v1_add_listener(w.decoration, &k_decoration_listener, &w);
    zxdg_toplevel_decoration_v1_set_mode(w.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  // The xdg handshake: commit with no buffer, wait for the first configure.
  wl_surface_commit(w.surface);
  const std::int64_t deadline = monotonic_ms() + k_first_configure_timeout_ms;
  while (!w.configured) {
    const std::int64_t left = deadline - monotonic_ms();
    bool unused = false;
    if (left <= 0 || !dispatch(static_cast<int>(left), -1, &unused)) {
      MV_LOG_ERROR("wayland_window: no configure from the compositor");
      close();
      return err(status::timeout);
    }
  }
  w.update_refresh();
  return {};
}

bool wayland_window_linux::dispatch(int timeout_ms, int extra_fd, bool* extra_ready) noexcept {
  impl& w = *impl_;
  if (extra_ready) *extra_ready = false;
  if (!w.display) return false;
  while (wl_display_prepare_read(w.display) != 0) {
    if (wl_display_dispatch_pending(w.display) < 0) return false;
  }
  if (wl_display_flush(w.display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(w.display);
    return false;
  }
  pollfd fds[2] = {{wl_display_get_fd(w.display), POLLIN, 0}, {extra_fd, POLLIN, 0}};
  const nfds_t count = extra_fd >= 0 ? 2 : 1;
  int ready = ::poll(fds, count, timeout_ms);
  if (ready < 0 && errno != EINTR) {
    wl_display_cancel_read(w.display);
    return false;
  }
  if (ready > 0 && (fds[0].revents & POLLIN)) {
    if (wl_display_read_events(w.display) < 0) return false;
  } else {
    wl_display_cancel_read(w.display);
    if (ready > 0 && (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
  }
  if (wl_display_dispatch_pending(w.display) < 0) return false;
  if (extra_ready && count == 2 && ready > 0 && (fds[1].revents & POLLIN)) *extra_ready = true;
  return true;
}

void wayland_window_linux::before_commit(std::uint32_t width, std::uint32_t height) noexcept {
  impl& w = *impl_;
  const surface_config c = w.config.acquire();
  // A newer configure of another size is coming in a later snapshot: commit
  // this buffer with the state that matches it, ack nothing yet.
  if (c.physical_w != width || c.physical_h != height) return;
  if (w.viewport) {
    if (c.logical_w != w.dest_w || c.logical_h != w.dest_h) {
      wp_viewport_set_destination(w.viewport, static_cast<std::int32_t>(c.logical_w),
                                  static_cast<std::int32_t>(c.logical_h));
      w.dest_w = c.logical_w;
      w.dest_h = c.logical_h;
    }
  } else if (c.buffer_scale != w.committed_scale) {
    wl_surface_set_buffer_scale(w.surface, c.buffer_scale);
    w.committed_scale = c.buffer_scale;
  }
  if (c.serial != 0 && c.serial != w.acked_serial) {
    xdg_surface_ack_configure(w.xdg, c.serial);
    w.acked_serial = c.serial;
  }
}

void wayland_window_linux::close() noexcept {
  impl& w = *impl_;
  if (w.keys) xkb_state_unref(w.keys);
  if (w.keymap) xkb_keymap_unref(w.keymap);
  if (w.xkb) xkb_context_unref(w.xkb);
  w.keys = nullptr;
  w.keymap = nullptr;
  w.xkb = nullptr;
  if (w.decoration) zxdg_toplevel_decoration_v1_destroy(w.decoration);
  if (w.toplevel) xdg_toplevel_destroy(w.toplevel);
  if (w.xdg) xdg_surface_destroy(w.xdg);
  if (w.fractional) wp_fractional_scale_v1_destroy(w.fractional);
  if (w.viewport) wp_viewport_destroy(w.viewport);
  if (w.surface) wl_surface_destroy(w.surface);
  w.decoration = nullptr;
  w.toplevel = nullptr;
  w.xdg = nullptr;
  w.fractional = nullptr;
  w.viewport = nullptr;
  w.surface = nullptr;
  if (w.pointer) wl_pointer_destroy(w.pointer);
  if (w.keyboard) wl_keyboard_destroy(w.keyboard);
  w.pointer = nullptr;
  w.keyboard = nullptr;
  for (auto& o : w.outputs) {
    if (o.proxy) wl_output_destroy(o.proxy);
    o = {};
  }
  if (w.seat) wl_seat_destroy(w.seat);
  if (w.decoration_manager) zxdg_decoration_manager_v1_destroy(w.decoration_manager);
  if (w.fractional_manager) wp_fractional_scale_manager_v1_destroy(w.fractional_manager);
  if (w.viewporter) wp_viewporter_destroy(w.viewporter);
  if (w.wm_base) xdg_wm_base_destroy(w.wm_base);
  if (w.compositor) wl_compositor_destroy(w.compositor);
  if (w.registry) wl_registry_destroy(w.registry);
  w.seat = nullptr;
  w.decoration_manager = nullptr;
  w.fractional_manager = nullptr;
  w.viewporter = nullptr;
  w.wm_base = nullptr;
  w.compositor = nullptr;
  w.registry = nullptr;
  if (w.display) {
    wl_display_flush(w.display);
    wl_display_disconnect(w.display);
  }
  w.display = nullptr;
  w.configured = false;
}

}  // namespace mv::shell
