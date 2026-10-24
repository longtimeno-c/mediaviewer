// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-driftwire-bench: a Driftwire transfer between two real machines, with no
// app and no window (#540 step 1). One side receives, the other sends a fixed
// set over whatever path joins them (Wi-Fi, Ethernet, a Thunderbolt bridge, a
// VM across the internet), and each prints one JSON line: MB/s, files/s, CPU,
// time to the first byte, and where its time went (stages.h). The sender can
// also run iperf3 on the same path first, so the line says what share of the
// network the transfer got.
//
//   mv-driftwire-bench receive [--port N] [--dir D] [--runs N] [--keep]
//                              [--disk-check-mb N] [--json FILE] [--label L]
//   mv-driftwire-bench send <host> <port> <secret> [--set big|small|tiny|DIR]
//                              [--big-mb N] [--fixture D] [--runs N]
//                              [--iperf3] [--iperf3-streams N] [--line-mb-s X]
//                              [--json FILE] [--label L]
//
// The transfer is the real one: the internet handshake a direct code makes
// (CPace on a secret, then Noise_XXhfs+psk0), then transfer.h's send_files and
// receive_files: every file sealed, written to a temporary, hashed, flushed,
// read back uncached and renamed, exactly as in the app. The receiver makes a
// random secret and prints it; only a sender that has it gets through, and
// the received files are removed after each run (--keep leaves them).
//
// Sets (made once under --fixture and kept): big, 1 file of --big-mb (default
// 8192); small, 500 x 2 MiB; tiny, 5,000 x 200 KiB; or any folder of your own.
// The bytes are incompressible.
//
// --disk-check-mb (receiver, default 1024): before the first run, that much
// written to the destination, flushed and read back uncached, timed. The
// receiver writes every byte and reads it back, so a transfer near
// 1 / (1/write + 1/read_back) is bound by the disk, not the network; the
// line says "disk_bound" then.
//
// --iperf3 (sender): iperf3 -c <host> -J for 10 s with 1 stream and with
// --iperf3-streams (default 4), before the first run; iperf3 -s must be
// running on the receiver. --line-mb-s instead gives the reference by hand.
//
// Base vs new, per CLAUDE.md: same machines, same build configuration, same
// set, runs alternated. Results go to docs/perf/driftwire-lan.jsonl. Nothing
// printed holds a path, a name or an address (rule 6): sizes, counts, times.
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "addons/driftwire/channel.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/stages.h"
#include "addons/driftwire/transfer.h"
#include "io/file_port.h"

namespace sp = mv::driftwire;
namespace fs = std::filesystem;

namespace {

using clock_type = std::chrono::steady_clock;
constexpr int kSecretBytes = 32;
constexpr std::uint16_t kDefaultPort = 54540;

int usage() {
  std::fprintf(stderr,
               "usage: mv-driftwire-bench receive [--port N] [--dir D] [--runs N] [--keep]\n"
               "                                  [--disk-check-mb N] [--json FILE] [--label L]\n"
               "       mv-driftwire-bench send <host> <port> <secret> [--set big|small|tiny|DIR]\n"
               "                                  [--big-mb N] [--fixture D] [--runs N] [--iperf3]\n"
               "                                  [--iperf3-streams N] [--line-mb-s X] [--json FILE] [--label L]\n");
  return 2;
}

fs::path from_utf8(std::string_view s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

double seconds_since(clock_type::time_point t0) {
  return std::chrono::duration<double>(clock_type::now() - t0).count();
}

// This process's CPU time (user + system), in seconds.
double cpu_seconds() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  auto secs = [](const FILETIME& f) {
    return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
  };
  return secs(kernel) + secs(user);
#else
  rusage u{};
  if (getrusage(RUSAGE_SELF, &u) != 0) return 0;
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
         static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}

const char* os_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

// A label goes into the JSON: letters, digits and -_. only (never a path).
std::string clean_label(std::string_view in) {
  std::string out;
  for (char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
      out += c;
  }
  return out.substr(0, 40);
}

void emit(const std::string& line, const std::string& json_path) {
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
  if (json_path.empty()) return;
  std::ofstream out(from_utf8(json_path), std::ios::app);
  out << line << '\n';
}

// The stage with the most thread time that is work, not a wait.
const char* busiest(const sp::stage_times& t) {
  int best = -1;
  std::uint64_t most = 0;
  for (int i = 0; i < sp::stage_times::kCount; ++i) {
    const auto s = static_cast<sp::stage>(i);
    if (s == sp::stage::recv || s == sp::stage::wait_room || s == sp::stage::verify_wait) continue;
    const std::uint64_t ns = t.ns[i].load();
    if (ns > most) {
      most = ns;
      best = i;
    }
  }
  return best < 0 ? "" : sp::stage_name(static_cast<sp::stage>(best));
}

// ---- fixtures --------------------------------------------------------------------------

// Incompressible bytes, written once; kept when the size already matches.
bool fixture_file(const fs::path& p, std::uint64_t bytes, std::uint64_t seed) {
  std::error_code ec;
  if (fs::exists(p, ec) && fs::file_size(p, ec) == bytes) return true;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  std::vector<char> block(1 << 20);
  std::uint64_t x = 0x9E3779B97F4A7C15ull ^ seed;
  std::uint64_t left = bytes;
  while (left > 0) {
    for (std::size_t i = 0; i + 8 <= block.size(); i += 8) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      std::memcpy(block.data() + i, &x, 8);
    }
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, block.size()));
    out.write(block.data(), static_cast<std::streamsize>(n));
    left -= n;
  }
  return static_cast<bool>(out);
}

// The files of a set, made under `root` if they are not there yet.
std::optional<std::vector<sp::outgoing_file>> make_set(const std::string& set, const fs::path& root,
                                                       std::uint64_t big_mb) {
  std::vector<sp::outgoing_file> files;
  auto add = [&](const fs::path& p, std::string rel) {
    std::error_code ec;
    files.push_back({utf8(p), std::move(rel), fs::file_size(p, ec), 0});
  };
  auto many = [&](const char* name, std::size_t count, std::uint64_t each) {
    for (std::size_t i = 0; i < count; ++i) {
      char leaf[32];
      std::snprintf(leaf, sizeof leaf, "IMG_%05zu.JPG", i);
      const fs::path p = root / name / leaf;
      if (!fixture_file(p, each, 100 + i)) return false;
      add(p, leaf);
    }
    return true;
  };
  if (set == "big") {
    const fs::path p = root / "big" / "clip.mov";
    if (!fixture_file(p, big_mb << 20, 1)) return std::nullopt;
    add(p, "clip.mov");
  } else if (set == "small") {
    if (!many("small", 500, 2u << 20)) return std::nullopt;
  } else if (set == "tiny") {
    if (!many("tiny", 5000, 200u << 10)) return std::nullopt;
  } else {
    // A folder of the person's own: sent as one folder, as the app sends one.
    const fs::path dir = from_utf8(set);
    const std::string top = utf8(dir.filename().empty() ? dir.parent_path().filename() : dir.filename());
    auto walked = mv::io::walk_files(utf8(dir), 16, [&](const mv::io::tree_entry& e) {
      if (!e.cloud_only) files.push_back({e.path_utf8, top + "/" + e.relative_utf8, e.size, e.mtime_unix});
      return files.size() < sp::kMaxFiles;
    });
    if (!walked && walked.error() != mv::status::cancelled) return std::nullopt;
  }
  if (files.empty()) return std::nullopt;
  return files;
}

// ---- the disk's own rate (receiver) ----------------------------------------------------

struct disk_rates {
  double write_mb_s = 0, read_back_mb_s = 0;
};

// What the destination sustains for the receiver's own pattern: written in
// kChunkBytes, flushed, then read back uncached in 4 MiB.
std::optional<disk_rates> disk_check(const fs::path& dir, std::uint64_t mb) {
  const std::string path = utf8(dir / "disk-check.mvtmp");
  (void)mv::io::remove_file(path);
  disk_rates r;
  {
    mv::io::file_writer out;
    auto made = out.create_new(path);
    if (!made || *made != mv::io::rename_outcome::renamed) return std::nullopt;
    std::vector<std::uint8_t> chunk(sp::kChunkBytes);
    std::uint64_t x = 0x2545F4914F6CDD1Dull;
    for (std::size_t i = 0; i + 8 <= chunk.size(); i += 8) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      std::memcpy(chunk.data() + i, &x, 8);
    }
    const std::uint64_t bytes = mb << 20;
    const auto t0 = clock_type::now();
    for (std::uint64_t done = 0; done < bytes; done += chunk.size()) {
      if (!out.write(chunk)) return std::nullopt;
    }
    if (!out.flush_durable() || !out.close()) return std::nullopt;
    r.write_mb_s = static_cast<double>(bytes) / 1e6 / seconds_since(t0);
  }
  {
    mv::io::file_reader in;
    if (!in.open(path, mv::io::read_mode::uncached)) return std::nullopt;
    mv::io::aligned_buffer buf(4u << 20);
    if (buf.empty()) return std::nullopt;
    std::uint64_t got = 0;
    const auto t0 = clock_type::now();
    for (;;) {
      auto n = in.read({buf.data(), buf.size()});
      if (!n) return std::nullopt;
      if (*n == 0) break;
      got += *n;
    }
    r.read_back_mb_s = static_cast<double>(got) / 1e6 / seconds_since(t0);
  }
  (void)mv::io::remove_file(path);
  return r;
}

// ---- iperf3 (sender) ---------------------------------------------------------------------

// iperf3's received rate in MB/s, or nothing when it is not installed or failed.
std::optional<double> iperf3(const std::string& host, int streams) {
  std::string cmd = "iperf3 -c \"" + host + "\" -J -t 10 -P " + std::to_string(streams);
#if defined(_WIN32)
  FILE* p = _popen(cmd.c_str(), "r");
#else
  FILE* p = popen(cmd.c_str(), "r");
#endif
  if (!p) return std::nullopt;
  std::string out;
  char buf[4096];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p)) out.append(buf, n);
#if defined(_WIN32)
  _pclose(p);
#else
  pclose(p);
#endif
  // The end's "sum_received": what arrived, all streams together.
  const auto at = out.rfind("\"sum_received\"");
  if (at == std::string::npos) return std::nullopt;
  const auto bps = out.find("\"bits_per_second\"", at);
  if (bps == std::string::npos) return std::nullopt;
  const auto colon = out.find(':', bps);
  if (colon == std::string::npos) return std::nullopt;
  const double v = std::strtod(out.c_str() + colon + 1, nullptr);
  if (v <= 0) return std::nullopt;
  return v / 8 / 1e6;
}

bool host_name_ok(std::string_view h) {
  // Passed to a shell for iperf3: an address or a host name, nothing else.
  if (h.empty() || h.size() > 253) return false;
  return std::all_of(h.begin(), h.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == ':' || c == '%';
  });
}

// ---- the two sides -----------------------------------------------------------------------

std::string json_number(const char* key, std::optional<double> v, const char* fmt = "%.2f") {
  if (!v) return "";
  char num[64];
  std::snprintf(num, sizeof num, fmt, *v);
  return std::string(",\"") + key + "\":" + num;
}

int receive_main(int argc, char** argv) {
  unsigned long long port = kDefaultPort, runs = 1, disk_mb = 1024;
  fs::path dir = fs::temp_directory_path() / "mv-driftwire-bench";
  bool keep = false;
  std::string json_path, label;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--port" && has) port = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--dir" && has) dir = from_utf8(argv[++i]);
    else if (a == "--runs" && has) runs = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--keep") keep = true;
    else if (a == "--disk-check-mb" && has) disk_mb = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--json" && has) json_path = argv[++i];
    else if (a == "--label" && has) label = clean_label(argv[++i]);
    else return usage();
  }
  if (port > 65535 || runs == 0) return usage();
  std::error_code ec;
  fs::create_directories(dir, ec);

  std::optional<disk_rates> disk;
  if (disk_mb > 0) {
    disk = disk_check(dir, disk_mb);
    if (!disk) std::fprintf(stderr, "disk check failed; going on without it\n");
  }

  std::vector<std::uint8_t> secret(kSecretBytes);
  randombytes_buf(secret.data(), secret.size());
  char hex[kSecretBytes * 2 + 1];
  sodium_bin2hex(hex, sizeof hex, secret.data(), secret.size());

  auto l = sp::net::listen_internet(static_cast<std::uint16_t>(port), false);
  if (!l) {
    std::fprintf(stderr, "cannot listen on port %llu\n", port);
    return 1;
  }
  std::fprintf(stderr, "listening on port %u; on the sender:\n  mv-driftwire-bench send <this machine> %u %s\n",
               static_cast<unsigned>((*l)->port()), static_cast<unsigned>((*l)->port()), hex);

  int failures = 0;
  for (unsigned long long run = 1; run <= runs; ++run) {
    auto a = (*l)->accept(24 * 60 * 60 * 1000);
    if (!a) return 1;
    std::shared_ptr<sp::link> link(std::move(*a));
    const auto t_connect = clock_type::now();
    auto hi = sp::channel::read_hello(*link, clock_type::now() + std::chrono::seconds(20));
    if (!hi || !hi->internet) {
      std::fprintf(stderr, "a connection that was not the bench's; closed\n");
      link->shutdown();
      --run;
      continue;
    }
    sp::channel::options o;
    o.internet_secret = secret;
    auto ch = sp::channel::open_server(link, *hi, "bbbbbbbbbbbbbbbb", "bench-receiver", o);
    if (!ch) {
      std::fprintf(stderr, "handshake failed (a wrong secret?)\n");
      --run;
      continue;
    }
    const double handshake_ms = seconds_since(t_connect) * 1000;
    auto offer = sp::read_offer(**ch, 30000);
    if (!offer) {
      std::fprintf(stderr, "no offer\n");
      ++failures;
      continue;
    }
    const fs::path dest = dir / ("run-" + std::to_string(run));
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);

    sp::stage_times stages;
    sp::hooks h;
    h.stages = &stages;
    std::atomic<std::int64_t> first_byte_ns{-1};
    clock_type::time_point t0;
    h.progress = [&](std::uint64_t bytes, std::uint32_t) {
      if (bytes > 0 && first_byte_ns.load() < 0) {
        first_byte_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - t0).count());
      }
    };
    const double cpu0 = cpu_seconds();
    t0 = clock_type::now();
    if (!sp::send_accept(**ch)) {
      ++failures;
      continue;
    }
    const auto res = sp::receive_files(**ch, *offer, utf8(dest), h);
    const double wall = seconds_since(t0);
    const double cpu = cpu_seconds() - cpu0;
    if (!keep) fs::remove_all(dest, ec);
    if (res.how != sp::outcome::delivered) ++failures;

    const double mb_s = static_cast<double>(offer->bytes) / 1e6 / wall;
    std::optional<double> ceiling;
    if (disk && disk->write_mb_s > 0 && disk->read_back_mb_s > 0) {
      ceiling = 1.0 / (1.0 / disk->write_mb_s + 1.0 / disk->read_back_mb_s);
    }
    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"tool\":\"driftwire-bench\",\"side\":\"receive\",\"label\":\"%s\",\"os\":\"%s\",\"run\":%llu,"
                  "\"outcome\":\"%s\",\"files\":%zu,\"bytes\":%llu,\"seconds\":%.3f,\"mb_s\":%.1f,\"files_s\":%.1f,"
                  "\"cpu_pct\":%.0f,\"handshake_ms\":%.0f,\"first_byte_ms\":%.1f,\"busiest\":\"%s\"",
                  label.c_str(), os_name(), run, sp::outcome_name(res.how), offer->files.size(),
                  static_cast<unsigned long long>(offer->bytes), wall, mb_s,
                  static_cast<double>(offer->files.size()) / wall, cpu / wall * 100, handshake_ms,
                  static_cast<double>(first_byte_ns.load()) / 1e6, busiest(stages));
    std::string line = head;
    if (disk) {
      line += json_number("disk_write_mb_s", disk->write_mb_s, "%.1f");
      line += json_number("disk_read_back_mb_s", disk->read_back_mb_s, "%.1f");
      line += json_number("disk_ceiling_mb_s", ceiling, "%.1f");
      line += std::string(",\"disk_bound\":") + (ceiling && mb_s >= 0.9 * *ceiling ? "true" : "false");
    }
    line += ",\"stages\":" + stages.json(wall) + "}";
    emit(line, json_path);
  }
  return failures == 0 ? 0 : 1;
}

int send_main(int argc, char** argv) {
  if (argc < 5) return usage();
  const std::string host = argv[2];
  const unsigned long long port = std::strtoull(argv[3], nullptr, 10);
  std::vector<std::uint8_t> secret(kSecretBytes);
  std::size_t secret_len = 0;
  if (port == 0 || port > 65535 ||
      sodium_hex2bin(secret.data(), secret.size(), argv[4], std::strlen(argv[4]), nullptr, &secret_len, nullptr) != 0 ||
      secret_len != secret.size()) {
    return usage();
  }
  std::string set = "big", json_path, label;
  unsigned long long big_mb = 8192, runs = 1, streams = 4;
  fs::path fixture = fs::temp_directory_path() / "mv-driftwire-bench-fixture";
  bool run_iperf = false;
  std::optional<double> line_mb_s;
  for (int i = 5; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--set" && has) set = argv[++i];
    else if (a == "--big-mb" && has) big_mb = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--fixture" && has) fixture = from_utf8(argv[++i]);
    else if (a == "--runs" && has) runs = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--iperf3") run_iperf = true;
    else if (a == "--iperf3-streams" && has) streams = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--line-mb-s" && has) line_mb_s = std::strtod(argv[++i], nullptr);
    else if (a == "--json" && has) json_path = argv[++i];
    else if (a == "--label" && has) label = clean_label(argv[++i]);
    else return usage();
  }
  if (runs == 0 || big_mb == 0 || streams == 0 || streams > 128) return usage();
  const auto addr = sp::net::parse_address(host);
  if (!addr) {
    std::fprintf(stderr, "the host must be an IP address\n");
    return usage();
  }

  std::fprintf(stderr, "preparing the set...\n");
  const auto files = make_set(set, fixture, big_mb);
  if (!files) {
    std::fprintf(stderr, "could not make or read the set\n");
    return 1;
  }
  std::uint64_t bytes = 0;
  for (const auto& f : *files) bytes += f.size;
  const char* set_name = set == "big" || set == "small" || set == "tiny" ? set.c_str() : "folder";

  std::optional<double> iperf_1, iperf_n;
  if (run_iperf && host_name_ok(host)) {
    std::fprintf(stderr, "iperf3, 1 stream and %llu...\n", streams);
    iperf_1 = iperf3(host, 1);
    iperf_n = iperf3(host, static_cast<int>(streams));
    if (!iperf_1) std::fprintf(stderr, "iperf3 gave nothing (installed? iperf3 -s on the receiver?)\n");
  }
  std::optional<double> reference = line_mb_s;
  if (!reference && (iperf_1 || iperf_n)) reference = std::max(iperf_1.value_or(0), iperf_n.value_or(0));

  int failures = 0;
  for (unsigned long long run = 1; run <= runs; ++run) {
    const auto t_connect = clock_type::now();
    auto c = sp::net::connect({host, static_cast<std::uint16_t>(port)}, 10000);
    if (!c) {
      std::fprintf(stderr, "cannot connect\n");
      return 1;
    }
    auto ch = sp::channel::open_internet(std::shared_ptr<sp::link>(std::move(*c)), secret, "aaaaaaaaaaaaaaaa",
                                         "bench-sender");
    if (!ch) {
      std::fprintf(stderr, "handshake failed (a wrong secret?)\n");
      return 1;
    }
    const double handshake_ms = seconds_since(t_connect) * 1000;

    sp::stage_times stages;
    sp::hooks h;
    h.stages = &stages;
    std::atomic<std::int64_t> first_byte_ns{-1};
    clock_type::time_point t0 = clock_type::now();
    double cpu0 = cpu_seconds();
    h.progress = [&](std::uint64_t moved, std::uint32_t) {
      if (moved > 0 && first_byte_ns.load() < 0) {
        first_byte_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - t0).count());
      }
    };
    // Timed from the accept, as the receiver is: the offer and its answer
    // are not the transfer.
    const auto res = sp::send_files(**ch, *files, h, [&] {
      stages.reset();
      t0 = clock_type::now();
      cpu0 = cpu_seconds();
    });
    const double wall = seconds_since(t0);
    const double cpu = cpu_seconds() - cpu0;
    if (res.how != sp::outcome::delivered) ++failures;
    const double mb_s = static_cast<double>(bytes) / 1e6 / wall;

    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"tool\":\"driftwire-bench\",\"side\":\"send\",\"label\":\"%s\",\"os\":\"%s\",\"set\":\"%s\","
                  "\"run\":%llu,\"outcome\":\"%s\",\"files\":%zu,\"bytes\":%llu,\"seconds\":%.3f,\"mb_s\":%.1f,"
                  "\"files_s\":%.1f,\"cpu_pct\":%.0f,\"handshake_ms\":%.0f,\"first_byte_ms\":%.1f,\"busiest\":\"%s\"",
                  label.c_str(), os_name(), set_name, run, sp::outcome_name(res.how), files->size(),
                  static_cast<unsigned long long>(bytes), wall, mb_s, static_cast<double>(files->size()) / wall,
                  cpu / wall * 100, handshake_ms, static_cast<double>(first_byte_ns.load()) / 1e6, busiest(stages));
    std::string line = head;
    line += json_number("iperf3_p1_mb_s", iperf_1, "%.1f");
    if (iperf_n) {
      line += ",\"iperf3_streams\":" + std::to_string(streams);
      line += json_number("iperf3_pn_mb_s", iperf_n, "%.1f");
    }
    line += json_number("reference_mb_s", reference, "%.1f");
    if (reference && *reference > 0) line += json_number("line_share", mb_s / *reference, "%.3f");
    line += ",\"stages\":" + stages.json(wall) + "}";
    emit(line, json_path);
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (sodium_init() < 0) return 1;
  if (argc >= 2 && std::strcmp(argv[1], "receive") == 0) return receive_main(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "send") == 0) return send_main(argc, argv);
  return usage();
}
