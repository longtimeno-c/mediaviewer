// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model downloader's transport on Windows (fetch.h): WinHTTP, cookies off,
// no credentials, redirects followed (Hugging Face sends a file to its CDN).
// Sends a User-Agent and, when resuming, a Range header; nothing else.
#include "addons/ai/fetch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace mv::ai {
namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

struct handle {
  HINTERNET h = nullptr;
  explicit handle(HINTERNET x) : h(x) {}
  ~handle() {
    if (h) WinHttpCloseHandle(h);
  }
  handle(const handle&) = delete;
  handle& operator=(const handle&) = delete;
};

class win_transport final : public http_transport {
 public:
  http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int)>& head,
                 const std::function<bool(std::span<const std::uint8_t>)>& body) override {
    http_reply reply;
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
      reply.error = "bad URL";
      return reply;
    }
    handle session(WinHttpOpen(L"MediaViewer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
      reply.error = "WinHTTP unavailable";
      return reply;
    }
    WinHttpSetTimeouts(session.h, 30000, 30000, 60000, 60000);
    handle connect(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connect.h) {
      reply.error = "cannot connect";
      return reply;
    }
    handle request(WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) {
      reply.error = "cannot open the request";
      return reply;
    }
    DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    std::wstring headers;
    if (from > 0) headers = L"Range: bytes=" + std::to_wstring(from) + L"-\r\n";
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
      reply.error = "no response (" + std::to_string(GetLastError()) + ")";
      return reply;
    }
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &len, WINHTTP_NO_HEADER_INDEX);
    reply.status = static_cast<int>(code);
    if (!head(reply.status)) return reply;
    std::vector<std::uint8_t> buf(1 << 20);
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
        reply.error = "transfer cut (" + std::to_string(GetLastError()) + ")";
        break;
      }
      if (got == 0) break;
      if (!body(std::span<const std::uint8_t>(buf.data(), got))) break;
    }
    return reply;
  }
};

}  // namespace

std::unique_ptr<http_transport> make_http_transport() { return std::make_unique<win_transport>(); }

}  // namespace mv::ai
