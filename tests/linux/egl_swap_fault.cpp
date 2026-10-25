// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An LD_PRELOAD module for tools/linux/lab_smoke.sh: eglSwapBuffers number
// MV_FAIL_SWAP_AT fails once with EGL_BAD_SURFACE, the way a surface lost to a
// GPU reset does, and every other swap goes through to the real one. The
// smoke checks that the lab rebuilds and goes on presenting.

#include <EGL/egl.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

std::atomic<long> g_swaps{0};
std::atomic<bool> g_error_owed{false};

long fail_at() noexcept {
  const char* at = std::getenv("MV_FAIL_SWAP_AT");
  return at ? std::strtol(at, nullptr, 10) : 0;
}

template <class Fn>
Fn next(const char* name) noexcept {
  return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

}  // namespace

extern "C" {

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  static const auto real = next<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglSwapBuffers");
  const long n = ++g_swaps;
  if (n == fail_at()) {
    std::fprintf(stderr, "egl_swap_fault: failing swap %ld with EGL_BAD_SURFACE\n", n);
    g_error_owed.store(true);
    return EGL_FALSE;
  }
  return real ? real(display, surface) : EGL_FALSE;
}

EGLint eglGetError(void) {
  static const auto real = next<EGLint (*)(void)>("eglGetError");
  if (g_error_owed.exchange(false)) return EGL_BAD_SURFACE;
  return real ? real() : EGL_SUCCESS;
}

}  // extern "C"
