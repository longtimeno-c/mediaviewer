// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/settings_store.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <new>
#include <utility>

#include "core/trace.h"

namespace mv::shell {
namespace {

char lower_ascii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  }
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

}  // namespace

// ---- settings_doc -------------------------------------------------------------

const std::string* settings_doc::find(std::string_view section_name, std::string_view key) const noexcept {
  for (const auto& s : sections) {
    if (!iequals(s.name, section_name)) continue;
    for (const auto& e : s.entries) {
      if (iequals(e.key, key)) return &e.value;
    }
  }
  return nullptr;
}

void settings_doc::set(std::string_view section_name, std::string_view key, std::string_view value) {
  section* target = nullptr;
  for (auto& s : sections) {
    if (iequals(s.name, section_name)) {
      target = &s;
      break;
    }
  }
  if (!target) {
    sections.push_back(section{std::string(section_name), {}});
    target = &sections.back();
  }
  for (auto& e : target->entries) {
    if (iequals(e.key, key)) {
      e.value.assign(value);
      return;
    }
  }
  target->entries.push_back(entry{std::string(key), std::string(value)});
}

bool settings_doc::erase(std::string_view section_name, std::string_view key) noexcept {
  for (auto it = sections.begin(); it != sections.end(); ++it) {
    if (!iequals(it->name, section_name)) continue;
    for (auto e = it->entries.begin(); e != it->entries.end(); ++e) {
      if (!iequals(e->key, key)) continue;
      it->entries.erase(e);
      if (it->entries.empty()) sections.erase(it);
      return true;
    }
  }
  return false;
}

void settings_doc::erase_prefix(std::string_view section_name, std::string_view prefix) noexcept {
  for (auto it = sections.begin(); it != sections.end(); ++it) {
    if (!iequals(it->name, section_name)) continue;
    std::erase_if(it->entries, [&](const entry& e) { return istarts_with(e.key, prefix); });
    if (it->entries.empty()) sections.erase(it);
    return;
  }
}

std::string settings_doc::get(std::string_view section_name, std::string_view key,
                              std::string_view fallback) const {
  const std::string* v = find(section_name, key);
  return v ? *v : std::string(fallback);
}

int settings_doc::get_int(std::string_view section_name, std::string_view key, int fallback) const noexcept {
  // GetPrivateProfileInt semantics: leading integer, else the fallback.
  const std::string* v = find(section_name, key);
  if (!v || v->empty()) return fallback;
  int out = 0;
  if (std::sscanf(v->c_str(), "%d", &out) != 1) return fallback;
  return out;
}

settings_doc parse_settings_ini(std::string_view text) {
  settings_doc doc;
  settings_doc::section* current = nullptr;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = trim(text.substr(0, nl));
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      const std::size_t close = line.find(']');
      if (close == std::string_view::npos) continue;
      const std::string_view name = trim(line.substr(1, close - 1));
      current = nullptr;
      for (auto& s : doc.sections) {
        if (iequals(s.name, name)) current = &s;
      }
      if (!current) {
        doc.sections.push_back(settings_doc::section{std::string(name), {}});
        current = &doc.sections.back();
      }
      continue;
    }
    if (!current) continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view key = trim(line.substr(0, eq));
    if (key.empty()) continue;
    std::string_view value = trim(line.substr(eq + 1));
    // WritePrivateProfileString never quoted, but GetPrivateProfileString
    // strips one pair of surrounding double quotes. Keep that reading.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    bool replaced = false;
    for (auto& e : current->entries) {
      if (iequals(e.key, key)) {  // first spelling wins, as the profile API did
        replaced = true;
        break;
      }
    }
    if (!replaced) current->entries.push_back(settings_doc::entry{std::string(key), std::string(value)});
  }
  return doc;
}

std::string serialize_settings_ini(const settings_doc& doc) {
  std::string out;
  bool first = true;
  for (const auto& s : doc.sections) {
    if (s.entries.empty()) continue;
    if (!first) out += "\r\n";
    first = false;
    out += '[';
    out += s.name;
    out += "]\r\n";
    for (const auto& e : s.entries) {
      out += e.key;
      out += '=';
      out += e.value;
      out += "\r\n";
    }
  }
  return out;
}

// ---- rule 1 detector ------------------------------------------------------------

namespace {

std::atomic<DWORD> g_ui_thread{0};
std::atomic<std::uint64_t> g_ui_thread_writes{0};
thread_local int t_exit_scope = 0;

std::wstring wide_from_utf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string utf8_from_wide(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

// Files written by WritePrivateProfileStringW (PR 4-7) are ANSI unless they
// began life with a UTF-16 BOM. The store writes UTF-16LE with a BOM, which is
// also what the profile API reads as Unicode, so an older build still reads
// the file this one writes.
std::string decode_settings_bytes(std::string_view bytes) {
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {
    std::wstring wide((bytes.size() - 2) / 2, L'\0');
    for (std::size_t i = 0; i < wide.size(); ++i) {
      wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(bytes[2 + 2 * i]) |
                                     (static_cast<unsigned char>(bytes[3 + 2 * i]) << 8));
    }
    return utf8_from_wide(wide);
  }
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    return std::string(bytes.substr(3));
  }
  if (bytes.empty()) return {};
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                            nullptr, 0) > 0) {
    return std::string(bytes);
  }
  const int n = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), n);
  return utf8_from_wide(wide);
}

std::wstring default_settings_path() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
    return {};
  }
  std::wstring dir = local;
  dir += L"\\MediaViewer";
  if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
    return {};
  }
  return dir + L"\\settings.ini";
}

}  // namespace

void register_settings_ui_thread() noexcept { g_ui_thread.store(::GetCurrentThreadId()); }
std::uint64_t settings_ui_thread_writes() noexcept { return g_ui_thread_writes.load(); }
void reset_settings_ui_thread_detector() noexcept {
  g_ui_thread.store(0);
  g_ui_thread_writes.store(0);
}
settings_exit_write_scope::settings_exit_write_scope() noexcept { ++t_exit_scope; }
settings_exit_write_scope::~settings_exit_write_scope() { --t_exit_scope; }

bool write_settings_file_atomic(const std::wstring& path, std::string_view utf8_text) noexcept {
  const DWORD ui = g_ui_thread.load();
  if (ui != 0 && ui == ::GetCurrentThreadId() && t_exit_scope == 0) {
    g_ui_thread_writes.fetch_add(1);
    MV_LOG_ERROR("settings: settings.ini written on the UI thread (rule 1)");
  }
  if (path.empty()) return false;
  try {
    const std::wstring wide = wide_from_utf8(utf8_text);
    if (!utf8_text.empty() && wide.empty()) return false;
    std::string bytes;
    bytes.reserve(2 + wide.size() * 2);
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (wchar_t c : wide) {
      bytes.push_back(static_cast<char>(c & 0xFF));
      bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    const std::wstring tmp = path + L".tmp";
    HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && ::FlushFileBuffers(f);
    ::CloseHandle(f);
    // The rename is the commit: until it lands, settings.ini is the old file.
    if (ok) {
      ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
    }
    if (!ok) ::DeleteFileW(tmp.c_str());
    return ok;
  } catch (...) {
    return false;
  }
}

// ---- settings_store ---------------------------------------------------------------

settings_store::settings_store(std::wstring path) noexcept : path_(std::move(path)) {
  try {
    current_.store(std::make_shared<const version>());
  } catch (...) {
  }
  settled_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

settings_store::~settings_store() {
  stop();
  if (settled_event_) ::CloseHandle(static_cast<HANDLE>(settled_event_));
}

void settings_store::load_from_disk() noexcept {
  if (path_.empty()) return;
  try {
    HANDLE f = ::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;  // no file yet: defaults
    std::string bytes;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
      bytes.resize(static_cast<std::size_t>(size.QuadPart));
      DWORD read = 0;
      if (!::ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) read = 0;
      bytes.resize(read);
    }
    ::CloseHandle(f);
    auto v = std::make_shared<version>();
    v->doc = parse_settings_ini(decode_settings_bytes(bytes));
    const auto cur = current_.load();
    v->seq = cur ? cur->seq : 0;  // what is on disk is, by definition, settled
    settled_seq_.store(v->seq);
    current_.store(std::move(v));
  } catch (...) {
    MV_LOG_WARN("settings: could not read settings.ini; using defaults");
  }
}

bool settings_store::start() noexcept {
  if (running_.load()) return true;
  // One thread of the shared job system, not the file_jobs worker: a card-dump
  // move there can run for minutes, and its stop() drops queued jobs — a
  // settings write must neither wait behind a copy nor be dropped at exit.
  if (pool_.start(1) != mv::status::ok) {
    MV_LOG_WARN("settings: persist worker did not start; settings are written at exit");
    return false;
  }
  running_.store(true);
  // Anything set before the worker existed.
  const auto cur = current_.load();
  if (cur && cur->seq > settled_seq_.load()) publish(cur);
  return true;
}

std::shared_ptr<const settings_doc> settings_store::snapshot() const noexcept {
  auto v = current_.load();
  if (!v) return {};
  return std::shared_ptr<const settings_doc>(v, &v->doc);
}

std::string settings_store::get(std::string_view section, std::string_view key,
                                std::string_view fallback) const {
  const auto v = current_.load();
  return v ? v->doc.get(section, key, fallback) : std::string(fallback);
}

int settings_store::get_int(std::string_view section, std::string_view key, int fallback) const noexcept {
  const auto v = current_.load();
  return v ? v->doc.get_int(section, key, fallback) : fallback;
}

void settings_store::set(std::string_view section, std::string_view key, std::string_view value) noexcept {
  update([&](settings_doc& d) { d.set(section, key, value); });
}

void settings_store::set_int(std::string_view section, std::string_view key, int value) noexcept {
  char buf[16]{};
  (void)std::snprintf(buf, sizeof buf, "%d", value);
  set(section, key, buf);
}

void settings_store::erase(std::string_view section, std::string_view key) noexcept {
  update([&](settings_doc& d) { d.erase(section, key); });
}

void settings_store::update(const std::function<void(settings_doc&)>& mutate) noexcept {
  try {
    auto cur = current_.load();
    for (;;) {
      auto next = std::make_shared<version>();
      if (cur) next->doc = cur->doc;
      mutate(next->doc);
      if (cur && next->doc == cur->doc) return;  // nothing changed, nothing to write
      next->seq = (cur ? cur->seq : 0) + 1;
      std::shared_ptr<const version> published = next;
      // Read-copy-update: a concurrent mutation from another thread makes the
      // CAS fail, and this one is re-applied on top of it.
      if (current_.compare_exchange_strong(cur, published)) {
        publish(std::move(published));
        return;
      }
    }
  } catch (...) {
    MV_LOG_WARN("settings: out of memory updating settings; change not kept");
  }
}

void settings_store::publish(std::shared_ptr<const version> v) noexcept {
  auto old = pending_.load();
  do {
    if (old && old->seq >= v->seq) break;  // a newer snapshot is already waiting
  } while (!pending_.compare_exchange_weak(old, v));
  schedule();
}

void settings_store::schedule() noexcept {
  if (!running_.load()) return;
  if (job_queued_.exchange(true)) return;  // the queued job will take the newest
  const mv::job_id id = pool_.submit_at(mv::background_generation, [this](const mv::job_context&) {
    persist_pending();
    return mv::status::ok;
  });
  if (id == mv::invalid_job) job_queued_.store(false);
}

void settings_store::persist_pending() noexcept {
  // Clear before taking: a snapshot published after this line schedules a new
  // job; one published before it is taken below.
  job_queued_.store(false);
  const auto v = pending_.exchange(nullptr);
  if (v && v->seq > settled_seq_.load()) {
    bool ok = true;
    if (!path_.empty()) {
      try {
        ok = write_settings_file_atomic(path_, serialize_settings_ini(v->doc));
      } catch (...) {
        ok = false;
      }
    }
    if (ok) {
      writes_ok_.fetch_add(1);
    } else {
      writes_failed_.fetch_add(1);
      failed_seq_.store(v->seq);
      MV_LOG_WARN("settings: settings.ini write failed; kept in memory, retried on the next change or at exit");
    }
    settled_seq_.store(v->seq);
  }
  if (settled_event_) ::SetEvent(static_cast<HANDLE>(settled_event_));
}

bool settings_store::flush(unsigned timeout_ms) noexcept {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  const auto settled_clean = [this](std::uint64_t target) noexcept {
    const std::uint64_t settled = settled_seq_.load();
    return settled >= target && failed_seq_.load() != settled;
  };
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto cur = current_.load();
    if (!cur) return false;
    if (settled_clean(cur->seq)) return true;

    if (settled_seq_.load() >= cur->seq) {
      // The newest attempt failed. Publish the same document under a fresh
      // seq so it is tried once more.
      try {
        auto again = std::make_shared<version>();
        again->doc = cur->doc;
        again->seq = cur->seq + 1;
        std::shared_ptr<const version> next = again;
        std::shared_ptr<const version> expected = cur;
        if (current_.compare_exchange_strong(expected, next)) {
          cur = next;
          publish(next);
        } else {
          cur = expected;  // a newer change raced in; it is already published
        }
      } catch (...) {
        return false;
      }
    }

    if (!running_.load()) {
      // No worker (it failed to start, or stop() already ran). This is the
      // exit path, which is allowed to write on the calling thread.
      settings_exit_write_scope scope;
      pending_.store(current_.load());
      persist_pending();
      continue;
    }

    for (;;) {
      const std::uint64_t target = current_.load()->seq;
      if (settled_seq_.load() >= target) break;
      if (settled_event_) ::ResetEvent(static_cast<HANDLE>(settled_event_));
      if (settled_seq_.load() >= target) break;
      const ULONGLONG now = ::GetTickCount64();
      if (now >= deadline) return false;
      if (settled_event_) {
        (void)::WaitForSingleObject(static_cast<HANDLE>(settled_event_), static_cast<DWORD>(deadline - now));
      } else {
        ::Sleep(1);
      }
    }
  }
  return settled_clean(current_.load()->seq);
}

void settings_store::stop() noexcept {
  if (!running_.exchange(false)) return;
  pool_.shutdown();
  job_queued_.store(false);
}

settings_store& app_settings() noexcept {
  // Leaked on purpose: no static-destruction-order question with the logger,
  // and the host has already flushed and stopped it on the exit path.
  static settings_store* store = [] {
    auto* s = new (std::nothrow) settings_store(default_settings_path());
    if (s) s->load_from_disk();
    return s;
  }();
  if (!store) {
    static settings_store fallback;
    return fallback;
  }
  return *store;
}

}  // namespace mv::shell
