// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The canvas's EGL window surface on a borrowed wl_surface (Linux PR 1): the
// twin of gfx/metal_layer.h. 8-bit sRGB (D6): EGL_GL_COLORSPACE_SRGB on the
// device's ARGB8888 config, GL_FRAMEBUFFER_SRGB on, alpha written as 1 and the
// surface marked opaque. eglSwapInterval(0): the swap never blocks; the wait
// before recording is gfx/wayland_frame_clock's (docs/design/03).
//
// The surface is borrowed so Linux PR 3 can pass Qt's child surface instead of
// the lab's toplevel. No EGL, GL or Wayland type in this header (D9).
#pragma once

#include <cstdint>

#include "core/result.h"
#include "gfx/device_gl.h"

namespace mv::gfx {

class gl_surface_wayland {
 public:
  gl_surface_wayland() = default;
  ~gl_surface_wayland();

  gl_surface_wayland(const gl_surface_wayland&) = delete;
  gl_surface_wayland& operator=(const gl_surface_wayland&) = delete;

  // [render thread] Creates the wl_egl_window and the sRGB EGL surface at
  // `width` x `height` buffer pixels and makes the device's context current
  // on it. unsupported_format when the surface is not sRGB-encoded: the lab
  // refuses to run rather than present linear values as sRGB (D6).
  [[nodiscard]] expected attach(void* wl_display, void* wl_surface, gl_device& device,
                                std::uint32_t width, std::uint32_t height) noexcept;
  void destroy() noexcept;
  [[nodiscard]] bool valid() const noexcept { return egl_surface_ != nullptr; }

  // [render thread] Before the frame is recorded: the next swap's buffer is
  // `width` x `height`. Never re-reads the refresh (issue #162).
  [[nodiscard]] expected resize(std::uint32_t width, std::uint32_t height) noexcept;
  // [render thread] eglSwapBuffers: attaches and commits the buffer.
  // device_lost when the context or surface is gone.
  [[nodiscard]] expected present() noexcept;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  gl_device* device_ = nullptr;
  void* wl_surface_ = nullptr;   // borrowed
  void* egl_window_ = nullptr;   // wl_egl_window*
  void* egl_surface_ = nullptr;  // EGLSurface
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
};

}  // namespace mv::gfx
