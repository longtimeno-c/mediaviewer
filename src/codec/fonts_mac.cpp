// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// codec/fonts.h on macOS: CoreText's descriptor matching, family mandatory so
// a missing family is "not installed", never a silent substitute.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <array>

#include "codec/fonts.h"

namespace mv::codec::fonts {
namespace {

template <class T>
struct cf {
  T ref = nullptr;
  cf() = default;
  explicit cf(T r) : ref(r) {}
  ~cf() {
    if (ref) CFRelease(ref);
  }
  cf(const cf&) = delete;
  cf& operator=(const cf&) = delete;
};

std::string utf8_of(CFStringRef s) {
  if (!s) return {};
  const CFIndex len = CFStringGetLength(s);
  const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(max), '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

}  // namespace

std::optional<face_ref> find(std::string_view family, bool bold, bool italic) {
  if (family.empty()) return std::nullopt;
  cf<CFStringRef> name(CFStringCreateWithBytes(kCFAllocatorDefault,
                                               reinterpret_cast<const UInt8*>(family.data()),
                                               static_cast<CFIndex>(family.size()),
                                               kCFStringEncodingUTF8, false));
  if (!name.ref) return std::nullopt;

  std::uint32_t wanted = 0;
  if (bold) wanted |= kCTFontBoldTrait;
  if (italic) wanted |= kCTFontItalicTrait;
  cf<CFNumberRef> traits_value(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &wanted));
  const void* trait_keys[] = {kCTFontSymbolicTrait};
  const void* trait_values[] = {traits_value.ref};
  cf<CFDictionaryRef> traits(CFDictionaryCreate(kCFAllocatorDefault, trait_keys, trait_values, 1,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks));
  const void* keys[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
  const void* values[] = {name.ref, traits.ref};
  cf<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks));
  cf<CTFontDescriptorRef> wanted_desc(CTFontDescriptorCreateWithAttributes(attrs.ref));
  const void* mandatory_keys[] = {kCTFontFamilyNameAttribute};
  cf<CFSetRef> mandatory(CFSetCreate(kCFAllocatorDefault, mandatory_keys, 1, &kCFTypeSetCallBacks));
  cf<CTFontDescriptorRef> match(
      CTFontDescriptorCreateMatchingFontDescriptor(wanted_desc.ref, mandatory.ref));
  if (!match.ref) return std::nullopt;

  cf<CFURLRef> url(static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(match.ref, kCTFontURLAttribute)));
  if (!url.ref) return std::nullopt;
  UInt8 path[4096];
  if (!CFURLGetFileSystemRepresentation(url.ref, true, path, sizeof path)) return std::nullopt;

  face_ref out;
  out.path = reinterpret_cast<const char*>(path);
  cf<CFStringRef> ps(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(match.ref, kCTFontNameAttribute)));
  out.postscript = utf8_of(ps.ref);
  cf<CFDictionaryRef> got(static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(match.ref, kCTFontTraitsAttribute)));
  if (got.ref) {
    if (auto v = static_cast<CFNumberRef>(CFDictionaryGetValue(got.ref, kCTFontSymbolicTrait))) {
      std::uint32_t t = 0;
      CFNumberGetValue(v, kCFNumberSInt32Type, &t);
      out.bold = (t & kCTFontBoldTrait) != 0;
      out.italic = (t & kCTFontItalicTrait) != 0;
    }
  }
  return out;
}

std::span<const std::string_view> fallbacks() noexcept {
  static constexpr std::array<std::string_view, 9> kFallbacks = {
      "Helvetica Neue", "Helvetica", "Arial", "PingFang SC", "Hiragino Sans",
      "Apple SD Gothic Neo", "Arial Unicode MS", "Apple Symbols", "STIXGeneral",
  };
  return kFallbacks;
}

}  // namespace mv::codec::fonts
