// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The system proxy on the Mac (sysproxy.h): every enabled network service's
// automatic proxy configuration (ProxyAutoConfigEnable and
// ProxyAutoConfigURLString in its Proxies protocol), what System Settings ->
// Network -> Details -> Proxies -> "Automatic proxy configuration" sets.
// Changing it needs an administrator: the system asks once, and the right is
// kept for the rest of the run (this port's AuthorizationRef), so turning the
// Tunnel off and on again does not ask again.
//
// What was there is kept as one line per service: its id, the old enable
// flag (0, 1, or - when the key was absent) and the old URL, tab-separated.
#include "addons/driftwire/sysproxy.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Authorization.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <mutex>
#include <sstream>
#include <vector>

namespace mv::driftwire::sysproxy {

namespace {

constexpr const char* kRight = "system.services.systemconfiguration.network";

template <class T>
struct cf {
  T ref = nullptr;
  cf() = default;
  explicit cf(T r) : ref(r) {}
  cf(const cf&) = delete;
  cf& operator=(const cf&) = delete;
  ~cf() {
    if (ref) CFRelease(ref);
  }
  explicit operator bool() const { return ref != nullptr; }
};

std::string to_utf8(CFStringRef s) {
  if (!s) return {};
  const CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(n), '\0');
  if (!CFStringGetCString(s, out.data(), n, kCFStringEncodingUTF8)) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

cf<CFStringRef> from_utf8(const std::string& s) {
  return cf<CFStringRef>(CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8));
}

struct entry {
  std::string service;
  std::string enable;  // "0", "1" or "-"
  std::string url;
};

std::string join(const std::vector<entry>& es) {
  std::string out;
  for (const auto& e : es) out += e.service + "\t" + e.enable + "\t" + e.url + "\n";
  return out;
}

std::vector<entry> split(const std::string& text) {
  std::vector<entry> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto a = line.find('\t');
    const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    out.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return out;
}

class mac_port final : public port {
 public:
  ~mac_port() override {
    if (auth_) AuthorizationFree(auth_, kAuthorizationFlagDefaults);
  }
  std::string unsupported() const override { return {}; }

  result<std::string> set(const std::string& pac_url) override {
    if (pac_url.find_first_of("\t\n") != std::string::npos) return err(status::invalid_arg);
    std::lock_guard lock(m_);
    std::vector<entry> before;
    const status s = change([&](SCNetworkServiceRef svc, SCNetworkProtocolRef proto) {
      const auto id = to_utf8(SCNetworkServiceGetServiceID(svc));
      if (id.empty() || id.find_first_of("\t\n") != std::string::npos) return false;
      CFDictionaryRef old = SCNetworkProtocolGetConfiguration(proto);
      entry e{id, "-", ""};
      if (old) {
        if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(old, kSCPropNetProxiesProxyAutoConfigEnable));
            n && CFGetTypeID(n) == CFNumberGetTypeID()) {
          int v = 0;
          CFNumberGetValue(n, kCFNumberIntType, &v);
          e.enable = v ? "1" : "0";
        }
        if (auto u = static_cast<CFStringRef>(CFDictionaryGetValue(old, kSCPropNetProxiesProxyAutoConfigURLString));
            u && CFGetTypeID(u) == CFStringGetTypeID()) {
          e.url = to_utf8(u);
          if (e.url.find_first_of("\t\n") != std::string::npos) return false;
        }
      }
      cf<CFMutableDictionaryRef> next(
          old ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, old)
              : CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks));
      if (!next) return false;
      int one = 1;
      cf<CFNumberRef> on(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one));
      auto url = from_utf8(pac_url);
      if (!on || !url) return false;
      CFDictionarySetValue(next.ref, kSCPropNetProxiesProxyAutoConfigEnable, on.ref);
      CFDictionarySetValue(next.ref, kSCPropNetProxiesProxyAutoConfigURLString, url.ref);
      if (!SCNetworkProtocolSetConfiguration(proto, next.ref)) return false;
      before.push_back(std::move(e));
      return true;
    });
    if (s != status::ok) return err(s);
    if (before.empty()) return err(status::io);  // no enabled service took it
    return join(before);
  }

  bool restore(const std::string& saved, const std::string& pac_url) override {
    std::lock_guard lock(m_);
    const auto was = split(saved);
    const status s = change([&](SCNetworkServiceRef svc, SCNetworkProtocolRef proto) {
      const auto id = to_utf8(SCNetworkServiceGetServiceID(svc));
      const entry* e = nullptr;
      for (const auto& x : was) {
        if (x.service == id) e = &x;
      }
      if (!e) return false;
      CFDictionaryRef now = SCNetworkProtocolGetConfiguration(proto);
      if (!now) return false;
      auto u = static_cast<CFStringRef>(CFDictionaryGetValue(now, kSCPropNetProxiesProxyAutoConfigURLString));
      if (!u || CFGetTypeID(u) != CFStringGetTypeID() || to_utf8(u) != pac_url) return false;  // not ours any more
      cf<CFMutableDictionaryRef> next(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, now));
      if (!next) return false;
      if (e->enable == "-") {
        CFDictionaryRemoveValue(next.ref, kSCPropNetProxiesProxyAutoConfigEnable);
      } else {
        int v = e->enable == "1" ? 1 : 0;
        cf<CFNumberRef> n(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v));
        if (n) CFDictionarySetValue(next.ref, kSCPropNetProxiesProxyAutoConfigEnable, n.ref);
      }
      if (e->url.empty()) {
        CFDictionaryRemoveValue(next.ref, kSCPropNetProxiesProxyAutoConfigURLString);
      } else if (auto old = from_utf8(e->url)) {
        CFDictionarySetValue(next.ref, kSCPropNetProxiesProxyAutoConfigURLString, old.ref);
      }
      return static_cast<bool>(SCNetworkProtocolSetConfiguration(proto, next.ref));
    });
    // Nothing of ours found is nothing left to undo.
    return s == status::ok || s == status::not_found;
  }

 private:
  // The administrator's right, asked for once a run.
  status authorise() {
    if (!auth_ && AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, &auth_) !=
                      errAuthorizationSuccess) {
      auth_ = nullptr;
      return status::io;
    }
    AuthorizationItem item{kRight, 0, nullptr, 0};
    AuthorizationRights rights{1, &item};
    const OSStatus r = AuthorizationCopyRights(
        auth_, &rights, nullptr,
        kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights | kAuthorizationFlagPreAuthorize,
        nullptr);
    if (r == errAuthorizationCanceled || r == errAuthorizationDenied) return status::permission_denied;
    return r == errAuthorizationSuccess ? status::ok : status::io;
  }

  // Runs `each` on every enabled service's Proxies protocol under the
  // preferences' lock, then commits and applies when one changed.
  // not_found when none did.
  template <class F>
  status change(F each) {
    if (const status a = authorise(); a != status::ok) return a;
    cf<SCPreferencesRef> prefs(SCPreferencesCreateWithAuthorization(kCFAllocatorDefault, CFSTR("MediaViewer Driftwire"),
                                                                    nullptr, auth_));
    if (!prefs) return status::io;
    if (!SCPreferencesLock(prefs.ref, true)) return status::io;
    bool changed = false;
    {
      cf<SCNetworkSetRef> set(SCNetworkSetCopyCurrent(prefs.ref));
      cf<CFArrayRef> services(set ? SCNetworkSetCopyServices(set.ref) : nullptr);
      const CFIndex n = services ? CFArrayGetCount(services.ref) : 0;
      for (CFIndex i = 0; i < n; ++i) {
        auto svc = static_cast<SCNetworkServiceRef>(const_cast<void*>(CFArrayGetValueAtIndex(services.ref, i)));
        if (!SCNetworkServiceGetEnabled(svc)) continue;
        cf<SCNetworkProtocolRef> proto(SCNetworkServiceCopyProtocol(svc, kSCNetworkProtocolTypeProxies));
        if (!proto) continue;
        if (each(svc, proto.ref)) changed = true;
      }
    }
    status out = status::not_found;
    if (changed) {
      out = SCPreferencesCommitChanges(prefs.ref) && SCPreferencesApplyChanges(prefs.ref) ? status::ok : status::io;
    }
    SCPreferencesUnlock(prefs.ref);
    return out;
  }

  std::mutex m_;
  AuthorizationRef auth_ = nullptr;  // under m_
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<mac_port>(); }

}  // namespace mv::driftwire::sysproxy
