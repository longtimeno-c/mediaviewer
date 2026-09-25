// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15: MediaViewerThumbs.dll, the Explorer thumbnail handler (plan/09
// "Windows integration"). An IThumbnailProvider over IInitializeWithStream.
//
// Out of process, never in Explorer: a handler that initialises from a
// stream is run by the shell in an isolated surrogate (dllhost.exe), and the
// class also declares its own DllSurrogate AppID (the app's registration,
// shell/shellext_install.cpp). A decoder that crashes on a malformed HEIC
// takes down that surrogate; Explorer keeps running and shows the generic
// icon (the PR 15 verify).
//
// The surrogate may be shared with other vendors' handlers, so nothing here
// changes the process: no job object, no DLL search path, no global state
// beyond the COM object count. Each request reads at most kMaxSourceBytes and
// answers within kDeadline or not at all (shellext/thumb_request.h).
//
// Registered on the MediaViewer.Image ProgId only (plan/12 2026-09-25), so it
// runs for the types the user made MediaViewer the default for and never
// replaces another handler.
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlwapi.h>
#include <thumbcache.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <vector>

#include "shellext/thumb_request.h"

namespace {

// {6A3F1B52-8C0E-4D7A-9B21-5E4C7D2F9A13}: the handler's CLSID. Also written by
// the app (shellext_install.cpp) and the wizard (mediaviewer.iss). Never change.
constexpr CLSID kClsid = {0x6a3f1b52, 0x8c0e, 0x4d7a, {0x9b, 0x21, 0x5e, 0x4c, 0x7d, 0x2f, 0x9a, 0x13}};

std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

class thumb_provider final : public IInitializeWithStream, public IThumbnailProvider {
 public:
  thumb_provider() noexcept { ++g_objects; }
  thumb_provider(const thumb_provider&) = delete;
  thumb_provider& operator=(const thumb_provider&) = delete;

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
      *out = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IThumbnailProvider) {
      *out = static_cast<IThumbnailProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const long n = --refs_;
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }

  // IInitializeWithStream: the bytes are read here, once, with the size cap.
  // The stream is not kept: the provider holds no handle to the user's file.
  IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
    if (!stream) return E_INVALIDARG;
    if (initialized_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    try {
      STATSTG stat{};
      if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return E_FAIL;
      if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > mv::shellext::kMaxSourceBytes) {
        return E_FAIL;
      }
      bytes_.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
      std::size_t got = 0;
      while (got < bytes_.size()) {
        const ULONG want = static_cast<ULONG>(std::min<std::size_t>(bytes_.size() - got, 1u << 24));
        ULONG read = 0;
        const HRESULT hr = stream->Read(bytes_.data() + got, want, &read);
        if (FAILED(hr) || read == 0) break;
        got += read;
      }
      if (got == 0) return E_FAIL;
      bytes_.resize(got);
    } catch (const std::bad_alloc&) {
      bytes_ = {};
      return E_OUTOFMEMORY;
    }
    initialized_ = true;
    return S_OK;
  }

  // IThumbnailProvider
  IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha) override {
    if (!out || !alpha) return E_POINTER;
    *out = nullptr;
    *alpha = WTSAT_UNKNOWN;
    if (!initialized_ || bytes_.empty()) return E_UNEXPECTED;
    // No path, name or pixels are logged, here or by the core (rule 6).
    auto thumb = mv::shellext::render_thumbnail_by(std::move(bytes_), cx);
    bytes_ = {};
    if (!thumb) return thumb.error() == mv::status::out_of_memory ? E_OUTOFMEMORY : E_FAIL;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(thumb->width);
    bi.bmiHeader.biHeight = -static_cast<LONG>(thumb->height);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
      if (bmp) ::DeleteObject(bmp);
      return E_OUTOFMEMORY;
    }
    std::memcpy(bits, thumb->bgra.data(), static_cast<std::size_t>(thumb->width) * thumb->height * 4u);
    *out = bmp;
    *alpha = thumb->has_alpha ? WTSAT_ARGB : WTSAT_RGB;
    return S_OK;
  }

 private:
  ~thumb_provider() { --g_objects; }

  std::atomic<long> refs_{1};
  bool initialized_ = false;
  std::vector<std::uint8_t> bytes_;
};

class class_factory final : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != IID_IUnknown && riid != IID_IClassFactory) return E_NOINTERFACE;
    *out = static_cast<IClassFactory*>(this);
    return S_OK;  // static lifetime: no count
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
  IFACEMETHODIMP_(ULONG) Release() override { return 1; }

  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* p = new (std::nothrow) thumb_provider();
    if (!p) return E_OUTOFMEMORY;
    const HRESULT hr = p->QueryInterface(riid, out);
    p->Release();
    return hr;
  }
  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) ++g_locks; else --g_locks;
    return S_OK;
  }
};

class_factory g_factory;

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, out);
}

STDAPI DllCanUnloadNow() {
  return g_objects.load() == 0 && g_locks.load() == 0 ? S_OK : S_FALSE;
}
