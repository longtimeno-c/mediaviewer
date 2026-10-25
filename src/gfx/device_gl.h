// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenGL device for the Linux present lab (Linux PR 1): an EGL display on
// the host's Wayland connection, desktop GL 3.3 core (whatever newer core the
// driver gives), and the one ARGB8888 config every surface and later every
// worker's shared context uses (docs/plans/linux.md §2).
//
// No EGL, GL or Wayland type in this header (D9): handles are void*, as in
// gfx/device_mac.h. The render thread owns the device and is the only thread
// that makes its context current.
#pragma once

#include "core/result.h"

namespace mv::gfx {

struct gl_device_info {
  char vendor[64] = {};
  char renderer[128] = {};
  char version[96] = {};
  // The DRM render node EGL renders on (EGL_EXT_device_query), "" when the
  // driver has none (llvmpipe) or does not say. Shown in F3, never logged.
  char drm_node[64] = {};
  bool srgb_colorspace = false;  // EGL_KHR_gl_colorspace: an sRGB window surface can exist
  bool surfaceless = false;      // EGL_KHR_surfaceless_context (worker contexts, Linux PR 2)
  bool robustness = false;       // the context reports a GPU reset (device loss)
};

class gl_device {
 public:
  gl_device() = default;
  ~gl_device();

  gl_device(const gl_device&) = delete;
  gl_device& operator=(const gl_device&) = delete;

  // `wl_display` is the host's wl_display*, borrowed for the device's life.
  // Fails with unsupported_format when the driver cannot give an sRGB window
  // surface: presenting linear values as sRGB is not a fallback (D6).
  [[nodiscard]] expected create(void* wl_display) noexcept;
  void destroy() noexcept;
  [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

  // EGLDisplay / EGLConfig / EGLContext, borrowed. Valid until destroy().
  [[nodiscard]] void* egl_display() const noexcept { return display_; }
  [[nodiscard]] void* egl_config() const noexcept { return config_; }
  [[nodiscard]] void* egl_context() const noexcept { return context_; }

  // [render thread, context current] Reads GL_VENDOR / RENDERER / VERSION.
  void query_gl_info() noexcept;
  [[nodiscard]] const gl_device_info& info() const noexcept { return info_; }

  // [render thread, context current] True once the driver says the context
  // was lost to a GPU reset (EGL_EXT_create_context_robustness with
  // LOSE_CONTEXT_ON_RESET). The lab then rebuilds, as Windows'
  // rebuild_device() does. Always false without robustness.
  [[nodiscard]] bool reset_detected() noexcept;

 private:
  void* display_ = nullptr;
  void* config_ = nullptr;
  void* context_ = nullptr;
  void* reset_status_fn_ = nullptr;  // glGetGraphicsResetStatus, when robust
  gl_device_info info_{};
};

}  // namespace mv::gfx
