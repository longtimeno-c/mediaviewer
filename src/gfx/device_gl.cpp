// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/device_gl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstring>

#include "core/trace.h"

namespace mv::gfx {

namespace {

bool has_extension(const char* list, const char* name) noexcept {
  if (!list || !name) return false;
  const std::size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n) {
    const bool starts = p == list || p[-1] == ' ';
    const bool ends = p[n] == ' ' || p[n] == '\0';
    if (starts && ends) return true;
  }
  return false;
}

void copy_string(char* out, std::size_t cap, const char* in) noexcept {
  if (cap == 0) return;
  std::snprintf(out, cap, "%s", in ? in : "");
}

// ARGB8888 with alpha 8, not XRGB8888: Mesa refuses EGL_GL_COLORSPACE_SRGB on
// a config without alpha (EGL_BAD_MATCH, measured on llvmpipe). The lab writes
// alpha 1 and marks the surface opaque, so the compositor never blends it.
EGLConfig choose_config(EGLDisplay display) noexcept {
  const EGLint attributes[] = {EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                               EGL_CONFORMANT,      EGL_OPENGL_BIT,
                               EGL_RED_SIZE,        8,
                               EGL_GREEN_SIZE,      8,
                               EGL_BLUE_SIZE,       8,
                               EGL_ALPHA_SIZE,      8,
                               EGL_NONE};
  EGLConfig configs[64];
  EGLint count = 0;
  if (!eglChooseConfig(display, attributes, configs, 64, &count) || count <= 0) return nullptr;
  // eglChooseConfig sorts deeper formats first; the swapchain is 8-bit sRGB
  // (D6), so take an exact 8:8:8:8.
  for (EGLint i = 0; i < count; ++i) {
    EGLint r = 0, g = 0, b = 0, a = 0;
    eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
    eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
    if (r == 8 && g == 8 && b == 8 && a == 8) return configs[i];
  }
  return nullptr;
}

EGLContext create_context(EGLDisplay display, EGLConfig config, bool robust) noexcept {
  // 3.3 core is the floor the GLSL twin is written for (docs/plans/linux.md
  // §6); drivers hand back their newest compatible core version.
  const EGLint robust_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                      3,
                                      EGL_CONTEXT_MINOR_VERSION,
                                      3,
                                      EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                      EGL_CONTEXT_OPENGL_ROBUST_ACCESS,
                                      EGL_TRUE,
                                      EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY,
                                      EGL_LOSE_CONTEXT_ON_RESET,
                                      EGL_NONE};
  const EGLint plain_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                     3,
                                     EGL_CONTEXT_MINOR_VERSION,
                                     3,
                                     EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                     EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                     EGL_NONE};
  return eglCreateContext(display, config, EGL_NO_CONTEXT,
                          robust ? robust_attributes : plain_attributes);
}

void query_drm_node(EGLDisplay display, char* out, std::size_t cap) noexcept {
  const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!has_extension(client, "EGL_EXT_device_query")) return;
  const auto query_display = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDisplayAttribEXT"));
  const auto query_string = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
      eglGetProcAddress("eglQueryDeviceStringEXT"));
  if (!query_display || !query_string) return;
  EGLAttrib device = 0;
  if (!query_display(display, EGL_DEVICE_EXT, &device) || device == 0) return;
  const auto egl_device = reinterpret_cast<EGLDeviceEXT>(device);
  const char* device_extensions = query_string(egl_device, EGL_EXTENSIONS);
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
  if (has_extension(device_extensions, "EGL_EXT_device_drm_render_node")) {
    copy_string(out, cap, query_string(egl_device, EGL_DRM_RENDER_NODE_FILE_EXT));
    if (out[0] != '\0') return;
  }
#endif
  if (has_extension(device_extensions, "EGL_EXT_device_drm")) {
    copy_string(out, cap, query_string(egl_device, EGL_DRM_DEVICE_FILE_EXT));
  }
}

}  // namespace

gl_device::~gl_device() { destroy(); }

expected gl_device::create(void* wl_display) noexcept {
  if (!wl_display) return err(status::invalid_arg);
  destroy();

  EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, nullptr);
  if (display == EGL_NO_DISPLAY) {
    MV_LOG_ERROR("gl_device: no EGL display for the Wayland connection");
    return err(status::unsupported_format);
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(display, &major, &minor)) {
    MV_LOG_ERROR("gl_device: eglInitialize failed (0x%x)", static_cast<unsigned>(eglGetError()));
    return err(status::unsupported_format);
  }
  display_ = display;

  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  info_ = {};
  // EGL 1.5 has EGL_GL_COLORSPACE in core; the extension is what says a
  // driver honours it on window surfaces.
  info_.srgb_colorspace = has_extension(extensions, "EGL_KHR_gl_colorspace");
  info_.surfaceless = has_extension(extensions, "EGL_KHR_surfaceless_context");
  const bool robust_available =
      (major > 1 || (major == 1 && minor >= 5)) ||
      has_extension(extensions, "EGL_EXT_create_context_robustness");
  if (!info_.srgb_colorspace) {
    MV_LOG_ERROR("gl_device: the EGL driver offers no sRGB window surface "
                 "(EGL_KHR_gl_colorspace); refusing to present linear values as sRGB (D6)");
    destroy();
    return err(status::unsupported_format);
  }
  if (!eglBindAPI(EGL_OPENGL_API)) {
    MV_LOG_ERROR("gl_device: desktop OpenGL is not available through EGL");
    destroy();
    return err(status::unsupported_format);
  }
  EGLConfig config = choose_config(display);
  if (!config) {
    MV_LOG_ERROR("gl_device: no 8:8:8:8 OpenGL window config");
    destroy();
    return err(status::unsupported_format);
  }
  config_ = config;

  EGLContext context = robust_available ? create_context(display, config, true) : EGL_NO_CONTEXT;
  info_.robustness = context != EGL_NO_CONTEXT;
  if (context == EGL_NO_CONTEXT) context = create_context(display, config, false);
  if (context == EGL_NO_CONTEXT) {
    MV_LOG_ERROR("gl_device: no OpenGL 3.3 core context (0x%x)",
                 static_cast<unsigned>(eglGetError()));
    destroy();
    return err(status::unsupported_format);
  }
  context_ = context;
  if (info_.robustness) {
    reset_status_fn_ = reinterpret_cast<void*>(eglGetProcAddress("glGetGraphicsResetStatus"));
    if (!reset_status_fn_) {
      reset_status_fn_ = reinterpret_cast<void*>(eglGetProcAddress("glGetGraphicsResetStatusARB"));
    }
  }
  query_drm_node(display, info_.drm_node, sizeof(info_.drm_node));
  return {};
}

void gl_device::query_gl_info() noexcept {
  copy_string(info_.vendor, sizeof(info_.vendor),
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
  copy_string(info_.renderer, sizeof(info_.renderer),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
  copy_string(info_.version, sizeof(info_.version),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
}

bool gl_device::reset_detected() noexcept {
  if (!reset_status_fn_) return false;
  using reset_status = GLenum (*)();
  return reinterpret_cast<reset_status>(reset_status_fn_)() != GL_NO_ERROR;
}

void gl_device::destroy() noexcept {
  auto display = static_cast<EGLDisplay>(display_);
  if (display != EGL_NO_DISPLAY && display != nullptr) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_) eglDestroyContext(display, static_cast<EGLContext>(context_));
    eglTerminate(display);
    eglReleaseThread();
  }
  display_ = nullptr;
  config_ = nullptr;
  context_ = nullptr;
  reset_status_fn_ = nullptr;
}

}  // namespace mv::gfx
