// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The nearby service on Windows (advert.h): one DNS-SD registration through
// Windows' own mDNS responder (DnsServiceRegister, Windows 10 1809 and later;
// the app's floor is 2004). The phone looks for _mediaviewer._tcp as it does
// for a Mac (docs/plans/mobile.md §7.5).
//
// Windows does not rename on a conflict as mDNSResponder does, so the instance
// is "MediaViewer-" and six random hex digits, new each time: unique on a home
// network, and it says nothing about the machine or the person (rule 6). The
// host name is the one Windows already answers for (<computer>.local).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>

#include <cwchar>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "addons/driftwire/advert.h"

namespace mv::driftwire::advert {

namespace {

std::wstring wide(const std::string& s) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Everything Windows may still touch from its completion thread: the request,
// the instance it points at and the strings that built it. Freed only once a
// deregistration has completed; if Windows never answers, it is left alone
// rather than freed under a callback that may still come.
struct state {
  HANDLE registered = nullptr;  // the register's completion
  HANDLE gone = nullptr;        // the deregister's
  DNS_SERVICE_REGISTER_REQUEST request{};
  PDNS_SERVICE_INSTANCE instance = nullptr;
  std::wstring host, name;
  std::vector<std::wstring> keys, values;
  ~state() {
    if (instance) ::DnsServiceFreeInstance(instance);
    if (registered) ::CloseHandle(registered);
    if (gone) ::CloseHandle(gone);
  }
};

// Register's and deregister's completion: frees the copy it is handed and
// signals the event the request carried as its context (each has its own, so
// a late register's answer never ends the wait for the deregister's).
VOID WINAPI on_complete(DWORD, PVOID context, PDNS_SERVICE_INSTANCE instance) {
  if (instance) ::DnsServiceFreeInstance(instance);
  ::SetEvent(static_cast<HANDLE>(context));
}

class dns_registration final : public registration {
 public:
  explicit dns_registration(std::unique_ptr<state> s) : s_(std::move(s)) {}
  dns_registration(const dns_registration&) = delete;
  dns_registration& operator=(const dns_registration&) = delete;

  ~dns_registration() override {
    // Bounded: the DNS service may be stopping with the machine. A callback
    // that may still come keeps the state alive (a leak, never a use after free).
    if (::WaitForSingleObject(s_->registered, 2000) != WAIT_OBJECT_0) {
      (void)s_.release();
      return;
    }
    s_->request.pQueryContext = s_->gone;
    if (::DnsServiceDeRegister(&s_->request, nullptr) == DNS_REQUEST_PENDING &&
        ::WaitForSingleObject(s_->gone, 2000) != WAIT_OBJECT_0) {
      (void)s_.release();
    }
  }

 private:
  std::unique_ptr<state> s_;
};

}  // namespace

std::unique_ptr<registration> publish(std::uint16_t port, const std::vector<std::pair<std::string, std::string>>& txt) {
  auto s = std::make_unique<state>();
  s->registered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  s->gone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!s->registered || !s->gone) return nullptr;

  wchar_t host[256] = {};
  DWORD host_len = static_cast<DWORD>(std::size(host));
  if (!::GetComputerNameExW(ComputerNameDnsHostname, host, &host_len) || host_len == 0) return nullptr;
  s->host = std::wstring(host, host_len) + L".local";

  std::random_device rd;
  wchar_t tag[8] = {};
  (void)::swprintf_s(tag, std::size(tag), L"%06x", static_cast<unsigned>(rd() & 0xFFFFFFu));
  s->name = std::wstring(L"MediaViewer-") + tag + L"." + wide(kServiceType) + L".local";

  std::vector<PCWSTR> keys, values;
  for (const auto& [k, v] : txt) {
    if (k.size() + v.size() + 1 > 255) return nullptr;
    s->keys.push_back(wide(k));
    s->values.push_back(wide(v));
  }
  for (std::size_t i = 0; i < s->keys.size(); ++i) {
    keys.push_back(s->keys[i].c_str());
    values.push_back(s->values[i].c_str());
  }
  s->instance = ::DnsServiceConstructInstance(s->name.c_str(), s->host.c_str(), nullptr, nullptr, port, 0, 0,
                                              static_cast<DWORD>(keys.size()), keys.data(), values.data());
  if (!s->instance) return nullptr;

  s->request.Version = DNS_QUERY_REQUEST_VERSION1;
  s->request.InterfaceIndex = 0;  // every interface
  s->request.pServiceInstance = s->instance;
  s->request.pRegisterCompletionCallback = &on_complete;
  s->request.pQueryContext = s->registered;
  s->request.unicastEnabled = FALSE;
  // Not waited on, as dns_sd's register is not: the responder answers in the
  // background, and a failure means only that a phone does not find this PC.
  if (::DnsServiceRegister(&s->request, nullptr) != DNS_REQUEST_PENDING) return nullptr;
  return std::make_unique<dns_registration>(std::move(s));
}

}  // namespace mv::driftwire::advert
