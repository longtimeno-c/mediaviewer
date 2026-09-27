// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/crash_reporter_win.h"

#include <windows.h>
#include <shlobj.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <client/annotation.h>
#include <client/annotation_list.h>
#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/crashpad_info.h>
#include <client/settings.h>

#include "core/trace.h"
#include "mediaviewer/mediaviewer_crash.h"
#include "shell/minidump_scrub.h"
#include "shell/settings.h"

#ifndef MV_APP_VERSION
#define MV_APP_VERSION "0.0.0"
#endif

namespace mv::shell::crash {
namespace {

crashpad::CrashpadClient* g_client = nullptr;

std::string utf8(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

std::wstring exe_dir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  const auto slash = buf.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : buf.substr(0, slash);
}

void register_annotations() {
  mv_crash_context_info info{};
  if (mv_crash_context(&info) != MV_OK || !info.slots) return;
  crashpad::AnnotationList::Register();
  static constexpr const char* kNames[] = {"mv_decode_0", "mv_decode_1", "mv_decode_2",
                                           "mv_decode_3", "mv_decode_4", "mv_decode_5",
                                           "mv_decode_6", "mv_decode_7"};
  const std::uint32_t count = info.slot_count < 8 ? info.slot_count : 8;
  for (std::uint32_t i = 0; i < count; ++i) {
    // Leaked on purpose: annotations must outlive every thread that can crash.
    // The value lives in the core DLL; the handler reads it at crash time.
    auto* a = new crashpad::Annotation(
        crashpad::Annotation::Type::kString, kNames[i],
        const_cast<char*>(info.slots) + static_cast<std::size_t>(i) * info.slot_bytes);
    a->SetSize(info.slot_bytes);
  }
  auto* last = new crashpad::Annotation(crashpad::Annotation::UserDefinedType(1),
                                        "mv_last_call_cid",
                                        const_cast<std::uint64_t*>(info.last_call_correlation_id));
  last->SetSize(sizeof(std::uint64_t));
}

bool read_file(const std::wstring& path, std::vector<std::uint8_t>& out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = ::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31);
  if (ok) {
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ::ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
         read == out.size();
  }
  ::CloseHandle(f);
  return ok;
}

bool replace_file(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
  const std::wstring tmp = path + L".scrub.tmp";
  HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && ::FlushFileBuffers(f);
  ::CloseHandle(f);
  // Atomic replace: an exit mid-scrub leaves the old dump, never half of one.
  if (ok) ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
  if (!ok) ::DeleteFileW(tmp.c_str());
  return ok;
}

void background_pass(std::wstring db_dir, bool uploads_allowed, bool url_configured) {
  const auto database = crashpad::CrashReportDatabase::Initialize(base::FilePath(db_dir));
  if (!database) {
    MV_LOG_WARN("crash: report database unavailable; reports will not be kept");
    return;
  }
  if (auto* settings = database->GetSettings()) (void)settings->SetUploadsEnabled(uploads_allowed);

  std::size_t pending = 0;
  std::vector<crashpad::CrashReportDatabase::Report> reports;
  if (database->GetPendingReports(&reports) == crashpad::CrashReportDatabase::kNoError) {
    pending = reports.size();
  }
  const std::size_t rewritten = scrub_reports_in(db_dir + L"\\reports", local_identities());

  const crash_settings cs = load_crash_settings();
  // plan/13: "Ask before the first send, plainly, once." No endpoint exists
  // in PR 7, so this is always false today; the dialog is PR 15 and must not
  // stack with other first-run prompts (plan/09).
  const bool ask = should_ask_crash_consent(cs, pending);
  MV_LOG_INFO("crash: %zu pending report(s), %zu scrubbed this launch, uploads %s%s",
              pending, rewritten, uploads_allowed ? "enabled" : "disabled",
              ask ? ", consent prompt due" : "");
  (void)url_configured;
}

}  // namespace

std::wstring crashes_dir() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
    return {};
  }
  return std::wstring(local) + L"\\MediaViewer\\Crashes";
}

std::vector<std::string> local_identities() {
  std::vector<std::string> out;
  wchar_t name[256]{};
  DWORD n = 256;
  if (::GetUserNameW(name, &n) && n > 1) out.push_back(utf8(name));
  wchar_t host[MAX_COMPUTERNAME_LENGTH + 1]{};
  n = MAX_COMPUTERNAME_LENGTH + 1;
  if (::GetComputerNameW(host, &n) && n > 0) out.push_back(utf8(host));
  return out;
}

std::size_t scrub_reports_in(const std::wstring& reports_dir,
                             const std::vector<std::string>& identities) noexcept {
  try {
    std::size_t rewritten = 0;
    scrub_options options;
    options.identities = identities;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((reports_dir + L"\\*.dmp").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const std::wstring path = reports_dir + L"\\" + fd.cFileName;
      std::vector<std::uint8_t> bytes;
      if (!read_file(path, bytes) || minidump_is_scrubbed(bytes)) continue;
      const scrub_report r = scrub_minidump(bytes, options);
      if (r.valid && replace_file(path, bytes)) ++rewritten;
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
    return rewritten;
  } catch (...) {
    return 0;
  }
}

start_result start() noexcept {
  start_result result;
  LARGE_INTEGER t0{}, t1{}, freq{};
  ::QueryPerformanceFrequency(&freq);
  ::QueryPerformanceCounter(&t0);
  try {
    const std::wstring dir = exe_dir();
    const std::wstring handler = dir + L"\\crashpad_handler.exe";
    const std::wstring db = crashes_dir();
    result.handler_found = !dir.empty() && ::GetFileAttributesW(handler.c_str()) !=
                                               INVALID_FILE_ATTRIBUTES;
    if (!result.handler_found || db.empty()) {
      MV_LOG_WARN("crash: crashpad_handler.exe not found beside the exe; crash reporting is off");
    } else {
      auto* info = crashpad::CrashpadInfo::GetCrashpadInfo();
      // plan/13: stacks, contexts, modules — no heap walk, no extra ranges,
      // and no hand-off to WER (which would upload an unscrubbed dump).
      info->set_gather_indirectly_referenced_memory(crashpad::TriState::kDisabled, 0);
      info->set_system_crash_reporter_forwarding(crashpad::TriState::kDisabled);
      info->set_crashpad_handler_behavior(crashpad::TriState::kEnabled);
      register_annotations();

      const std::map<std::string, std::string> annotations = {
          {"prod", "MediaViewer"},
          {"ver", MV_APP_VERSION},
      };
      static crashpad::CrashpadClient client;
      // url "" — see the header. asynchronous_start: no CreateProcess or pipe
      // handshake on the UI thread.
      result.started = client.StartHandler(base::FilePath(handler), base::FilePath(db),
                                           base::FilePath(), std::string(), annotations,
                                           {"--no-rate-limit"}, false, true);
      if (result.started) g_client = &client;
      else MV_LOG_WARN("crash: handler did not start; crash reporting is off");

      const crash_settings cs = load_crash_settings();
      const bool url = !cs.upload_url.empty();
      const bool uploads = url && cs.consent == 1;
      // Detached: an exit mid-pass is safe (replace_file is an atomic rename),
      // and a joinable global would terminate() on an early return.
      std::thread([db, uploads, url] {
        try {
          background_pass(db, uploads, url);
        } catch (...) {
        }
      }).detach();
    }
  } catch (...) {
    MV_LOG_WARN("crash: reporter setup failed; crash reporting is off");
  }
  ::QueryPerformanceCounter(&t1);
  result.elapsed_ms =
      static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  MV_LOG_INFO("crash: reporter %s in %.2f ms", result.started ? "armed" : "not armed",
              result.elapsed_ms);
  return result;
}

}  // namespace mv::shell::crash
