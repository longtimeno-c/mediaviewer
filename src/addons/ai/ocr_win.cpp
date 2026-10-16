// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// text_reader.h on Windows: Windows.Media.Ocr (in-box since Windows 10), the
// recogniser for the user's profile languages, through C++/WinRT from the
// Windows SDK (no NuGet package, no new library: windowsapp.lib is the OS's
// umbrella). In-process on the calling worker, an MTA thread; nothing is sent
// anywhere. C++/WinRT reports failure by throwing hresult_error, so every call
// here is inside one try: nothing crosses into the engine as an exception.
#include <windows.h>
#include <roapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

#include "addons/ai/text_reader.h"

#pragma comment(lib, "windowsapp.lib")

namespace mv::ai {
namespace {

namespace wg = winrt::Windows::Graphics::Imaging;
namespace wo = winrt::Windows::Media::Ocr;
namespace ws = winrt::Windows::Storage::Streams;

// The worker's apartment: the engine's threads join the MTA once each. A
// thread some other code put in an STA keeps it (RPC_E_CHANGED_MODE), and
// RecognizeAsync().get() would then refuse: read() says io.
bool join_apartment() noexcept {
  thread_local const bool joined = [] {
    const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    return SUCCEEDED(hr);
  }();
  return joined;
}

class windows_reader final : public text_reader {
 public:
  explicit windows_reader(wo::OcrEngine engine) : engine_(std::move(engine)) {}

  result<std::vector<std::string>> read(const rgb_frame& img) override {
    if (img.width == 0 || img.height == 0 || img.rgb.size() < static_cast<std::size_t>(img.width) * img.height * 3) {
      return err(status::invalid_arg);
    }
    if (!join_apartment()) return err(status::io);
    try {
      // The recogniser's own limit (MaxImageDimension, 10,000 px today): the
      // engine reads at most 2,048 px, so this only guards a future change.
      const std::uint32_t limit = wo::OcrEngine::MaxImageDimension();
      if (img.width > limit || img.height > limit) return err(status::invalid_arg);
      const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
      std::vector<std::uint8_t> bgra(n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        bgra[i * 4] = img.rgb[i * 3 + 2];
        bgra[i * 4 + 1] = img.rgb[i * 3 + 1];
        bgra[i * 4 + 2] = img.rgb[i * 3];
        bgra[i * 4 + 3] = 255;
      }
      ws::DataWriter writer;
      writer.WriteBytes(winrt::array_view<const std::uint8_t>(bgra.data(), bgra.data() + bgra.size()));
      const ws::IBuffer buffer = writer.DetachBuffer();
      const wg::SoftwareBitmap bitmap = wg::SoftwareBitmap::CreateCopyFromBuffer(
          buffer, wg::BitmapPixelFormat::Bgra8, static_cast<std::int32_t>(img.width),
          static_cast<std::int32_t>(img.height), wg::BitmapAlphaMode::Premultiplied);
      wo::OcrResult found{nullptr};
      {
        // One recognition at a time per engine: OcrEngine is not documented
        // as safe to share across concurrent calls.
        std::lock_guard lock(m_);
        found = engine_.RecognizeAsync(bitmap).get();
      }
      std::vector<std::string> lines;
      for (const wo::OcrLine& line : found.Lines()) {
        std::string text = winrt::to_string(line.Text());
        if (!text.empty()) lines.push_back(std::move(text));
      }
      return lines;
    } catch (...) {
      return err(status::io);
    }
  }
  const std::string& spec_key() const noexcept override { return key_; }

 private:
  wo::OcrEngine engine_;
  std::mutex m_;
  std::string key_ = "ocr/windows-media/1";
};

}  // namespace

std::unique_ptr<text_reader> make_text_reader() {
  if (!join_apartment()) return nullptr;
  try {
    // The user's profile languages; none installed with an OCR pack: none.
    wo::OcrEngine engine = wo::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) return nullptr;
    return std::make_unique<windows_reader>(std::move(engine));
  } catch (...) {
    return nullptr;
  }
}

}  // namespace mv::ai
