// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// update.h's port on Linux: libcurl, opened at run time as libsecret is
// (secrets_linux.cpp), so the packaged program still needs only glibc
// (cmake/driftwire-cli's NEEDED gate). Without libcurl there is no check;
// `driftwire update` still runs install.sh, which fetches with curl or wget.
// https only, redirects to https only, no cookies (an easy handle keeps none
// unless asked), a User-Agent and nothing else. The program's own path is
// /proc/self/exe.
#include "addons/driftwire/cli/update.h"

#include <dlfcn.h>
#include <unistd.h>

#include <climits>
#include <cstdint>

namespace mv::driftwire::cli {

namespace {

// libcurl's ABI (curl/curl.h): stable since 7.x, and only what this uses.
using CURL = void;
using curl_code = int;
constexpr int kOptWriteData = 10001;        // CURLOPT_WRITEDATA
constexpr int kOptUrl = 10002;              // CURLOPT_URL
constexpr int kOptUserAgent = 10018;        // CURLOPT_USERAGENT
constexpr int kOptWriteFunction = 20011;    // CURLOPT_WRITEFUNCTION
constexpr int kOptFollowLocation = 52;      // CURLOPT_FOLLOWLOCATION
constexpr int kOptMaxRedirs = 68;           // CURLOPT_MAXREDIRS
constexpr int kOptNoSignal = 99;            // CURLOPT_NOSIGNAL
constexpr int kOptTimeoutMs = 155;          // CURLOPT_TIMEOUT_MS
constexpr int kOptConnectTimeoutMs = 156;   // CURLOPT_CONNECTTIMEOUT_MS
constexpr int kOptProtocols = 181;          // CURLOPT_PROTOCOLS (a bitmask; _STR came in 7.85)
constexpr int kOptRedirProtocols = 182;     // CURLOPT_REDIR_PROTOCOLS
constexpr long kProtoHttps = 1 << 1;        // CURLPROTO_HTTPS
constexpr int kInfoResponseCode = 0x200002; // CURLINFO_RESPONSE_CODE

struct curl_api {
  CURL* (*easy_init)() = nullptr;
  curl_code (*easy_setopt)(CURL*, int, ...) = nullptr;
  curl_code (*easy_perform)(CURL*) = nullptr;
  curl_code (*easy_getinfo)(CURL*, int, ...) = nullptr;
  void (*easy_cleanup)(CURL*) = nullptr;
  const char* (*easy_strerror)(curl_code) = nullptr;
  bool ok = false;
};

template <typename F>
bool take(void* lib, const char* name, F& into) {
  into = reinterpret_cast<F>(::dlsym(lib, name));
  return into != nullptr;
}

const curl_api& curl() {
  static const curl_api api = [] {
    curl_api c;
    void* lib = ::dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = ::dlopen("libcurl-gnutls.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return c;
    c.ok = take(lib, "curl_easy_init", c.easy_init) && take(lib, "curl_easy_setopt", c.easy_setopt) &&
           take(lib, "curl_easy_perform", c.easy_perform) && take(lib, "curl_easy_getinfo", c.easy_getinfo) &&
           take(lib, "curl_easy_cleanup", c.easy_cleanup) && take(lib, "curl_easy_strerror", c.easy_strerror);
    return c;  // never closed: the process keeps it
  }();
  return api;
}

struct sink {
  std::string* body;
  std::size_t max;
  bool too_long = false;
};

std::size_t write_to(char* data, std::size_t size, std::size_t n, void* user) {
  auto* s = static_cast<sink*>(user);
  const std::size_t bytes = size * n;
  if (s->body->size() + bytes > s->max) {
    s->too_long = true;
    return 0;  // stops the transfer
  }
  s->body->append(data, bytes);
  return bytes;
}

}  // namespace

http_answer https_get(const std::string& url, std::size_t max_bytes, int timeout_ms) {
  http_answer a;
  if (url.rfind("https://", 0) != 0) {
    a.error = "bad URL";
    return a;
  }
  const curl_api& c = curl();
  if (!c.ok) {
    a.error = "no libcurl";
    return a;
  }
  CURL* h = c.easy_init();
  if (!h) {
    a.error = "libcurl did not start";
    return a;
  }
  sink s{&a.body, max_bytes};
  c.easy_setopt(h, kOptUrl, url.c_str());
  c.easy_setopt(h, kOptUserAgent, "driftwire");
  c.easy_setopt(h, kOptProtocols, kProtoHttps);
  c.easy_setopt(h, kOptRedirProtocols, kProtoHttps);
  c.easy_setopt(h, kOptFollowLocation, 1L);
  c.easy_setopt(h, kOptMaxRedirs, 5L);
  c.easy_setopt(h, kOptNoSignal, 1L);  // a thread of its own: no SIGALRM for DNS timeouts
  c.easy_setopt(h, kOptTimeoutMs, static_cast<long>(timeout_ms));
  c.easy_setopt(h, kOptConnectTimeoutMs, static_cast<long>(timeout_ms));
  c.easy_setopt(h, kOptWriteFunction, &write_to);
  c.easy_setopt(h, kOptWriteData, &s);
  const curl_code r = c.easy_perform(h);
  long status = 0;
  c.easy_getinfo(h, kInfoResponseCode, &status);
  c.easy_cleanup(h);
  if (r != 0) {
    a.error = s.too_long ? std::string("too long") : std::string(c.easy_strerror(r));
    a.body.clear();
    return a;
  }
  a.status = static_cast<int>(status);
  return a;
}

std::string self_path() {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::string(buf, static_cast<std::size_t>(n));
}

std::string local_app_data() { return {}; }

}  // namespace mv::driftwire::cli
