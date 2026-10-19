// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over the login Keychain: generic passwords under the service
// "MediaViewer Driftwire", one account per name, readable by this app only
// once unlocked (kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly: never in a
// backup restored to another Mac).
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::secrets {

namespace {

constexpr const char* kService = "MediaViewer Driftwire";

struct cf_release {
  void operator()(CFTypeRef r) const noexcept {
    if (r) CFRelease(r);
  }
};
template <typename T>
using cf_ptr = std::unique_ptr<std::remove_pointer_t<T>, cf_release>;

cf_ptr<CFStringRef> cf_string(const std::string& s) {
  return cf_ptr<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                                     static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false));
}

cf_ptr<CFMutableDictionaryRef> query_for(const std::string& name) {
  cf_ptr<CFMutableDictionaryRef> q(
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  auto service = cf_string(kService);
  auto account = cf_string(name);
  CFDictionarySetValue(q.get(), kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q.get(), kSecAttrService, service.get());
  CFDictionarySetValue(q.get(), kSecAttrAccount, account.get());
  return q;
}

class keychain final : public store {
 public:
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    cf_ptr<CFDataRef> data(CFDataCreate(nullptr, value.data(), static_cast<CFIndex>(value.size())));
    auto q = query_for(name);
    cf_ptr<CFMutableDictionaryRef> change(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(change.get(), kSecValueData, data.get());
    if (SecItemUpdate(q.get(), change.get()) == errSecSuccess) return true;
    CFDictionarySetValue(q.get(), kSecValueData, data.get());
    CFDictionarySetValue(q.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    return SecItemAdd(q.get(), nullptr) == errSecSuccess;
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    auto q = query_for(name);
    CFDictionarySetValue(q.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef out = nullptr;
    if (SecItemCopyMatching(q.get(), &out) != errSecSuccess || !out) return std::nullopt;
    cf_ptr<CFTypeRef> hold(out);
    if (CFGetTypeID(out) != CFDataGetTypeID()) return std::nullopt;
    const auto* d = static_cast<CFDataRef>(out);
    const auto n = static_cast<std::size_t>(CFDataGetLength(d));
    if (n > kMaxValue) return std::nullopt;
    const UInt8* p = CFDataGetBytePtr(d);
    return std::vector<std::uint8_t>(p, p + n);
  }
  bool erase(const std::string& name) override {
    auto q = query_for(name);
    return SecItemDelete(q.get()) == errSecSuccess;
  }
};

}  // namespace

std::unique_ptr<store> os_store() { return std::make_unique<keychain>(); }

}  // namespace mv::driftwire::secrets
