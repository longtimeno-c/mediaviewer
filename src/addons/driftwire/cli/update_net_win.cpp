// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// update.h's port on Windows: WinHTTP (cookies and authentication off,
// redirects never from https to http, a User-Agent and nothing else), and
// the program's own path from GetModuleFileNameW.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winhttp.h>

#include "addons/driftwire/cli/update.h"

#include <vector>

namespace mv::driftwire::cli {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

struct handle {
  HINTERNET h = nullptr;
  explicit handle(HINTERNET x) : h(x) {}
  ~handle() {
    if (h) ::WinHttpCloseHandle(h);
  }
  handle(const handle&) = delete;
  handle& operator=(const handle&) = delete;
};

}  // namespace

http_answer https_get(const std::string& url, std::size_t max_bytes, int timeout_ms) {
  http_answer a;
  const std::wstring wurl = wide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    a.error = "bad URL";
    return a;
  }
  handle session(::WinHttpOpen(L"driftwire", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.h) {
    a.error = "WinHTTP unavailable";
    return a;
  }
  ::WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  handle connect(::WinHttpConnect(session.h, host, parts.nPort, 0));
  if (!connect.h) {
    a.error = "cannot connect";
    return a;
  }
  handle request(::WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request.h) {
    a.error = "cannot open the request";
    return a;
  }
  DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  ::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.h, nullptr)) {
    a.error = "no answer (" + std::to_string(::GetLastError()) + ")";
    return a;
  }
  DWORD status = 0, size = sizeof(status);
  if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    a.error = "no status";
    return a;
  }
  a.status = static_cast<int>(status);
  if (status != 200) return a;
  std::vector<char> chunk(4096);
  for (;;) {
    DWORD got = 0;
    if (!::WinHttpReadData(request.h, chunk.data(), static_cast<DWORD>(chunk.size()), &got)) {
      a.status = 0;
      a.error = "cut off";
      return a;
    }
    if (got == 0) break;
    if (a.body.size() + got > max_bytes) {
      a.status = 0;
      a.error = "too long";
      return a;
    }
    a.body.append(chunk.data(), got);
  }
  return a;
}

std::string self_path() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return utf8(buf);
    }
    if (buf.size() > 32768) return {};
    buf.resize(buf.size() * 2);
  }
}

std::string local_app_data() {
  PWSTR w = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &w)) && w) out = w;
  ::CoTaskMemFree(w);
  return utf8(out);
}

}  // namespace mv::driftwire::cli
