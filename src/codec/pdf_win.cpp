// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PDF on Windows through Windows.Data.Pdf (docs/plans/audio-and-documents.md
// §2.4): in the box since Windows 8.1, so no bundled PDF library and no Store
// pack (rule 7). Driven through the WinRT ABI with WRL on a decode worker: the
// page renders to an in-memory BMP, which the bundled BMP reader takes, so the
// pixels come out exactly as every other still's do.
#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <windows.data.pdf.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

#include "codec/card.h"
#include "codec/decode.h"

namespace mv::codec {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wf = ABI::Windows::Foundation;
namespace wpdf = ABI::Windows::Data::Pdf;
namespace wss = ABI::Windows::Storage::Streams;

// BitmapEncoder.BmpEncoderId (CLSID_WICBmpEncoder).
constexpr GUID kBmpEncoder = {0x69be8bb4, 0xd66d, 0x47c8,
                              {0x86, 0x5a, 0xed, 0x15, 0x89, 0x43, 0x37, 0x82}};

// How long one page may take before it is treated as broken. A hostile file
// can make the renderer spin; the worker gives up rather than sit forever.
constexpr ULONGLONG kRenderTimeoutMs = 30000;

// Balanced per-call COM init on the worker, as os_decode_win.cpp does. A
// multithreaded apartment is enough for WinRT activation.
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

// IAsyncInfo::get_Status's own parameter type, without naming the enum's
// namespace (it moved between SDK headers over the years).
template <class T>
struct status_arg;
template <class C, class A>
struct status_arg<HRESULT (STDMETHODCALLTYPE C::*)(A*)> {
  using type = A;
};
using async_status = status_arg<decltype(&wf::IAsyncInfo::get_Status)>::type;

enum class wait_result { done, wrong_password, failed, cancelled };

// Polls the operation on this worker. WinRT's completion callbacks would need
// an agile handler for every operation type; a worker has nothing better to
// do than wait, and polling leaves cancellation in our hands.
wait_result wait_for(IUnknown* operation, const job_context* ctx) noexcept {
  ComPtr<wf::IAsyncInfo> info;
  if (FAILED(operation->QueryInterface(IID_PPV_ARGS(&info)))) return wait_result::failed;
  const ULONGLONG deadline = GetTickCount64() + kRenderTimeoutMs;
  for (;;) {
    async_status st{};
    if (FAILED(info->get_Status(&st))) return wait_result::failed;
    const int value = static_cast<int>(st);  // Started 0, Completed 1, Canceled 2, Error 3
    if (value == 1) return wait_result::done;
    if (value == 2) return wait_result::cancelled;
    if (value == 3) {
      HRESULT code = S_OK;
      (void)info->get_ErrorCode(&code);
      return code == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) ? wait_result::wrong_password
                                                               : wait_result::failed;
    }
    if (ctx && ctx->cancelled()) {
      (void)info->Cancel();
      return wait_result::cancelled;
    }
    if (GetTickCount64() > deadline) {
      (void)info->Cancel();
      return wait_result::failed;
    }
    Sleep(1);
  }
}

status from_wait(wait_result w) noexcept {
  switch (w) {
    case wait_result::done: return status::ok;
    case wait_result::cancelled: return status::cancelled;
    case wait_result::wrong_password:
    case wait_result::failed: break;
  }
  return status::corrupt;
}

raster locked_page() {
  const card c = make_card(card_kind::locked, 1200, 1600);
  raster out;
  out.format = format_family::pdf;
  out.width = c.width;
  out.height = c.height;
  out.rgba = c.rgba;
  out.tagged_srgb = true;
  return out;
}

// One page of a loaded document at `long_edge`, white behind it.
result<raster> render_page(wpdf::IPdfDocument* document, std::uint32_t page, UINT32 count,
                           std::uint32_t long_edge, const job_context* ctx) {
  ComPtr<wpdf::IPdfPage> pdf_page;
  if (FAILED(document->GetPage(page, &pdf_page)) || !pdf_page) return err(status::corrupt);

  // Size is the crop box with the page's rotation applied, in DIPs.
  wf::Size size{};
  if (FAILED(pdf_page->get_Size(&size)) || !(size.Width > 0.5f) || !(size.Height > 0.5f)) {
    return err(status::corrupt);
  }
  const double scale = static_cast<double>(long_edge) /
                       static_cast<double>(std::max(size.Width, size.Height));
  const auto width = static_cast<UINT32>(std::max(1.0, std::round(size.Width * scale)));
  const auto height = static_cast<UINT32>(std::max(1.0, std::round(size.Height * scale)));

  ComPtr<IInspectable> made;
  if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions).Get(),
                                &made))) {
    return err(status::internal);
  }
  ComPtr<wpdf::IPdfPageRenderOptions> options;
  if (FAILED(made.As(&options))) return err(status::internal);
  (void)options->put_DestinationWidth(width);
  (void)options->put_DestinationHeight(height);
  (void)options->put_BitmapEncoderId(kBmpEncoder);  // white background is the default

  ComPtr<IInspectable> out_made;
  if (FAILED(RoActivateInstance(
          HStringReference(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream).Get(),
          &out_made))) {
    return err(status::internal);
  }
  ComPtr<wss::IRandomAccessStream> output;
  if (FAILED(out_made.As(&output))) return err(status::internal);

  ComPtr<wf::IAsyncAction> rendering;
  if (FAILED(pdf_page->RenderWithOptionsToStreamAsync(output.Get(), options.Get(), &rendering))) {
    return err(status::corrupt);
  }
  const wait_result rendered = wait_for(rendering.Get(), ctx);
  if (rendered != wait_result::done) return err(from_wait(rendered));

  UINT64 encoded_size = 0;
  if (FAILED(output->get_Size(&encoded_size)) || encoded_size == 0 || encoded_size > 0x7fffffffu) {
    return err(status::corrupt);
  }
  ComPtr<IStream> readback;
  if (FAILED(CreateStreamOverRandomAccessStream(output.Get(), IID_PPV_ARGS(&readback)))) {
    return err(status::internal);
  }
  LARGE_INTEGER zero{};
  if (FAILED(readback->Seek(zero, STREAM_SEEK_SET, nullptr))) return err(status::internal);
  std::vector<std::uint8_t> bmp;
  try {
    bmp.resize(static_cast<std::size_t>(encoded_size));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  std::size_t have = 0;
  while (have < bmp.size()) {
    ULONG got = 0;
    const ULONG want = static_cast<ULONG>(std::min<std::size_t>(bmp.size() - have, 1u << 24));
    if (FAILED(readback->Read(bmp.data() + have, want, &got)) || got == 0) break;
    have += got;
  }
  if (have != bmp.size()) return err(status::corrupt);
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  auto decoded = decode_bmp(bmp, ctx);
  if (!decoded) return err(decoded.error() == status::cancelled ? status::cancelled : status::corrupt);
  raster out = std::move(decoded).value();
  out.format = format_family::pdf;
  out.tagged_srgb = true;
  out.page = page;
  out.page_count = count;
  return out;
}

// The document over a copy of `bytes`. `locked` when it needs a password,
// which is shown as the locked card, never asked for.
struct pdf_doc {
  ComPtr<wpdf::IPdfDocument> document;
  bool locked = false;
  UINT32 count = 0;
};

status load_doc(std::span<const std::uint8_t> bytes, const job_context* ctx, pdf_doc& out) {
  // The renderer reads a stream; SHCreateMemStream copies the bytes into one.
  ComPtr<IStream> memory;
  memory.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
  if (!memory) return status::out_of_memory;
  ComPtr<wss::IRandomAccessStream> input;
  if (FAILED(CreateRandomAccessStreamOverStream(memory.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&input)))) {
    return status::internal;
  }

  ComPtr<wpdf::IPdfDocumentStatics> statics;
  if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
                                    IID_PPV_ARGS(&statics)))) {
    return status::internal;
  }
  ComPtr<wf::IAsyncOperation<wpdf::PdfDocument*>> loading;
  if (FAILED(statics->LoadFromStreamAsync(input.Get(), &loading))) return status::corrupt;
  const wait_result loaded = wait_for(loading.Get(), ctx);
  if (loaded == wait_result::wrong_password) {
    out.locked = true;
    out.count = 1;
    return status::ok;
  }
  if (loaded != wait_result::done) return from_wait(loaded);
  if (FAILED(loading->GetResults(&out.document)) || !out.document) return status::corrupt;
  if (FAILED(out.document->get_PageCount(&out.count)) || out.count == 0) return status::corrupt;
  return status::ok;
}

}  // namespace

result<raster> decode_pdf(std::span<const std::uint8_t> bytes, std::uint32_t page,
                          const job_context* ctx, std::uint32_t long_edge) {
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kPdfLongEdge);
  if (probe(bytes) != format_family::pdf) return err(status::unsupported_format);
  if (bytes.size() > 0x7fffffffu) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const com_scope com;
  if (!com.usable()) return err(status::internal);
  pdf_doc doc;
  if (const status s = load_doc(bytes, ctx, doc); s != status::ok) return err(s);
  if (page >= doc.count) return err(status::invalid_arg);
  if (doc.locked) return locked_page();
  return render_page(doc.document.Get(), page, doc.count, long_edge, ctx);
}

result<std::uint32_t> decode_pdf_pages(std::span<const std::uint8_t> bytes,
                                       std::span<const std::uint32_t> pages,
                                       std::uint32_t long_edge, const job_context* ctx,
                                       void* user, page_sink sink) {
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kPdfLongEdge);
  if (probe(bytes) != format_family::pdf) return err(status::unsupported_format);
  if (bytes.size() > 0x7fffffffu) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const com_scope com;
  if (!com.usable()) return err(status::internal);
  pdf_doc doc;
  if (const status s = load_doc(bytes, ctx, doc); s != status::ok) return err(s);
  for (const std::uint32_t page : pages) {
    if (page >= doc.count) continue;
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    auto one = doc.locked ? result<raster>(locked_page())
                          : render_page(doc.document.Get(), page, doc.count, long_edge, ctx);
    if (!one) {
      if (one.error() == status::cancelled || one.error() == status::out_of_memory) {
        return err(one.error());
      }
      continue;  // a broken page leaves its thumbnail empty, not the others
    }
    if (!sink(user, std::move(one).value())) break;
  }
  return std::uint32_t{doc.count};
}

}  // namespace mv::codec
