// SPDX-License-Identifier: GPL-2.0-or-later
// Windows OS-codec probe (D3): WIC's HEIF decoder for HEIC stills that it can
// show identically to libheif, libheif otherwise. *_win.cpp is the D9 port
// (plan/15). The policy is written down in codec/os_decode.h.
#include "codec/os_decode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace mv::codec {
namespace {

template <class T>
class com {
 public:
  com() = default;
  ~com() { reset(); }
  com(const com&) = delete;
  com& operator=(const com&) = delete;
  T** put() noexcept {
    reset();
    return &p_;
  }
  T* get() const noexcept { return p_; }
  T* operator->() const noexcept { return p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }
  void reset() noexcept {
    if (p_) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

// Balanced per-call COM init on the worker. A thread already in an STA
// (RPC_E_CHANGED_MODE) is used as it is and not uninitialised.
class com_scope {
 public:
  com_scope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~com_scope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  com_scope(const com_scope&) = delete;
  com_scope& operator=(const com_scope&) = delete;
  [[nodiscard]] bool usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_;
};

// Is an HEVC video decoder MFT registered? WIC's HEIF decoder needs one (the
// Store HEVC Video Extension, or an OEM equivalent). mfplat is loaded
// dynamically so the codec library takes no link dependency for a probe.
bool hevc_mft_present() noexcept {
  HMODULE mf = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!mf) return false;
  using enum_fn = HRESULT(WINAPI*)(GUID, UINT32, const void*, const void*, void***, UINT32*);
  const auto enum_ex = reinterpret_cast<enum_fn>(
      reinterpret_cast<void*>(GetProcAddress(mf, "MFTEnumEx")));
  bool found = false;
  if (enum_ex) {
    // MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, MFVideoFormat_HEVC.
    constexpr GUID kVideoDecoder = {0xd6c02d4b, 0x6833, 0x45b4,
                                    {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
    constexpr GUID kVideo = {0x73646976, 0x0000, 0x0010,
                             {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    constexpr GUID kHevc = {0x43564548, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    struct register_type_info {
      GUID major;
      GUID sub;
    } const input{kVideo, kHevc};
    constexpr UINT32 kFlags = 0x00000001 | 0x00000002 | 0x00000004 | 0x00000040;  // sync|async|hw|sort
    void** activates = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(enum_ex(kVideoDecoder, kFlags, &input, nullptr, &activates, &count)) &&
        activates) {
      found = count > 0;
      for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) static_cast<IUnknown*>(activates[i])->Release();
      }
      CoTaskMemFree(static_cast<void*>(activates));
    }
  }
  FreeLibrary(mf);
  return found;
}

// Codec packs are not installed mid-session in practice; enumerate once per
// process instead of loading mfplat for every HEIC.
bool hevc_mft_present_cached() noexcept {
  static std::once_flag once;
  static bool present = false;
  std::call_once(once, [] { present = hevc_mft_present(); });
  return present;
}

// Anything that is not a clean success here means "let libheif decide".
constexpr status kFallThrough = status::unsupported_format;

}  // namespace

bool os_codec_enabled() noexcept {
  wchar_t value[8]{};
  const DWORD n = GetEnvironmentVariableW(L"MV_OS_CODEC", value, 8);
  return !(n == 1 && value[0] == L'0');
}

result<raster> try_os_decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  if (!os_codec_enabled()) return err(kFallThrough);
  if (probe(bytes) != format_family::heic) return err(kFallThrough);
  if (bytes.size() > 0xFFFFFFFFull) return err(kFallThrough);

  // Only files whose WIC rendering is the same as libheif's (os_decode.h).
  auto inspected = inspect_heic(bytes);
  if (!inspected) return err(kFallThrough);
  const heic_still_info& info = inspected.value();
  if (!info.hevc || info.sequence || info.hdr || info.has_alpha || info.luma_bits != 8 ||
      (!info.has_icc && !info.srgb_in_effect)) {
    return err(kFallThrough);
  }

  try {
    com_scope scope;
    if (!scope.usable()) return err(kFallThrough);
    if (!hevc_mft_present_cached()) return err(kFallThrough);

    com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put())))) {
      return err(kFallThrough);
    }
    com<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size())))) {
      return err(kFallThrough);
    }
    com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return err(kFallThrough);  // no HEIF decoder registered
    }
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatHeif) {
      return err(kFallThrough);
    }
    com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return err(kFallThrough);
    UINT w = 0, h = 0;
    // WIC's HEIF decoder applies irot/imir itself, like libheif: the size must
    // match the container's displayed size, or the two paths disagree.
    if (FAILED(frame->GetSize(&w, &h)) || w != info.width || h != info.height) {
      return err(kFallThrough);
    }

    raster out;
    out.width = w;
    out.height = h;
    out.format = format_family::heic;
    out.intent = transfer_intent::display_referred;

    if (info.has_icc) {
      // The profile must come back through WIC, or WIC's output is untagged
      // bytes of a tagged file (D6). No profile → bundled.
      UINT count = 0;
      if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) {
        return err(kFallThrough);
      }
      std::vector<IWICColorContext*> contexts(count, nullptr);
      std::vector<com<IWICColorContext>> owned(count);
      for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(owned[i].put()))) return err(kFallThrough);
        contexts[i] = owned[i].get();
      }
      UINT got = 0;
      if (FAILED(frame->GetColorContexts(count, contexts.data(), &got))) return err(kFallThrough);
      for (UINT i = 0; i < got && out.icc.empty(); ++i) {
        WICColorContextType type{};
        if (FAILED(contexts[i]->GetType(&type)) || type != WICColorContextProfile) continue;
        UINT size = 0;
        if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &size)) || size == 0) continue;
        std::vector<std::uint8_t> icc(size);
        if (SUCCEEDED(contexts[i]->GetProfileBytes(size, icc.data(), &size))) {
          icc.resize(size);
          out.icc = std::move(icc);
        }
      }
      if (out.icc.empty()) return err(kFallThrough);
    }

    if (ctx && ctx->cancelled()) return err(status::cancelled);
    com<IWICBitmapSource> rgba;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.get(), rgba.put()))) {
      return err(kFallThrough);
    }
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    const WICRect all{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    // The HEVC decode happens here: failure without the codec pack lands in
    // this branch and is silent.
    if (FAILED(rgba->CopyPixels(&all, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) {
      return err(kFallThrough);
    }
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    return out;
  } catch (const std::bad_alloc&) {
    return err(kFallThrough);
  }
}

}  // namespace mv::codec
