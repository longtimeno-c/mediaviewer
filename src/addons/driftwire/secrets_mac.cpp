// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over the data protection keychain (#501): generic passwords
// under the service "MediaViewer Driftwire", one account per name, in this
// app's keychain access group.
//
//   - kSecUseDataProtectionKeychain: the iOS-style keychain, where
//     kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly holds (readable once
//     the Mac is unlocked after a start; never in a backup or a migration to
//     another Mac). The login keychain ignores that attribute, and moves with
//     Migration Assistant and Time Machine.
//   - kSecAttrAccessGroup on every query: the OS lets only apps signed with
//     that group (the app's own entitlement, so its team) make, read or
//     change an item there. Another app of the same user cannot plant an
//     identity before Driftwire's first use, or read what it keeps.
//   - The group is read from the running app's own entitlements
//     (keychain-access-groups, else an app group). Without one (a dev build,
//     the lab binary, a build signed without it) there is no store: the
//     identity is this run's and nothing can be remembered. Never the login
//     keychain.
// Items Driftwire kept in the login keychain before #501 are removed (without
// any prompt), not carried over: one there may have been planted.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecTask.h>
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

cf_ptr<CFMutableDictionaryRef> new_dictionary() {
  return cf_ptr<CFMutableDictionaryRef>(
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

// The first string of an array-valued entitlement of this process.
cf_ptr<CFStringRef> first_entitlement(SecTaskRef task, CFStringRef key) {
  cf_ptr<CFTypeRef> value(SecTaskCopyValueForEntitlement(task, key, nullptr));
  if (!value || CFGetTypeID(value.get()) != CFArrayGetTypeID()) return nullptr;
  const auto* list = static_cast<CFArrayRef>(value.get());
  if (CFArrayGetCount(list) < 1) return nullptr;
  const void* first = CFArrayGetValueAtIndex(list, 0);
  if (!first || CFGetTypeID(first) != CFStringGetTypeID()) return nullptr;
  return cf_ptr<CFStringRef>(static_cast<CFStringRef>(CFRetain(first)));
}

// This app's keychain access group: its keychain-access-groups entitlement,
// else its first app group (on macOS a team-prefixed app group is also a
// keychain access group). nullptr when it was signed with neither.
cf_ptr<CFStringRef> access_group() {
  cf_ptr<SecTaskRef> task(SecTaskCreateFromSelf(nullptr));
  if (!task) return nullptr;
  if (auto g = first_entitlement(task.get(), CFSTR("keychain-access-groups"))) return g;
  return first_entitlement(task.get(), CFSTR("com.apple.security.application-groups"));
}

// Driftwire's items in the login keychain from before #501, gone. Never a
// prompt: one the user's keychain would ask about stays.
void remove_legacy_items(const std::string& name) {
  auto q = new_dictionary();
  auto service = cf_string(name);
  CFDictionarySetValue(q.get(), kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q.get(), kSecAttrService, service.get());
  CFDictionarySetValue(q.get(), kSecUseDataProtectionKeychain, kCFBooleanFalse);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFDictionarySetValue(q.get(), kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  for (int i = 0; i < 8 && SecItemDelete(q.get()) == errSecSuccess; ++i) {
  }
}

class keychain final : public store {
 public:
  keychain(std::string service, cf_ptr<CFStringRef> group) : service_(std::move(service)), group_(std::move(group)) {}

  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    cf_ptr<CFDataRef> data(CFDataCreate(nullptr, value.data(), static_cast<CFIndex>(value.size())));
    auto q = query_for(name);
    auto change = new_dictionary();
    CFDictionarySetValue(change.get(), kSecValueData, data.get());
    if (SecItemUpdate(q.get(), change.get()) == errSecSuccess) return true;
    CFDictionarySetValue(q.get(), kSecValueData, data.get());
    CFDictionarySetValue(q.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    CFDictionarySetValue(q.get(), kSecAttrSynchronizable, kCFBooleanFalse);
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

 private:
  // Every query names the data protection keychain and this app's group, so
  // nothing outside them (the login keychain, another app's group) matches.
  cf_ptr<CFMutableDictionaryRef> query_for(const std::string& name) const {
    auto q = new_dictionary();
    auto service = cf_string(service_);
    auto account = cf_string(name);
    CFDictionarySetValue(q.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q.get(), kSecAttrService, service.get());
    CFDictionarySetValue(q.get(), kSecAttrAccount, account.get());
    CFDictionarySetValue(q.get(), kSecUseDataProtectionKeychain, kCFBooleanTrue);
    CFDictionarySetValue(q.get(), kSecAttrAccessGroup, group_.get());
    return q;
  }

  std::string service_;
  cf_ptr<CFStringRef> group_;
};

}  // namespace

std::unique_ptr<store> keychain_store(const std::string& service) {
  remove_legacy_items(service);
  auto group = access_group();
  if (!group) return nullptr;
  return std::make_unique<keychain>(service, std::move(group));
}

std::unique_ptr<store> os_store() { return keychain_store(kService); }

}  // namespace mv::driftwire::secrets
