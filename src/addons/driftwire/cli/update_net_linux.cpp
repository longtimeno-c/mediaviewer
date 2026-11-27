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
constexpr int kOptPostFields = 10015;       // CURLOPT_POSTFIELDS
constexpr int kOptPostFieldSize = 60;       // CURLOPT_POSTFIELDSIZE
constexpr int kOptHttpHeader = 10023;       // CURLOPT_HTTPHEADER
constexpr int kOptHeaderData = 10029;       // CURLOPT_HEADERDATA
constexpr int kOptHeaderFunction = 20079;   // CURLOPT_HEADERFUNCTION
using curl_slist = void;

struct curl_api {
  CURL* (*easy_init)() = nullptr;
  curl_code (*easy_setopt)(CURL*, int, ...) = nullptr;
  curl_code (*easy_perform)(CURL*) = nullptr;
  curl_code (*easy_getinfo)(CURL*, int, ...) = nullptr;
  void (*easy_cleanup)(CURL*) = nullptr;
  const char* (*easy_strerror)(curl_code) = nullptr;
  curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
  void (*slist_free_all)(curl_slist*) = nullptr;
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
           take(lib, "curl_easy_cleanup", c.easy_cleanup) && take(lib, "curl_easy_strerror", c.easy_strerror) &&
           take(lib, "curl_slist_append", c.slist_append) && take(lib, "curl_slist_free_all", c.slist_free_all);
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

// The three headers ACME reads, as they come (any case).
std::size_t header_to(char* data, std::size_t size, std::size_t n, void* user) {
  auto* r = static_cast<https_reply*>(user);
  const std::size_t bytes = size * n;
  std::string line(data, bytes);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  const auto colon = line.find(':');
  if (colon == std::string::npos) return bytes;
  std::string name = line.substr(0, colon);
  for (auto& ch : name) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
  std::string v = line.substr(colon + 1);
  while (!v.empty() && v.front() == ' ') v.erase(v.begin());
  if (name == "location") r->location = v;
  if (name == "replay-nonce") r->nonce = v;
  if (name == "retry-after" && !v.empty() && v.size() < 10 && v.find_first_not_of("0123456789") == std::string::npos) {
    r->retry_after = std::stoll(v);
  }
  return bytes;
}

}  // namespace

https_reply https_request(const std::string& method, const std::string& url, const std::string& content_type,
                          const std::string& body, std::size_t max_bytes, int timeout_ms) {
  https_reply a;
  if (url.rfind("https://", 0) != 0 || (method != "GET" && method != "POST")) {
    a.error = "bad request";
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
  curl_slist* headers = nullptr;
  c.easy_setopt(h, kOptUrl, url.c_str());
  c.easy_setopt(h, kOptUserAgent, "driftwire");
  c.easy_setopt(h, kOptProtocols, kProtoHttps);
  c.easy_setopt(h, kOptNoSignal, 1L);
  c.easy_setopt(h, kOptTimeoutMs, static_cast<long>(timeout_ms));
  c.easy_setopt(h, kOptConnectTimeoutMs, static_cast<long>(timeout_ms));
  c.easy_setopt(h, kOptWriteFunction, &write_to);
  c.easy_setopt(h, kOptWriteData, &s);
  c.easy_setopt(h, kOptHeaderFunction, &header_to);
  c.easy_setopt(h, kOptHeaderData, &a);
  const std::string type = "Content-Type: " + content_type;
  if (method == "POST") {
    headers = c.slist_append(nullptr, type.c_str());
    c.easy_setopt(h, kOptHttpHeader, headers);
    c.easy_setopt(h, kOptPostFieldSize, static_cast<long>(body.size()));
    c.easy_setopt(h, kOptPostFields, body.c_str());
  }
  const curl_code r = c.easy_perform(h);
  long status = 0;
  c.easy_getinfo(h, kInfoResponseCode, &status);
  c.easy_cleanup(h);
  if (headers) c.slist_free_all(headers);
  if (r != 0) {
    const std::string why = s.too_long ? std::string("too long") : std::string(c.easy_strerror(r));
    a = https_reply{};
    a.error = why;
    return a;
  }
  a.status = static_cast<int>(status);
  return a;
}

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
