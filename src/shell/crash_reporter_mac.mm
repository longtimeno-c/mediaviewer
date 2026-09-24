// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/crash_reporter_mac.h"

#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <client/annotation.h>
#include <client/annotation_list.h>
#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/crashpad_info.h>
#include <client/settings.h>

#include "core/crash_context.h"
#include "core/trace.h"
#include "shell/minidump_scrub.h"

#ifndef MV_APP_VERSION
#define MV_APP_VERSION "0.0.0"
#endif

namespace mv::shell::crash {
namespace {

std::atomic<std::uint64_t> g_next_call{1};
std::string g_chrome_dir;  // Crashes/chrome, fixed at start (read by the exception handler)
std::vector<std::string> g_identities;
NSUncaughtExceptionHandler* g_previous_handler = nullptr;
// What the dump says about an exception: its (scrubbed, truncated) name and
// the call it happened in. A fixed buffer the handler owns.
crashpad::StringAnnotation<192>* g_exception_annotation = nullptr;

double now_ms() noexcept {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string exe_dir() {
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  buf.resize(std::strlen(buf.c_str()));
  char resolved[PATH_MAX]{};
  if (::realpath(buf.c_str(), resolved)) buf = resolved;
  const auto slash = buf.find_last_of('/');
  return slash == std::string::npos ? std::string{} : buf.substr(0, slash);
}

bool is_executable(const std::string& path) noexcept {
  return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

// MediaViewer.app/Contents/MacOS/MediaViewer -> ../Helpers/crashpad_handler;
// mediaviewer_lab -> beside it.
std::string find_handler() {
  const std::string dir = exe_dir();
  if (dir.empty()) return {};
  for (const std::string& candidate :
       {dir + "/../Helpers/crashpad_handler", dir + "/crashpad_handler"}) {
    if (is_executable(candidate)) return candidate;
  }
  return {};
}

bool make_dirs(const std::string& path) noexcept {
  std::string partial;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || (path[i] == '/' && i > 0)) {
      partial = path.substr(0, i);
      if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

void register_annotations() {
  crashpad::AnnotationList::Register();
  static constexpr const char* kNames[] = {"mv_decode_0", "mv_decode_1", "mv_decode_2",
                                           "mv_decode_3", "mv_decode_4", "mv_decode_5",
                                           "mv_decode_6", "mv_decode_7"};
  const auto slots = mv::crash_context::slots();
  for (std::size_t i = 0; i < mv::crash_context::kSlotCount && i < 8; ++i) {
    // Leaked on purpose: annotations must outlive every thread that can crash.
    auto* a = new crashpad::Annotation(crashpad::Annotation::Type::kString, kNames[i],
                                       slots.data() + i * mv::crash_context::kSlotBytes);
    a->SetSize(static_cast<crashpad::Annotation::ValueSizeType>(mv::crash_context::kSlotBytes));
  }
  auto* last = new crashpad::Annotation(
      crashpad::Annotation::UserDefinedType(1), "mv_last_call_cid",
      const_cast<std::uint64_t*>(mv::crash_context::last_call_address()));
  last->SetSize(sizeof(std::uint64_t));
  g_exception_annotation = new crashpad::StringAnnotation<192>("mv_exception");
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty() && out.size() < (1ull << 31);
}

// Atomic: an exit mid-scrub leaves the old dump, never half of one.
bool replace_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string tmp = path + ".scrub.tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  bool ok = ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()) &&
            ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) ::unlink(tmp.c_str());
  return ok;
}

void collect_dumps(const std::string& dir, std::vector<std::string>& out, int depth) {
  if (depth > 3) return;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      collect_dumps(path, out, depth + 1);
    } else if (S_ISREG(st.st_mode) && name.size() > 4 && name.compare(name.size() - 4, 4, ".dmp") == 0) {
      out.push_back(path);
    }
  }
  ::closedir(d);
}

void background_pass(std::string db_dir) {
  const auto database = crashpad::CrashReportDatabase::Initialize(base::FilePath(db_dir));
  if (!database) {
    MV_LOG_WARN("crash: report database unavailable; reports will not be kept");
    return;
  }
  // No endpoint exists (plan/13): uploads stay off, and no consent prompt.
  if (auto* settings = database->GetSettings()) (void)settings->SetUploadsEnabled(false);
  std::size_t pending = 0;
  std::vector<crashpad::CrashReportDatabase::Report> reports;
  if (database->GetPendingReports(&reports) == crashpad::CrashReportDatabase::kNoError) {
    pending = reports.size();
  }
  const std::size_t rewritten = scrub_reports_in(db_dir, local_identities());
  MV_LOG_INFO("crash: %zu pending report(s), %zu scrubbed this launch, uploads disabled", pending,
              rewritten);
}

// ---- the chrome capture path ----------------------------------------------

void write_all(int fd, const std::string& s) noexcept {
  const char* p = s.data();
  std::size_t left = s.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n <= 0) return;
    p += n;
    left -= static_cast<std::size_t>(n);
  }
}

void on_uncaught_exception(NSException* exception) {
  const std::uint64_t cid = *mv::crash_context::last_call_address();
  scrub_options opt;
  opt.identities = g_identities;
  const std::string name = scrub_text(exception.name ? exception.name.UTF8String : "?", opt);
  const std::string reason = scrub_text(exception.reason ? exception.reason.UTF8String : "", opt);
  // In the dump first: it is written when abort() follows, whatever happens here.
  if (g_exception_annotation) {
    const std::string note = "NSException " + name.substr(0, 120) + " cid=" + std::to_string(cid);
    g_exception_annotation->Set(note.c_str());
  }
  if (!g_chrome_dir.empty()) {
    struct timeval tv{};
    ::gettimeofday(&tv, nullptr);
    const long long ms = static_cast<long long>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    const std::string path =
        g_chrome_dir + "/" + std::to_string(ms) + "-cid" + std::to_string(cid) + "-nsexception.txt";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      std::string text = "MediaViewer " MV_APP_VERSION " chrome exception (macOS)\n";
      text += "cid=" + std::to_string(cid) + "\n";
      text += "name=" + name + "\n";
      text += "reason=" + reason + "\n";
      text += "stack:\n";
      for (NSString* frame in exception.callStackSymbols) {
        text += "  " + scrub_text(frame.UTF8String ? frame.UTF8String : "", opt) + "\n";
      }
      write_all(fd, text);
      ::fsync(fd);
      ::close(fd);
    }
  }
  MV_LOG_ERROR("crash: uncaught NSException (cid %llu)", static_cast<unsigned long long>(cid));
  if (g_previous_handler) g_previous_handler(exception);
  // Returning lets the runtime abort(): Crashpad writes the minidump.
}

void install_chrome_capture() {
  // AppKit otherwise catches an exception raised inside event handling, logs
  // it and carries on in an unknown state (plan/13: capture it instead).
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"NSApplicationCrashOnExceptions" : @YES}];
  g_previous_handler = NSGetUncaughtExceptionHandler();
  NSSetUncaughtExceptionHandler(&on_uncaught_exception);
}

}  // namespace

std::string crashes_dir() {
  NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                                 NSUserDomainMask, YES);
  if (dirs.count == 0) return {};
  return std::string(dirs.firstObject.UTF8String) + "/MediaViewer/Crashes";
}

std::vector<std::string> local_identities() {
  std::vector<std::string> out;
  auto add = [&](NSString* s) {
    if (s.length > 0) out.emplace_back(s.UTF8String);
  };
  add(NSUserName());
  add(NSFullUserName());
  // The Sharing name ("Alice's MacBook Pro"). Not NSHost: that can block on a
  // name lookup, and this runs on the main thread at launch.
  if (CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr)) {
    add((__bridge NSString*)name);
    CFRelease(name);
  }
  char host[256]{};
  if (::gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') out.emplace_back(host);
  return out;
}

std::size_t scrub_reports_in(const std::string& dir,
                             const std::vector<std::string>& identities) noexcept {
  try {
    std::vector<std::string> dumps;
    collect_dumps(dir, dumps, 0);
    scrub_options options;
    options.identities = identities;
    std::size_t rewritten = 0;
    for (const std::string& path : dumps) {
      std::vector<std::uint8_t> bytes;
      if (!read_file(path, bytes) || minidump_is_scrubbed(bytes)) continue;
      const scrub_report r = scrub_minidump(bytes, options);
      if (r.valid && replace_file(path, bytes)) ++rewritten;
    }
    return rewritten;
  } catch (...) {
    return 0;
  }
}

std::uint64_t note_native_call() noexcept {
  const std::uint64_t id = g_next_call.fetch_add(1, std::memory_order_relaxed);
  mv::crash_context::note_call(id);
  return id;
}

std::string armed_chrome_test() {
  const char* v = std::getenv("MV_CRASH_TEST");
  if (!v) return {};
  const std::string s = v;
  return (s == "nsexception" || s == "swift_trap") ? s : std::string{};
}

start_result start() noexcept {
  start_result result;
  const double t0 = now_ms();
  try {
    g_identities = local_identities();
    const std::string handler = find_handler();
    const std::string db = crashes_dir();
    result.handler_found = !handler.empty();
    if (!db.empty() && make_dirs(db + "/chrome")) g_chrome_dir = db + "/chrome";
    // The chrome path does not need the handler: install it regardless.
    install_chrome_capture();
    if (!result.handler_found || db.empty()) {
      MV_LOG_WARN("crash: crashpad_handler not found; native crash reporting is off");
    } else {
      auto* info = crashpad::CrashpadInfo::GetCrashpadInfo();
      // plan/13: stacks, contexts, modules — no heap walk, no extra ranges,
      // and no hand-off to ReportCrash (which would keep an unscrubbed copy).
      info->set_gather_indirectly_referenced_memory(crashpad::TriState::kDisabled, 0);
      info->set_system_crash_reporter_forwarding(crashpad::TriState::kDisabled);
      info->set_crashpad_handler_behavior(crashpad::TriState::kEnabled);
      register_annotations();

      const std::map<std::string, std::string> annotations = {
          {"prod", "MediaViewer"},
          {"ver", MV_APP_VERSION},
          {"plat", "macOS"},
      };
      static crashpad::CrashpadClient client;
      // url "" — see the header. restartable: a handler that dies is started
      // again. asynchronous_start is Windows-only in Crashpad.
      result.started = client.StartHandler(base::FilePath(handler), base::FilePath(db),
                                           base::FilePath(), std::string(), annotations,
                                           {"--no-rate-limit"}, true, false);
      if (!result.started) MV_LOG_WARN("crash: handler did not start; crash reporting is off");
      // Detached: an exit mid-pass is safe (replace_file is an atomic rename).
      std::thread([db] {
        try {
          background_pass(db);
        } catch (...) {
        }
      }).detach();
    }
  } catch (...) {
    MV_LOG_WARN("crash: reporter setup failed; crash reporting is off");
  }
  result.elapsed_ms = now_ms() - t0;
  MV_LOG_INFO("crash: reporter %s in %.2f ms", result.started ? "armed" : "not armed",
              result.elapsed_ms);
  return result;
}

}  // namespace mv::shell::crash
