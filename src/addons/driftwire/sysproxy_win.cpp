// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The system proxy on Windows (sysproxy.h): this user's WinINet setting for
// the LAN connection, what Settings -> Network & internet -> Proxy -> "Use
// setup script" sets (and Edge, Chrome, the Claude app and WinHTTP's
// importers read). Per user: no administrator. Other connections (a dial-up
// or a VPN entry of its own) keep theirs.
//
// What was there is kept as "<flags>\t<old script URL, UTF-8>".
#include "addons/driftwire/sysproxy.h"

#include <windows.h>
#include <wininet.h>

#include <string>

namespace mv::driftwire::sysproxy {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s) {
  if (!s || !*s) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

struct setting {
  DWORD flags = PROXY_TYPE_DIRECT;
  std::string url;
};

bool read(setting& out) {
  INTERNET_PER_CONN_OPTIONW opts[2]{};
  opts[0].dwOption = INTERNET_PER_CONN_FLAGS;
  opts[1].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
  INTERNET_PER_CONN_OPTION_LISTW list{};
  list.dwSize = sizeof list;
  list.pszConnection = nullptr;  // the LAN
  list.dwOptionCount = 2;
  list.pOptions = opts;
  DWORD size = sizeof list;
  if (!::InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size)) return false;
  out.flags = opts[0].Value.dwValue;
  out.url = narrow(opts[1].Value.pszValue);
  if (opts[1].Value.pszValue) ::GlobalFree(opts[1].Value.pszValue);
  return true;
}

bool write(const setting& s) {
  std::wstring url = wide(s.url);
  INTERNET_PER_CONN_OPTIONW opts[2]{};
  opts[0].dwOption = INTERNET_PER_CONN_FLAGS;
  opts[0].Value.dwValue = s.flags;
  opts[1].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
  opts[1].Value.pszValue = url.empty() ? nullptr : url.data();
  INTERNET_PER_CONN_OPTION_LISTW list{};
  list.dwSize = sizeof list;
  list.pszConnection = nullptr;
  list.dwOptionCount = 2;
  list.pOptions = opts;
  if (!::InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof list)) return false;
  // Running programs that read WinINet hear of it now.
  (void)::InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
  (void)::InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
  return true;
}

class win_port final : public port {
 public:
  std::string unsupported() const override { return {}; }

  result<std::string> set(const std::string& pac_url) override {
    setting was;
    if (!read(was) || was.url.find_first_of("\t\n") != std::string::npos) return err(status::io);
    setting next{was.flags | PROXY_TYPE_AUTO_PROXY_URL, pac_url};
    if (!write(next)) return err(status::io);
    return std::to_string(was.flags) + "\t" + was.url;
  }

  bool restore(const std::string& saved, const std::string& pac_url) override {
    setting now;
    if (!read(now)) return false;
    // Changed since by the person (or another program): theirs now.
    if (now.url != pac_url || !(now.flags & PROXY_TYPE_AUTO_PROXY_URL)) return true;
    const auto tab = saved.find('\t');
    if (tab == std::string::npos) return false;
    setting was;
    try {
      was.flags = static_cast<DWORD>(std::stoul(saved.substr(0, tab)));
    } catch (...) {
      return false;
    }
    was.url = saved.substr(tab + 1);
    return write(was);
  }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<win_port>(); }

}  // namespace mv::driftwire::sysproxy
