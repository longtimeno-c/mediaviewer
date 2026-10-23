// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/gl_surface_wayland.h"

// GL 3.0 entry points straight from glvnd's libOpenGL (OpenGL::OpenGL).
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <climits>
#include <cstring>

#include "core/trace.h"

namespace mv::gfx {

namespace {

struct compositor_probe {
  wl_compositor* compositor = nullptr;
};

void probe_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                  std::uint32_t) {
  auto* probe = static_cast<compositor_probe*>(data);
  if (probe->compositor || std::strcmp(interface, wl_compositor_interface.name) != 0) return;
  probe->compositor =
      static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
}
void probe_global_remove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener k_probe_listener = {probe_global, probe_global_remove};

// The whole surface is opaque, so the compositor never blends what is under
// it. Region extents past the surface are clipped by the compositor, so one
// region covers every size and scale. Double-buffered state: it applies at
// the first swap and stays. Done here, on a queue of its own, so the surface
// needs nothing from whoever owns it (the lab's toplevel, Qt's child surface).
void mark_opaque(wl_display* display, wl_surface* surface) noexcept {
  wl_event_queue* queue = wl_display_create_queue(display);
  if (!queue) return;
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  if (!wrapper) {
    wl_event_queue_destroy(queue);
    return;
  }
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  compositor_probe probe;
  wl_registry* registry = wl_display_get_registry(wrapper);
  if (registry) {
    wl_registry_add_listener(registry, &k_probe_listener, &probe);
    (void)wl_display_roundtrip_queue(display, queue);
  }
  if (probe.compositor) {
    if (wl_region* region = wl_compositor_create_region(probe.compositor)) {
      wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
      wl_surface_set_opaque_region(surface, region);
      wl_region_destroy(region);
    }
    wl_compositor_destroy(probe.compositor);
  }
  if (registry) wl_registry_destroy(registry);
  wl_proxy_wrapper_destroy(wrapper);
  wl_event_queue_destroy(queue);
}

}  // namespace

gl_surface_wayland::~gl_surface_wayland() { destroy(); }

expected gl_surface_wayland::attach(void* wl_display_ptr, void* wl_surface_ptr, gl_device& device,
                                    std::uint32_t width, std::uint32_t height) noexcept {
  if (!wl_display_ptr || !wl_surface_ptr || !device.valid() || width == 0 || height == 0)
    return err(status::invalid_arg);
  destroy();
  device_ = &device;
  wl_surface_ = wl_surface_ptr;
  auto display = static_cast<EGLDisplay>(device.egl_display());

  wl_egl_window* window = wl_egl_window_create(static_cast<wl_surface*>(wl_surface_ptr),
                                               static_cast<int>(width), static_cast<int>(height));
  if (!window) return err(status::out_of_memory);
  egl_window_ = window;

  const EGLAttrib attributes[] = {EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_SRGB, EGL_NONE};
  EGLSurface surface = eglCreatePlatformWindowSurface(
      display, static_cast<EGLConfig>(device.egl_config()), window, attributes);
  if (surface == EGL_NO_SURFACE) {
    MV_LOG_ERROR("gl_surface_wayland: no sRGB window surface (0x%x); refusing to present "
                 "linear values as sRGB (D6)",
                 static_cast<unsigned>(eglGetError()));
    destroy();
    return err(status::unsupported_format);
  }
  egl_surface_ = surface;
  if (!eglMakeCurrent(display, surface, surface, static_cast<EGLContext>(device.egl_context()))) {
    MV_LOG_ERROR("gl_surface_wayland: eglMakeCurrent failed (0x%x)",
                 static_cast<unsigned>(eglGetError()));
    destroy();
    return err(status::internal);
  }
  // Interval 0: Mesa and NVIDIA's egl-wayland block inside eglSwapBuffers on
  // the frame callback at interval 1, after the frame is recorded. The lab
  // waits for the callback before recording instead (docs/design/12, 2026-10-23).
  if (!eglSwapInterval(display, 0)) {
    MV_LOG_WARN("gl_surface_wayland: eglSwapInterval(0) refused (0x%x)",
                static_cast<unsigned>(eglGetError()));
  }

  // D6: both sides must say sRGB, the surface and the default framebuffer GL
  // writes through. A driver that accepted the attribute and ignored it would
  // otherwise show every linear value too dark.
  EGLint colorspace = 0;
  GLint encoding = 0;
  const bool egl_srgb =
      eglQuerySurface(display, surface, EGL_GL_COLORSPACE, &colorspace) &&
      colorspace == EGL_GL_COLORSPACE_SRGB;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT,
                                        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
  if (!egl_srgb || encoding != GL_SRGB) {
    MV_LOG_ERROR("gl_surface_wayland: the window surface is not sRGB-encoded (EGL %s, GL 0x%x); "
                 "refusing to present linear values as sRGB (D6)",
                 egl_srgb ? "sRGB" : "linear", static_cast<unsigned>(encoding));
    destroy();
    return err(status::unsupported_format);
  }
  glEnable(GL_FRAMEBUFFER_SRGB);

  mark_opaque(static_cast<wl_display*>(wl_display_ptr), static_cast<wl_surface*>(wl_surface_ptr));
  width_ = width;
  height_ = height;
  return {};
}

expected gl_surface_wayland::resize(std::uint32_t width, std::uint32_t height) noexcept {
  if (!egl_window_) return err(status::invalid_arg);
  if (width == 0 || height == 0) return err(status::invalid_arg);
  if (width == width_ && height == height_) return {};
  wl_egl_window_resize(static_cast<wl_egl_window*>(egl_window_), static_cast<int>(width),
                       static_cast<int>(height), 0, 0);
  width_ = width;
  height_ = height;
  return {};
}

expected gl_surface_wayland::present() noexcept {
  if (!egl_surface_ || !device_) return err(status::invalid_arg);
  if (eglSwapBuffers(static_cast<EGLDisplay>(device_->egl_display()),
                     static_cast<EGLSurface>(egl_surface_)))
    return {};
  const EGLint error = eglGetError();
  MV_LOG_ERROR("gl_surface_wayland: eglSwapBuffers failed (0x%x)", static_cast<unsigned>(error));
  return err(error == EGL_CONTEXT_LOST || error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW
                 ? status::device_lost
                 : status::internal);
}

void gl_surface_wayland::destroy() noexcept {
  if (egl_surface_ && device_ && device_->valid()) {
    auto display = static_cast<EGLDisplay>(device_->egl_display());
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, static_cast<EGLSurface>(egl_surface_));
  }
  egl_surface_ = nullptr;
  if (egl_window_) wl_egl_window_destroy(static_cast<wl_egl_window*>(egl_window_));
  egl_window_ = nullptr;
  device_ = nullptr;
  wl_surface_ = nullptr;
  width_ = 0;
  height_ = 0;
}

}  // namespace mv::gfx
