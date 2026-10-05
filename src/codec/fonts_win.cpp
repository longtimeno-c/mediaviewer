// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// codec/fonts.h on Windows: the DirectWrite system collection, used only to
// find the file behind a family and style. A font that is not a local file
// (a cloud font not yet downloaded) is treated as not installed.
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <mutex>
#include <vector>

#include "codec/fonts.h"

namespace mv::codec::fonts {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) {
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

// The shared factory is free-threaded; one for the process.
IDWriteFactory* factory() noexcept {
  static ComPtr<IDWriteFactory> shared;
  static std::once_flag once;
  std::call_once(once, [] {
    (void)DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(shared.GetAddressOf()));
  });
  return shared.Get();
}

}  // namespace

std::optional<face_ref> find(std::string_view family, bool bold, bool italic) {
  IDWriteFactory* f = factory();
  if (!f || family.empty()) return std::nullopt;
  ComPtr<IDWriteFontCollection> collection;
  if (FAILED(f->GetSystemFontCollection(&collection, FALSE)) || !collection) return std::nullopt;
  UINT32 index = 0;
  BOOL exists = FALSE;
  const std::wstring name = wide(family);
  if (FAILED(collection->FindFamilyName(name.c_str(), &index, &exists)) || !exists) return std::nullopt;
  ComPtr<IDWriteFontFamily> fam;
  if (FAILED(collection->GetFontFamily(index, &fam))) return std::nullopt;
  ComPtr<IDWriteFont> font;
  if (FAILED(fam->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL,
                                       italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                       &font))) {
    return std::nullopt;
  }
  ComPtr<IDWriteFontFace> face;
  if (FAILED(font->CreateFontFace(&face))) return std::nullopt;
  UINT32 file_count = 0;
  if (FAILED(face->GetFiles(&file_count, nullptr)) || file_count == 0) return std::nullopt;
  std::vector<IDWriteFontFile*> files(file_count, nullptr);
  if (FAILED(face->GetFiles(&file_count, files.data()))) return std::nullopt;
  std::vector<ComPtr<IDWriteFontFile>> owned;
  for (IDWriteFontFile* file : files) owned.emplace_back().Attach(file);

  const void* key = nullptr;
  UINT32 key_size = 0;
  ComPtr<IDWriteFontFileLoader> loader;
  ComPtr<IDWriteLocalFontFileLoader> local;
  if (FAILED(owned[0]->GetReferenceKey(&key, &key_size)) || FAILED(owned[0]->GetLoader(&loader)) ||
      FAILED(loader.As(&local))) {
    return std::nullopt;
  }
  UINT32 length = 0;
  if (FAILED(local->GetFilePathLengthFromKey(key, key_size, &length))) return std::nullopt;
  std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
  if (FAILED(local->GetFilePathFromKey(key, key_size, path.data(), length + 1))) return std::nullopt;
  path.resize(length);

  face_ref out;
  out.path = narrow(path);
  out.index = face->GetIndex();
  out.bold = font->GetWeight() >= DWRITE_FONT_WEIGHT_SEMI_BOLD;
  out.italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
  return out;
}

std::span<const std::string_view> fallbacks() noexcept {
  static constexpr std::array<std::string_view, 9> kFallbacks = {
      "Segoe UI", "Arial", "Microsoft YaHei", "Yu Gothic", "Malgun Gothic",
      "Nirmala UI", "Ebrima", "Segoe UI Symbol", "Segoe UI Historic",
  };
  return kFallbacks;
}

}  // namespace mv::codec::fonts
