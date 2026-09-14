// SPDX-License-Identifier: GPL-2.0-or-later
// PR 7 verify clause: "nothing in the broken corpus crashes or hangs".
//
// For every seed in tests/data/seeds/<family>/ this derives hostile variants
// deterministically — truncation, byte stomps and bit flips, length fields set
// to 0 / 0xFFFFFFFF, absurd dimensions patched into the real header fields, a
// valid header followed by garbage — and feeds each through every public
// decode entry point: codec::probe, codec::decode, the family's own decoder,
// decode_raw_preview, looks_like_raw, image::decode_bytes,
// image::decode_preview, and open_animation with every frame stepped. The
// hand-crafted files in tests/data/broken/ (decompression bombs, IFD loops,
// 20 000-frame GIFs, overlapping ICO entries) run as-is and truncated.
//
// Pass = every call returns, ok or error, and:
//   * no crash (an SEH fault names the variant on stderr before Catch2 or the
//     OS reports it; MV_BROKEN_DUMP=<dir> also writes its bytes there),
//   * no C++ exception escapes the decoder,
//   * a successful result is self-consistent (rgba.size() == w*h*4, w,h > 0),
//   * each call finishes inside the time budget: 5 s per call in an optimised
//     build, 60 s in Debug / ASan (MV_BROKEN_TIMEOUT_MS overrides). A hung
//     call cannot be abandoned safely, so an overrun prints the variant,
//     dumps it, and ends the process with exit code 3 — ctest reports a
//     failure, not a pass that never finished,
//   * private commit grows by no more than MV_BROKEN_MEM_MB (default 2560)
//     during one call, sampled every 5 ms. That is above what the decoders
//     legitimately allow (256 MP * RGBA8 = 1 GiB raster, plus the display
//     copy), so it catches unbounded growth, not the documented cap. In a
//     non-ASan build the process also runs in a Job Object with a hard
//     commit limit (MV_BROKEN_JOB_MB, default 6144): a runaway allocation
//     fails inside the decoder — exercising its out_of_memory path — instead
//     of paging the machine to death. ASan reserves and commits shadow memory
//     of its own, so the hard limit is left to ASan's allocator there.
//
// This is a separate executable (mv_broken_tests) so the Job Object and the
// vectored exception handler apply to nothing but this suite.
//
// Stub decoders (PR 7 in progress) simply return unsupported_format: the suite
// passes against them today and exercises the real decoders once they land.
// MV_BROKEN_FULL=1 widens every sweep (every truncation offset up to 64 KiB,
// every header offset up to 512); CI's nightly fuzz job sets it.
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "codec/decode.h"
#include "image/pipeline.h"

namespace fs = std::filesystem;
using mv::codec::format_family;

namespace {

// ---------------------------------------------------------------------------
// Budgets
// ---------------------------------------------------------------------------
std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(v, &end, 10);
  return end && *end == '\0' ? parsed : fallback;
}

bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif
#else
constexpr bool kAsan = false;
#endif

#if defined(NDEBUG)
constexpr bool kOptimised = !kAsan;
#else
constexpr bool kOptimised = false;
#endif

std::uint64_t timeout_ms() {
  return env_u64("MV_BROKEN_TIMEOUT_MS", kOptimised ? 5000 : 60000);
}
std::uint64_t mem_budget_bytes() { return env_u64("MV_BROKEN_MEM_MB", 2560) << 20; }
bool full_sweep() { return env_flag("MV_BROKEN_FULL"); }

// ---------------------------------------------------------------------------
// Crash attribution. The variant under test is published here before every
// call; a vectored handler (first in the chain) prints it on a fatal SEH code
// and, with MV_BROKEN_DUMP, writes its bytes with raw Win32 calls — no CRT
// allocation inside a faulting process. It returns CONTINUE_SEARCH, so Catch2
// and the OS still report the crash as a failure.
// ---------------------------------------------------------------------------
char g_current_name[512] = "";
std::atomic<const std::uint8_t*> g_current_data{nullptr};
std::atomic<std::size_t> g_current_size{0};
char g_dump_dir[MAX_PATH] = "";

void dump_bytes(const char* name, const std::uint8_t* data, std::size_t size) {
  if (!g_dump_dir[0] || !data) return;
  char path[MAX_PATH + 600];
  int n = std::snprintf(path, sizeof(path), "%s\\", g_dump_dir);
  for (const char* p = name; *p && n < static_cast<int>(sizeof(path)) - 8; ++p) {
    const char c = *p;
    path[n++] = (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')
                    ? c
                    : '_';
  }
  std::memcpy(path + n, ".bin", 5);
  HANDLE f = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(f, data, static_cast<DWORD>(std::min<std::size_t>(size, 0x7FFFFFFF)), &written,
              nullptr);
  ::CloseHandle(f);
}

LONG CALLBACK on_fatal_seh(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case STATUS_HEAP_CORRUPTION:
    case STATUS_STACK_BUFFER_OVERRUN:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true)) {
    std::fprintf(stderr, "\n[broken-corpus] FATAL SEH 0x%08lX while decoding: %s\n",
                 static_cast<unsigned long>(code), g_current_name);
    std::fflush(stderr);
    dump_bytes(g_current_name, g_current_data.load(), g_current_size.load());
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Process-wide setup once per test run: handler, dump dir, Job Object.
class broken_corpus_listener final : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(const Catch::TestRunInfo&) override {
    ::AddVectoredExceptionHandler(1, &on_fatal_seh);
    if (const char* dir = std::getenv("MV_BROKEN_DUMP"); dir && *dir) {
      std::error_code ec;
      fs::create_directories(dir, ec);
      std::snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dir);
    }
    if (!kAsan) {
      const std::uint64_t cap = env_u64("MV_BROKEN_JOB_MB", 6144) << 20;
      if (cap != 0) {
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(cap);
        // Nested jobs are fine on Windows 8+; if the runner already put us in
        // a job that forbids it, run without the hard cap rather than fail.
        if (job && ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                             sizeof(limits))) {
          ::AssignProcessToJobObject(job, ::GetCurrentProcess());
        }
        // The handle is deliberately leaked: closing it would not lift the
        // limit, and the job must live as long as the process.
      }
    }
  }
};
CATCH_REGISTER_LISTENER(broken_corpus_listener)

std::uint64_t private_bytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (!::GetProcessMemoryInfo(::GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    return 0;
  }
  return pmc.PrivateUsage;
}

// ---------------------------------------------------------------------------
// Watchdog: one persistent worker runs each call; the test thread waits with
// a deadline and samples memory while it waits.
// ---------------------------------------------------------------------------
class watchdog {
 public:
  watchdog() : worker_([this] { loop(); }) {}
  ~watchdog() {
    {
      std::lock_guard lock(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  watchdog(const watchdog&) = delete;
  watchdog& operator=(const watchdog&) = delete;

  // Runs `fn` on the worker. Appends to `failures` on an exception, a memory
  // overrun, or a false return from `fn` (an invariant). Never returns on a
  // timeout.
  void run(const std::string& name, std::span<const std::uint8_t> bytes,
           const std::function<std::string()>& fn, std::vector<std::string>& failures) {
    std::snprintf(g_current_name, sizeof(g_current_name), "%s", name.c_str());
    g_current_data = bytes.data();
    g_current_size = bytes.size();

    const std::uint64_t baseline = private_bytes();
    std::uint64_t peak = baseline;
    {
      std::lock_guard lock(mu_);
      job_ = &fn;
      done_ = false;
      outcome_.clear();
    }
    cv_.notify_all();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms());
    std::unique_lock lock(mu_);
    while (!done_) {
      cv_.wait_for(lock, std::chrono::milliseconds(5));
      peak = std::max(peak, private_bytes());
      if (!done_ && std::chrono::steady_clock::now() > deadline) {
        std::fprintf(stderr,
                     "\n[broken-corpus] HANG: %s did not return within %llu ms "
                     "(MV_BROKEN_TIMEOUT_MS). Exiting.\n",
                     name.c_str(), static_cast<unsigned long long>(timeout_ms()));
        std::fflush(stderr);
        dump_bytes(g_current_name, bytes.data(), bytes.size());
        std::_Exit(3);
      }
    }
    if (!outcome_.empty()) failures.push_back(name + ": " + outcome_);
    if (peak > baseline && peak - baseline > mem_budget_bytes()) {
      failures.push_back(name + ": private commit grew by " +
                         std::to_string((peak - baseline) >> 20) + " MiB (budget " +
                         std::to_string(mem_budget_bytes() >> 20) + " MiB)");
    }
    if (!outcome_.empty() || (peak - baseline > mem_budget_bytes())) {
      dump_bytes(g_current_name, bytes.data(), bytes.size());
    }
    g_current_data = nullptr;
    g_current_size = 0;
  }

 private:
  void loop() {
    std::unique_lock lock(mu_);
    for (;;) {
      cv_.wait(lock, [this] { return quit_ || job_ != nullptr; });
      if (quit_) return;
      const auto* job = job_;
      job_ = nullptr;
      lock.unlock();
      std::string outcome;
      try {
        outcome = (*job)();
      } catch (const std::exception& e) {
        outcome = std::string("C++ exception escaped: ") + e.what();
      } catch (...) {
        outcome = "non-std C++ exception escaped";
      }
      lock.lock();
      outcome_ = std::move(outcome);
      done_ = true;
      cv_.notify_all();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  const std::function<std::string()>* job_ = nullptr;
  std::string outcome_;
  bool done_ = true;
  bool quit_ = false;
  std::thread worker_;
};

// ---------------------------------------------------------------------------
// Corpus files
// ---------------------------------------------------------------------------
fs::path data_dir() {
#ifdef MV_TEST_DATA_DIR
  return fs::path(MV_TEST_DATA_DIR);
#else
  return fs::current_path() / "tests" / "data";
#endif
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::vector<std::uint8_t> out;
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    out.resize(static_cast<std::size_t>(size));
    out.resize(std::fread(out.data(), 1, out.size(), f));
  }
  std::fclose(f);
  return out;
}

struct corpus_file {
  std::string name;  // "jpeg/baseline.jpg"
  std::vector<std::uint8_t> bytes;
};

std::vector<corpus_file> files_in(const fs::path& dir, const std::string& prefix) {
  std::vector<corpus_file> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string leaf = entry.path().filename().string();
    if (leaf == "README.md") continue;
    out.push_back({prefix + leaf, read_file(entry.path())});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

// ---------------------------------------------------------------------------
// Deterministic mutation
// ---------------------------------------------------------------------------
struct splitmix64 {
  std::uint64_t state;
  std::uint64_t next() {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

std::uint64_t fnv1a(std::string_view s) {
  std::uint64_t h = 1469598103934665603ull;
  for (const char c : s) h = (h ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
  return h;
}

struct field_site {
  std::size_t offset;
  std::uint8_t width;  // bytes: 1, 2, 3, 4
  bool big_endian;
  const char* what;
  std::uint32_t bias = 0;  // WebP VP8X stores (dimension - 1)
};

std::uint32_t rd(std::span<const std::uint8_t> b, std::size_t off, int width, bool be) {
  if (off + static_cast<std::size_t>(width) > b.size()) return 0;
  std::uint32_t v = 0;
  for (int i = 0; i < width; ++i) {
    const std::uint32_t byte = b[off + static_cast<std::size_t>(i)];
    v |= be ? byte << (8 * (width - 1 - i)) : byte << (8 * i);
  }
  return v;
}

void wr(std::vector<std::uint8_t>& b, std::size_t off, int width, bool be, std::uint32_t v) {
  if (off + static_cast<std::size_t>(width) > b.size()) return;
  for (int i = 0; i < width; ++i) {
    const int shift = be ? 8 * (width - 1 - i) : 8 * i;
    b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> shift);
  }
}

// Where the real length and dimension fields live, per family, so the patched
// values land in the fields decoders trust rather than at random.
std::vector<field_site> dimension_sites(std::span<const std::uint8_t> b) {
  std::vector<field_site> sites;
  const auto family = mv::codec::probe(b);
  const auto has = [&](std::size_t off, const char* tag) {
    return off + 4 <= b.size() && std::memcmp(b.data() + off, tag, 4) == 0;
  };
  switch (family) {
    case format_family::png: {
      // Every chunk length; IHDR w/h; acTL frame count; fcTL w/h/x/y.
      std::size_t off = 8;
      while (off + 8 <= b.size()) {
        const std::uint32_t len = rd(b, off, 4, true);
        sites.push_back({off, 4, true, "chunk-length"});
        if (has(off + 4, "IHDR")) {
          sites.push_back({off + 8, 4, true, "ihdr-width"});
          sites.push_back({off + 12, 4, true, "ihdr-height"});
        } else if (has(off + 4, "acTL")) {
          sites.push_back({off + 8, 4, true, "actl-frames"});
          sites.push_back({off + 12, 4, true, "actl-plays"});
        } else if (has(off + 4, "fcTL")) {
          sites.push_back({off + 12, 4, true, "fctl-width"});
          sites.push_back({off + 16, 4, true, "fctl-height"});
          sites.push_back({off + 20, 4, true, "fctl-x"});
          sites.push_back({off + 24, 4, true, "fctl-y"});
        }
        if (len > b.size()) break;
        off += 12 + static_cast<std::size_t>(len);
      }
      break;
    }
    case format_family::gif: {
      sites.push_back({6, 2, false, "screen-width"});
      sites.push_back({8, 2, false, "screen-height"});
      // Image descriptors: a 0x2C preceded by a GCE terminator or at the top.
      for (std::size_t i = 13; i + 10 <= b.size() && sites.size() < 24; ++i) {
        if (b[i] == 0x2C && b[i - 1] == 0x00) {
          sites.push_back({i + 1, 2, false, "frame-left"});
          sites.push_back({i + 3, 2, false, "frame-top"});
          sites.push_back({i + 5, 2, false, "frame-width"});
          sites.push_back({i + 7, 2, false, "frame-height"});
        }
      }
      break;
    }
    case format_family::bmp:
      sites.push_back({2, 4, false, "file-size"});
      sites.push_back({10, 4, false, "pixel-offset"});
      sites.push_back({14, 4, false, "dib-size"});
      sites.push_back({18, 4, false, "width"});
      sites.push_back({22, 4, false, "height"});
      sites.push_back({28, 2, false, "bpp"});
      sites.push_back({34, 4, false, "image-size"});
      break;
    case format_family::webp: {
      sites.push_back({4, 4, false, "riff-size"});
      std::size_t off = 12;
      while (off + 8 <= b.size()) {
        const std::uint32_t len = rd(b, off + 4, 4, false);
        sites.push_back({off + 4, 4, false, "chunk-size"});
        if (has(off, "VP8X")) {
          sites.push_back({off + 12, 3, false, "vp8x-width", 1});
          sites.push_back({off + 15, 3, false, "vp8x-height", 1});
        } else if (has(off, "ANMF")) {
          sites.push_back({off + 8, 3, false, "anmf-x"});
          sites.push_back({off + 11, 3, false, "anmf-y"});
          sites.push_back({off + 14, 3, false, "anmf-width", 1});
          sites.push_back({off + 17, 3, false, "anmf-height", 1});
        } else if (has(off, "VP8 ")) {
          sites.push_back({off + 14, 2, false, "vp8-width"});
          sites.push_back({off + 16, 2, false, "vp8-height"});
        }
        if (len > b.size()) break;
        const std::size_t next = off + 8 + static_cast<std::size_t>(len) + (len & 1u);
        // Descend into ANMF: its sub-chunks start after the 16-byte frame header.
        off = has(off, "ANMF") ? off + 24 : next;
      }
      break;
    }
    case format_family::jpeg: {
      std::size_t i = 2;
      while (i + 4 <= b.size() && b[i] == 0xFF) {
        const std::uint8_t marker = b[i + 1];
        const std::uint32_t seg = rd(b, i + 2, 2, true);
        sites.push_back({i + 2, 2, true, "segment-length"});
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
          sites.push_back({i + 5, 2, true, "sof-height"});
          sites.push_back({i + 7, 2, true, "sof-width"});
          sites.push_back({i + 9, 1, true, "sof-components"});
        }
        if (marker == 0xDA) break;
        i += 2 + seg;
      }
      break;
    }
    case format_family::ico: {
      const std::uint32_t count = rd(b, 4, 2, false);
      sites.push_back({4, 2, false, "entry-count"});
      for (std::uint32_t e = 0; e < std::min<std::uint32_t>(count, 8); ++e) {
        const std::size_t d = 6 + 16 * static_cast<std::size_t>(e);
        sites.push_back({d, 1, false, "entry-width"});
        sites.push_back({d + 1, 1, false, "entry-height"});
        sites.push_back({d + 8, 4, false, "entry-size"});
        sites.push_back({d + 12, 4, false, "entry-offset"});
        const std::uint32_t at = rd(b, d + 12, 4, false);
        if (at + 16 <= b.size() && rd(b, at, 4, false) == 40) {
          sites.push_back({at + 4, 4, false, "dib-width"});
          sites.push_back({at + 8, 4, false, "dib-height"});
        }
      }
      break;
    }
    case format_family::tiff:
    case format_family::raw:
    case format_family::heic:
    case format_family::avif:
    case format_family::unknown:
      break;
  }

  // TIFF-shaped (TIFF, DNG, TIFF RAWs): walk IFD0 and its SubIFDs.
  if (b.size() >= 8 && ((b[0] == 'I' && b[1] == 'I') || (b[0] == 'M' && b[1] == 'M'))) {
    const bool be = b[0] == 'M';
    std::vector<std::uint32_t> ifds{rd(b, 4, 4, be)};
    sites.push_back({4, 4, be, "first-ifd"});
    for (std::size_t k = 0; k < ifds.size() && k < 8; ++k) {
      const std::size_t at = ifds[k];
      if (at + 2 > b.size()) continue;
      const std::uint32_t n = rd(b, at, 2, be);
      sites.push_back({at, 2, be, "ifd-count"});
      for (std::uint32_t e = 0; e < n && at + 2 + 12 * (e + 1) <= b.size(); ++e) {
        const std::size_t ent = at + 2 + 12 * e;
        const std::uint32_t tag = rd(b, ent, 2, be);
        const std::uint32_t type = rd(b, ent + 2, 2, be);
        const std::size_t value = ent + 8;
        const int w = type == 3 ? 2 : 4;
        switch (tag) {
          case 256: sites.push_back({value, static_cast<std::uint8_t>(w), be, "tiff-width"}); break;
          case 257: sites.push_back({value, static_cast<std::uint8_t>(w), be, "tiff-height"}); break;
          case 258: sites.push_back({value, static_cast<std::uint8_t>(w), be, "bits-per-sample"}); break;
          case 273: sites.push_back({value, 4, be, "strip-offsets"}); break;
          case 277: sites.push_back({value, static_cast<std::uint8_t>(w), be, "samples-per-pixel"}); break;
          case 278: sites.push_back({value, static_cast<std::uint8_t>(w), be, "rows-per-strip"}); break;
          case 279: sites.push_back({value, 4, be, "strip-byte-counts"}); break;
          case 322: sites.push_back({value, static_cast<std::uint8_t>(w), be, "tile-width"}); break;
          case 323: sites.push_back({value, static_cast<std::uint8_t>(w), be, "tile-length"}); break;
          case 324: sites.push_back({value, 4, be, "tile-offsets"}); break;
          case 325: sites.push_back({value, 4, be, "tile-byte-counts"}); break;
          case 330:
            sites.push_back({value, 4, be, "sub-ifds"});
            if (rd(b, ent + 4, 4, be) == 1) ifds.push_back(rd(b, value, 4, be));
            break;
          default: break;
        }
        sites.push_back({ent + 4, 4, be, "entry-count"});
      }
      const std::size_t next_at = at + 2 + 12 * static_cast<std::size_t>(n);
      if (next_at + 4 <= b.size()) {
        sites.push_back({next_at, 4, be, "next-ifd"});
        const std::uint32_t next = rd(b, next_at, 4, be);
        if (next != 0) ifds.push_back(next);
      }
    }
  }

  // ISO BMFF (HEIC, AVIF, CR3): every box size in the top level and meta,
  // and every 'ispe' (image spatial extent).
  if (b.size() >= 12 && std::memcmp(b.data() + 4, "ftyp", 4) == 0) {
    for (std::size_t i = 4; i + 4 <= b.size(); ++i) {
      if (has(i, "ispe") && i + 16 <= b.size()) {
        sites.push_back({i + 8, 4, true, "ispe-width"});
        sites.push_back({i + 12, 4, true, "ispe-height"});
      }
      static constexpr const char* kBoxes[] = {"ftyp", "meta", "iinf", "iloc", "iprp",
                                               "ipco", "ipma", "mdat", "moov", "trak",
                                               "stsz", "stco", "hvcC", "av1C", "pixi",
                                               "iref", "pitm", "idat", "colr", "stsd"};
      for (const char* box : kBoxes) {
        if (i >= 4 && has(i, box)) sites.push_back({i - 4, 4, true, "box-size"});
      }
      if (sites.size() > 256) break;
    }
  }
  return sites;
}

using variant_fn = std::function<void(const std::string&, std::span<const std::uint8_t>)>;

void for_each_variant(const corpus_file& seed, bool derive_mutations, const variant_fn& fn) {
  const auto& src = seed.bytes;
  const std::size_t n = src.size();
  const bool full = full_sweep();
  std::vector<std::uint8_t> buf;
  const auto emit = [&](const std::string& what) { fn(seed.name + " :: " + what, buf); };

  buf = src;
  emit("as-is");

  // Truncation.
  {
    const std::size_t dense = full ? std::min<std::size_t>(n, 65536) : std::min<std::size_t>(n, 1024);
    for (std::size_t len = 0; len < dense; ++len) {
      buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(len));
      emit("truncate@" + std::to_string(len));
    }
    if (n > dense) {
      const std::size_t samples = full ? 1024 : 128;
      for (std::size_t s = 0; s < samples; ++s) {
        const std::size_t len = dense + (n - dense) * s / samples;
        buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(len));
        emit("truncate@" + std::to_string(len));
      }
      buf.assign(src.begin(), src.end() - 1);
      emit("truncate@" + std::to_string(n - 1));
    }
  }
  if (!derive_mutations) return;

  // Byte stomps over the header, sampled across the body.
  {
    const std::size_t header = std::min<std::size_t>(n, full ? 512 : 96);
    static constexpr std::uint8_t kStomp[] = {0x00, 0xFF, 0x80, 0x7F};
    for (std::size_t off = 0; off < header; ++off) {
      for (const std::uint8_t v : kStomp) {
        if (src[off] == v) continue;
        buf = src;
        buf[off] = v;
        emit("stomp@" + std::to_string(off) + "=" + std::to_string(v));
      }
    }
    const std::size_t samples = full ? 512 : 64;
    for (std::size_t s = 0; header < n && s < samples; ++s) {
      const std::size_t off = header + (n - header) * s / samples;
      buf = src;
      buf[off] ^= 0xFF;
      emit("invert@" + std::to_string(off));
    }
  }

  // Random bit flips, seeded by the file name so a failure reproduces.
  {
    splitmix64 rng{fnv1a(seed.name)};
    const int rounds = full ? 2048 : 192;
    for (int r = 0; n > 0 && r < rounds; ++r) {
      buf = src;
      const int flips = 1 + static_cast<int>(rng.next() % 4);
      std::string what = "bitflip#" + std::to_string(r);
      for (int k = 0; k < flips; ++k) {
        const std::size_t off = static_cast<std::size_t>(rng.next() % n);
        buf[off] ^= static_cast<std::uint8_t>(1u << (rng.next() % 8));
      }
      emit(what);
    }
  }

  // Generic 16/32-bit length stomps at every header offset: catches length
  // fields in formats the site table below does not model.
  {
    const std::size_t header = std::min<std::size_t>(n, full ? 512 : 64);
    static constexpr std::uint32_t k32[] = {0x00000000u, 0xFFFFFFFFu, 0x7FFFFFFFu};
    for (std::size_t off = 0; off + 4 <= header; ++off) {
      for (const std::uint32_t v : k32) {
        for (const bool be : {true, false}) {
          buf = src;
          wr(buf, off, 4, be, v);
          if (buf == src) continue;
          emit("u32" + std::string(be ? "be" : "le") + "@" + std::to_string(off) + "=" +
               std::to_string(v));
        }
      }
    }
  }

  // Structured: the real length / dimension / offset fields, set to values
  // that break arithmetic.
  {
    static constexpr std::uint32_t kValues[] = {0,     1,      2,          255,        256,
                                                16000, 65535,  65536,      0x7FFFFFFF, 0x80000000,
                                                0xFFFFFFFE, 0xFFFFFFFF};
    for (const field_site& site : dimension_sites(src)) {
      const std::uint64_t max = site.width == 4 ? 0xFFFFFFFFull : ((1ull << (8 * site.width)) - 1);
      for (const std::uint32_t raw : kValues) {
        std::uint64_t v = raw;
        if (site.bias && v > 0) v -= site.bias;
        if (v > max) v = max;
        buf = src;
        wr(buf, site.offset, site.width, site.big_endian, static_cast<std::uint32_t>(v));
        if (buf == src) continue;
        emit(std::string(site.what) + "@" + std::to_string(site.offset) + "=" + std::to_string(raw));
      }
    }
  }

  // Valid header, then garbage; and structural replays.
  {
    splitmix64 rng{fnv1a(seed.name) ^ 0xA5A5A5A5ull};
    for (const std::size_t keep : {4u, 8u, 16u, 32u, 64u, 128u, 256u, 1024u}) {
      if (keep >= n) break;
      buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(keep));
      for (int i = 0; i < 4096; ++i) buf.push_back(static_cast<std::uint8_t>(rng.next()));
      emit("header" + std::to_string(keep) + "+garbage");
      buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(keep));
      buf.resize(keep + 4096, 0);
      emit("header" + std::to_string(keep) + "+zeros");
    }
    buf = src;
    buf.insert(buf.end(), src.begin(), src.end());
    emit("doubled");
    buf = src;
    for (int i = 0; i < 4096; ++i) buf.push_back(static_cast<std::uint8_t>(rng.next()));
    emit("trailing-garbage");
    if (n >= 64) {
      // Replay the middle third twice: repeated chunks, frames, boxes.
      buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(2 * n / 3));
      buf.insert(buf.end(), src.begin() + static_cast<std::ptrdiff_t>(n / 3), src.end());
      emit("middle-replayed");
      // And cut it out.
      buf.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n / 3));
      buf.insert(buf.end(), src.begin() + static_cast<std::ptrdiff_t>(2 * n / 3), src.end());
      emit("middle-removed");
    }
  }
}

// ---------------------------------------------------------------------------
// The entry points
// ---------------------------------------------------------------------------
std::string check_raster(const char* who, const mv::result<mv::codec::raster>& r) {
  if (!r) return {};
  const auto& img = r.value();
  const std::uint64_t want = static_cast<std::uint64_t>(img.width) * img.height * 4;
  if (img.width == 0 || img.height == 0 || img.rgba.size() != want) {
    return std::string(who) + " returned ok with " + std::to_string(img.width) + "x" +
           std::to_string(img.height) + " and " + std::to_string(img.rgba.size()) +
           " RGBA bytes";
  }
  return {};
}

std::string check_display(const char* who, const mv::result<mv::image::display_image>& r) {
  if (!r) return {};
  const auto& img = r.value();
  const std::uint64_t want = static_cast<std::uint64_t>(img.width) * img.height * 4;
  if (img.width == 0 || img.height == 0 || img.rgba.size() != want) {
    return std::string(who) + " returned ok with " + std::to_string(img.width) + "x" +
           std::to_string(img.height) + " and " + std::to_string(img.rgba.size()) +
           " RGBA bytes";
  }
  return {};
}

mv::result<mv::codec::raster> family_decoder(format_family f, std::span<const std::uint8_t> b) {
  switch (f) {
    case format_family::jpeg: return mv::codec::decode_jpeg(b);
    case format_family::png:  return mv::codec::decode_png(b);
    case format_family::bmp:  return mv::codec::decode_bmp(b);
    case format_family::gif:  return mv::codec::decode_gif(b);
    case format_family::webp: return mv::codec::decode_webp(b);
    case format_family::tiff: return mv::codec::decode_tiff(b);
    case format_family::ico:  return mv::codec::decode_ico(b);
    case format_family::heic: return mv::codec::decode_heic(b);
    case format_family::avif: return mv::codec::decode_avif(b);
    case format_family::raw:  return mv::codec::decode_raw(b);
    case format_family::unknown: break;
  }
  return mv::err(mv::status::unsupported_format);
}

struct sweep_options {
  std::uint32_t max_frames = 64;  // animation frames stepped per input
};

struct sweep_stats {
  std::size_t inputs = 0;
  std::size_t calls = 0;
};

// Every entry point on one input. `family` is the seed's family (the direct
// decoder is called even when a mutation changed what probe() says).
void sweep_one(watchdog& dog, const std::string& name, std::span<const std::uint8_t> b,
               format_family family, const sweep_options& opt, std::vector<std::string>& failures,
               sweep_stats& stats) {
  ++stats.inputs;
  const auto call = [&](const char* what, const std::function<std::string()>& fn) {
    ++stats.calls;
    dog.run(name + " [" + what + "]", b, fn, failures);
  };

  call("probe+looks_like_raw", [&] {
    (void)mv::codec::probe(b);
    (void)mv::codec::looks_like_raw(b);
    return std::string{};
  });
  call("codec::decode", [&] { return check_raster("decode", mv::codec::decode(b)); });
  const format_family probed = mv::codec::probe(b);
  call("family decoder", [&] { return check_raster("family decoder", family_decoder(family, b)); });
  if (probed != family && probed != format_family::unknown) {
    call("probed decoder", [&] { return check_raster("probed decoder", family_decoder(probed, b)); });
  }
  if (family == format_family::jpeg) {
    call("jpeg_dimensions+scale4", [&] {
      (void)mv::codec::jpeg_dimensions(b);
      return check_raster("decode_jpeg/4", mv::codec::decode_jpeg(b, nullptr, 4));
    });
  }
  call("decode_raw_preview", [&] {
    return check_raster("decode_raw_preview", mv::codec::decode_raw_preview(b));
  });
  call("image::decode_preview", [&] {
    return check_display("decode_preview", mv::image::decode_preview(b));
  });
  call("image::decode_bytes", [&] {
    return check_display("decode_bytes", mv::image::decode_bytes(b));
  });

  // Animation: open, then step every frame (up to max_frames), each step on
  // its own deadline; then rewind and step once more.
  std::unique_ptr<mv::codec::animation_source> source;
  call("open_animation", [&] {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(b.begin(), b.end());
    auto opened = mv::codec::open_animation(std::move(shared));
    if (opened) source = std::move(opened).value();
    return std::string{};
  });
  if (!source) return;
  mv::codec::canvas_frame frame;
  bool more = true;
  std::uint32_t stepped = 0;
  const auto step = [&](const char* what) {
    call(what, [&] {
      auto next = source->next(frame, nullptr);
      more = next && next.value();
      if (more) {
        const auto& info = source->info();
        const std::uint64_t want = static_cast<std::uint64_t>(info.width) * info.height * 4;
        if (frame.rgba.size() != want) {
          return "animation frame " + std::to_string(frame.index) + " has " +
                 std::to_string(frame.rgba.size()) + " bytes for a " +
                 std::to_string(info.width) + "x" + std::to_string(info.height) + " canvas";
        }
      }
      return std::string{};
    });
  };
  while (more && stepped < opt.max_frames) {
    step("animation next");
    ++stepped;
  }
  call("animation rewind", [&] {
    (void)source->rewind();
    return std::string{};
  });
  more = true;
  step("animation next after rewind");
  call("animation destroy", [&] {
    source.reset();
    return std::string{};
  });
}

std::string summarise(const std::vector<std::string>& failures) {
  std::string out = std::to_string(failures.size()) + " failure(s):\n";
  for (std::size_t i = 0; i < failures.size() && i < 40; ++i) out += "  " + failures[i] + "\n";
  if (failures.size() > 40) out += "  ...\n";
  out += "Set MV_BROKEN_DUMP=<dir> to write each failing input for tests/data/broken/.";
  return out;
}

format_family family_from_dir(std::string_view dir) {
  if (dir == "jpeg") return format_family::jpeg;
  if (dir == "png") return format_family::png;
  if (dir == "bmp") return format_family::bmp;
  if (dir == "gif") return format_family::gif;
  if (dir == "webp") return format_family::webp;
  if (dir == "tiff") return format_family::tiff;
  if (dir == "ico") return format_family::ico;
  if (dir == "heic") return format_family::heic;
  if (dir == "avif") return format_family::avif;
  if (dir == "raw") return format_family::raw;
  return format_family::unknown;
}

void run_family(const char* dir) {
  const auto seeds = files_in(data_dir() / "seeds" / dir, std::string(dir) + "/");
  if (seeds.empty()) {
    FAIL("no seeds in " << (data_dir() / "seeds" / dir).string()
                        << " — run tools/testmedia/make-seeds.py. This test proved NOTHING.");
  }
  const format_family family = family_from_dir(dir);
  watchdog dog;
  std::vector<std::string> failures;
  sweep_stats stats;
  const auto started = std::chrono::steady_clock::now();
  for (const auto& seed : seeds) {
    for_each_variant(seed, true, [&](const std::string& name, std::span<const std::uint8_t> b) {
      // Derived variants step a few frames; the seed itself steps them all.
      const bool as_is = name.ends_with(":: as-is");
      sweep_one(dog, name, b, family, sweep_options{as_is ? 100000u : 16u}, failures, stats);
    });
  }
  const auto secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("[broken-corpus] %s: %zu seeds, %zu inputs, %zu calls, %.1f s%s\n", dir,
              seeds.size(), stats.inputs, stats.calls, secs, full_sweep() ? " (full sweep)" : "");
  INFO(summarise(failures));
  CHECK(failures.empty());
}

}  // namespace

// ---------------------------------------------------------------------------
// Seeds are real, decodable files. Where the decoder is still the PR 7 stub
// (unsupported_format for a file of its own family) that is a warning, not a
// failure — it becomes a hard requirement with MV_REQUIRE_ALL_DECODERS=1,
// which the coordinator sets once every family has landed.
// ---------------------------------------------------------------------------
TEST_CASE("broken corpus: every seed decodes with its own decoder", "[codec][broken][seeds]") {
  static constexpr const char* kFamilies[] = {"jpeg", "png",  "bmp",  "gif", "webp",
                                              "tiff", "ico",  "heic", "avif", "raw"};
  static constexpr const char* kLandedBeforePr7[] = {"jpeg", "png", "bmp", "gif", "webp"};
  const bool require_all = env_flag("MV_REQUIRE_ALL_DECODERS");
  for (const char* dir : kFamilies) {
    const auto seeds = files_in(data_dir() / "seeds" / dir, std::string(dir) + "/");
    INFO("seeds/" << dir);
    REQUIRE_FALSE(seeds.empty());
    const bool landed = require_all || std::any_of(std::begin(kLandedBeforePr7),
                                                   std::end(kLandedBeforePr7),
                                                   [&](const char* d) { return std::strcmp(d, dir) == 0; });
    for (const auto& seed : seeds) {
      const format_family family = family_from_dir(dir);
      INFO(seed.name);
      if (family != format_family::raw) {
        CHECK(mv::codec::probe(seed.bytes) == family);
      } else {
        CHECK((mv::codec::probe(seed.bytes) == format_family::raw ||
               mv::codec::probe(seed.bytes) == format_family::tiff));
      }
      const auto decoded = family == format_family::raw ? mv::codec::decode_raw(seed.bytes)
                                                        : family_decoder(family, seed.bytes);
      if (decoded) {
        CHECK(check_raster("seed", decoded).empty());
      } else if (landed) {
        FAIL_CHECK(seed.name << " failed to decode: " << mv::status_name(decoded.error()));
      } else {
        WARN(seed.name << ": " << mv::status_name(decoded.error())
                       << " — decoder not landed yet (stub?)");
      }
    }
  }
}

TEST_CASE("broken corpus: JPEG", "[codec][broken]") { run_family("jpeg"); }
TEST_CASE("broken corpus: PNG and APNG", "[codec][broken]") { run_family("png"); }
TEST_CASE("broken corpus: BMP", "[codec][broken]") { run_family("bmp"); }
TEST_CASE("broken corpus: GIF", "[codec][broken]") { run_family("gif"); }
TEST_CASE("broken corpus: WebP", "[codec][broken]") { run_family("webp"); }
TEST_CASE("broken corpus: TIFF", "[codec][broken]") { run_family("tiff"); }
TEST_CASE("broken corpus: ICO", "[codec][broken]") { run_family("ico"); }
TEST_CASE("broken corpus: HEIC", "[codec][broken]") { run_family("heic"); }
TEST_CASE("broken corpus: AVIF", "[codec][broken]") { run_family("avif"); }
TEST_CASE("broken corpus: RAW (DNG)", "[codec][broken]") { run_family("raw"); }

TEST_CASE("broken corpus: hand-crafted files in tests/data/broken", "[codec][broken]") {
  const auto nasties = files_in(data_dir() / "broken", "broken/");
  REQUIRE_FALSE(nasties.empty());
  watchdog dog;
  std::vector<std::string> failures;
  sweep_stats stats;
  for (const auto& file : nasties) {
    const format_family family = mv::codec::probe(file.bytes);
    // As-is steps every frame (the 20 000-frame GIF included); truncations a few.
    for_each_variant(file, false, [&](const std::string& name, std::span<const std::uint8_t> b) {
      const bool as_is = name.ends_with(":: as-is");
      sweep_one(dog, name, b, family, sweep_options{as_is ? 200000u : 8u}, failures, stats);
    });
  }
  std::printf("[broken-corpus] broken/: %zu files, %zu inputs, %zu calls\n", nasties.size(),
              stats.inputs, stats.calls);
  INFO(summarise(failures));
  CHECK(failures.empty());
}

TEST_CASE("broken corpus: empty, one-byte, magic-only and synthetic bombs", "[codec][broken]") {
  std::vector<corpus_file> inputs;
  const auto add = [&](std::string name, std::vector<std::uint8_t> bytes) {
    inputs.push_back({std::move(name), std::move(bytes)});
  };
  add("empty", {});
  static constexpr int kOneByte[] = {0x00, 0xFF, 0x89, 'B', 'G', 'R', 'I', 'M', 0x01};
  for (const int v : kOneByte) {
    add("one-byte-" + std::to_string(v), {static_cast<std::uint8_t>(v)});
  }
  const auto bytes_of = [](std::string_view s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
  };
  using namespace std::string_view_literals;
  const std::pair<const char*, std::vector<std::uint8_t>> magics[] = {
      {"jpeg", {0xFF, 0xD8, 0xFF}},
      {"jpeg-soi-eoi", {0xFF, 0xD8, 0xFF, 0xD9}},
      {"png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}},
      {"bmp", bytes_of("BM")},
      {"gif87a", bytes_of("GIF87a")},
      {"gif89a", bytes_of("GIF89a")},
      {"webp", bytes_of("RIFF\0\0\0\0WEBP"sv)},
      {"webp-vp8x", bytes_of("RIFF\x16\0\0\0WEBPVP8X\x0a\0\0\0"sv)},
      {"ico", {0, 0, 1, 0, 1, 0}},
      {"tiff-le", bytes_of("II*\0"sv)},
      {"tiff-be", bytes_of("MM\0*"sv)},
      {"bigtiff", bytes_of("II+\0\x08\0\0\0"sv)},
      {"heic", bytes_of("\0\0\0\x18" "ftypheic\0\0\0\0mif1heic"sv)},
      {"mif1", bytes_of("\0\0\0\x10" "ftypmif1\0\0\0\0"sv)},
      {"avif", bytes_of("\0\0\0\x14" "ftypavif\0\0\0\0avif"sv)},
      {"avis", bytes_of("\0\0\0\x14" "ftypavis\0\0\0\0avis"sv)},
      {"cr3", bytes_of("\0\0\0\x18" "ftypcrx \0\0\0\x01" "crx isom"sv)},
      {"raf", bytes_of("FUJIFILMCCD-RAW 0201"sv)},
      {"orf", bytes_of("IIRO\x08\0\0\0"sv)},
      {"rw2", bytes_of("IIU\0\x08\0\0\0"sv)},
  };
  for (const auto& [name, magic] : magics) {
    add(std::string("magic-") + name, magic);
    auto zeros = magic;
    zeros.resize(magic.size() + 4096, 0x00);
    add(std::string("magic-") + name + "+zeros", std::move(zeros));
    auto ones = magic;
    ones.resize(magic.size() + 4096, 0xFF);
    add(std::string("magic-") + name + "+ff", std::move(ones));
    auto big_box = magic;  // ISO BMFF box size claiming 4 GiB, and a 64-bit largesize
    if (magic.size() >= 8 && std::memcmp(magic.data() + 4, "ftyp", 4) == 0) {
      big_box[0] = big_box[1] = big_box[2] = big_box[3] = 0xFF;
      add(std::string("magic-") + name + "-box-4GiB", big_box);
      auto large = magic;
      large[0] = large[1] = large[2] = 0;
      large[3] = 1;  // size==1: a 64-bit largesize follows the type
      large.insert(large.begin() + 8, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
      add(std::string("magic-") + name + "-largesize-max", std::move(large));
    }
  }

  // A GIF with 100 000 one-pixel frames (1.5 MB): stepping every one must
  // finish, and the source must not keep them all.
  {
    std::vector<std::uint8_t> gif = bytes_of("GIF89a");
    const std::uint8_t screen[] = {1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255};
    gif.insert(gif.end(), std::begin(screen), std::end(screen));
    const std::uint8_t frame[] = {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0x02, 0x02, 0x44, 0x01, 0x00};
    for (int i = 0; i < 100000; ++i) gif.insert(gif.end(), std::begin(frame), std::end(frame));
    gif.push_back(0x3B);
    add("gif-100000-frames", std::move(gif));
  }
  // An APNG whose acTL promises 100 000 frames and delivers one.
  {
    const auto seed = read_file(data_dir() / "seeds" / "png" / "apng.png");
    if (!seed.empty()) {
      auto b = seed;
      for (std::size_t i = 8; i + 12 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, "acTL", 4) == 0) {
          wr(b, i + 4, 4, true, 100000);  // CRC now wrong too: both paths get exercised
          break;
        }
      }
      add("apng-actl-100000", std::move(b));
    }
  }
  // Decompression bomb that is well formed: a 16384x16384 PNG whose IDAT is a
  // real zlib stream of zero scanlines, truncated after 64 KiB.
  {
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                     0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                     0, 0, 0x40, 0, 0, 0, 0x40, 0, 8, 6, 0, 0, 0,
                                     0, 0, 0, 0};  // CRC left zero: libspng may reject it, fine
    png.insert(png.end(), {0, 1, 0, 0, 'I', 'D', 'A', 'T', 0x78, 0x01});
    png.resize(png.size() + 65534 - 2, 0);
    png.insert(png.end(), {0, 0, 0, 0});
    add("png-16384x16384-bad-crc", std::move(png));
  }

  watchdog dog;
  std::vector<std::string> failures;
  sweep_stats stats;
  for (const auto& in : inputs) {
    sweep_one(dog, in.name, in.bytes, mv::codec::probe(in.bytes), sweep_options{200000u},
              failures, stats);
  }
  std::printf("[broken-corpus] synthetic: %zu inputs, %zu calls\n", stats.inputs, stats.calls);
  INFO(summarise(failures));
  CHECK(failures.empty());
}

TEST_CASE("broken corpus: the watchdog reports exceptions and lying results",
          "[codec][broken][harness]") {
  watchdog dog;
  std::vector<std::string> failures;
  const std::vector<std::uint8_t> none;
  dog.run("throws", none, [] () -> std::string { throw std::runtime_error("boom"); }, failures);
  dog.run("lies", none, [] { return std::string("rgba size mismatch"); }, failures);
  dog.run("fine", none, [] { return std::string{}; }, failures);
  REQUIRE(failures.size() == 2);
  CHECK(failures[0].find("boom") != std::string::npos);
  CHECK(failures[1].find("lies") != std::string::npos);

  mv::codec::raster lying;
  lying.width = 4;
  lying.height = 4;
  lying.rgba.resize(8);
  CHECK_FALSE(check_raster("x", mv::result<mv::codec::raster>(std::move(lying))).empty());
}
