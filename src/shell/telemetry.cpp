// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/telemetry.h"

#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/trace.h"
#include "shell/settings_store.h"
#include "shell/update_guard.h"

#pragma comment(lib, "bcrypt.lib")

// The shipped number comes from project(VERSION) via the lab target. The test
// binary links this file without that define; a build there is by definition
// not a release, and saying so is better than inventing a version.
#ifndef MV_APP_VERSION
#define MV_APP_VERSION "0.0.0-dev"
#endif

namespace mv::shell::telemetry {
namespace {

constexpr std::string_view kSection = "telemetry";

// Metric names, like event ids, are a closed set. A call site that invents one
// is a bug caught in the test, not a field that reaches a payload.
constexpr std::string_view kMetricNames[] = {
    "count",        // how many of whatever the event counts
    "width",        // decoded geometry - a number, never the file
    "height",       //
    "bit_depth",    //
    "p99_us",       // frame time, microseconds
    "p50_us",       //
    "refresh_hz",   //
    "hw_decode",    // 1 hardware, 0 software fallback
    "driver_build", // GPU driver build number
    "status",       // a decoder's own numeric result
};

[[nodiscard]] bool metric_name_allowed(std::string_view name) noexcept {
  for (std::string_view known : kMetricNames) {
    if (known == name) return true;
  }
  return false;
}

std::mutex g_spool_mutex;

[[nodiscard]] std::string random_hex_128() {
  unsigned char bytes[16] = {};
  if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return {};
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(bytes) * 2);
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

}  // namespace

bool looks_like_user_data(std::string_view s) noexcept {
  if (s.empty()) return true;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    // Printable ASCII only: anything else could be a wide path transcoded, or
    // a control character that breaks the line format.
    if (c < 0x20 || c > 0x7E) return true;
    switch (c) {
      case '\\':
      case '/':
      case ':':   // a drive letter, or a URL scheme
      case '%':   // %USERPROFILE%
      case '~':
      case '@':
      case '.':   // a file extension, a hostname, a version-looking path
      case '"':
      case '\'':
      case ',':   // the field separator of the line format
      case '=':
      case ' ':
        return true;
      default:
        break;
    }
  }
  return false;
}

bool tag_allowed(std::string_view tag) noexcept {
  if (tag.empty() || tag.size() > kMaxTagLength) return false;
  return !looks_like_user_data(tag);
}

bool enabled() noexcept {
  return mv::shell::app_settings().get_int(kSection, "enabled", 0) != 0;
}

bool asked() noexcept {
  return mv::shell::app_settings().get_int(kSection, "asked", 0) != 0;
}

void set_enabled(bool on) noexcept {
  auto& settings = mv::shell::app_settings();
  settings.set_int(kSection, "enabled", on ? 1 : 0);
  // Answering the screen either way is the answer. The opt-in never reappears.
  settings.set_int(kSection, "asked", 1);
  if (!on) {
    // "A setting that turns it off later and actually does" (plan/13): the id
    // is dropped and the spool is deleted, not merely ignored.
    settings.set(kSection, "install_id", "");
    const std::wstring spool = spool_path();
    if (!spool.empty()) ::DeleteFileW(spool.c_str());
    MV_LOG_INFO("telemetry: off; install id dropped and spool deleted");
  } else {
    MV_LOG_INFO("telemetry: on by explicit choice");
  }
}

std::string install_id() noexcept {
  if (!enabled()) return {};
  try {
    auto& settings = mv::shell::app_settings();
    std::string id = settings.get(kSection, "install_id", "");
    if (id.size() == 32) return id;
    id = random_hex_128();
    if (id.empty()) return {};
    settings.set(kSection, "install_id", id);
    return id;
  } catch (...) {
    return {};
  }
}

void rotate_install_id() noexcept {
  try {
    mv::shell::app_settings().set(kSection, "install_id", "");
  } catch (...) {
  }
}

std::wstring spool_path() {
  const mv::shell::update::install_layout layout = mv::shell::update::locate_install();
  std::wstring root = layout.root;
  if (root.empty()) {
    // A dev build: keep the spool beside the settings file rather than nowhere,
    // so the schema check can be run against a local run.
    wchar_t buffer[MAX_PATH] = {};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    root.assign(buffer, n);
    root += L"\\MediaViewer";
  }
  return root + L"\\telemetry\\spool.jsonl";
}

std::string format_event(event id, std::string_view tag, const std::vector<metric>& metrics,
                         std::string_view install, std::string_view version) {
  std::string line = "{\"event\":";
  line += std::to_string(static_cast<std::int32_t>(id));
  line += ",\"tag\":\"";
  line.append(tag);
  line += "\",\"install\":\"";
  line.append(install);
  line += "\",\"version\":\"";
  line.append(version);
  line += "\"";
  for (const metric& m : metrics) {
    line += ",\"";
    line.append(m.name);
    line += "\":";
    line += std::to_string(m.value);
  }
  line += "}";
  return line;
}

bool record(event id, std::string_view tag, const std::vector<metric>& metrics) noexcept {
  // The inert path, and it is first: with consent off nothing is generated,
  // nothing is opened, nothing is buffered.
  if (!enabled()) return false;
  if (!tag_allowed(tag)) {
    MV_LOG_WARN("telemetry: event %d dropped, tag is not in the vocabulary",
                static_cast<int>(id));
    return false;
  }
  for (const metric& m : metrics) {
    if (!metric_name_allowed(m.name)) {
      MV_LOG_WARN("telemetry: event %d dropped, unknown metric name", static_cast<int>(id));
      return false;
    }
  }
  try {
    const std::string install = install_id();
    if (install.empty()) return false;
    const std::wstring spool = spool_path();
    if (spool.empty()) return false;

    const std::string line = format_event(id, tag, metrics, install, MV_APP_VERSION) + "\n";

    std::lock_guard<std::mutex> lock(g_spool_mutex);
    const std::size_t slash = spool.find_last_of(L'\\');
    if (slash != std::wstring::npos) ::CreateDirectoryW(spool.substr(0, slash).c_str(), nullptr);
    HANDLE file = ::CreateFileW(spool.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written,
                                nullptr);
    ::CloseHandle(file);
    return ok != FALSE && written == line.size();
  } catch (...) {
    return false;
  }
}

}  // namespace mv::shell::telemetry
