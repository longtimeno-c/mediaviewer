// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// copybench: the copy engine without the app (io/verified_copy.h; plan/12
// 2026-10-01, fast network copies). Copies every file under a folder into a
// fresh folder of its own under the destination, the way F8 and Import do,
// and prints one JSON object: files, bytes, seconds, MB/s, files/s.
//
//   copybench <src dir> <dest dir> [--mode auto|seq|deep] [--depth N] [--files N]
//             [--chunk-kib N] [--no-verify] [--rtt-us N] [--json <file>] [--keep]
//   copybench --make-fixture <dir> --count N --size-kib N [--seed N]
//
// --mode auto   what the app would pick (batch_copy_profile: deep only to or
//               from a network share, never deep reads of a card)
// --mode seq    the sequential path: one request and one file at a time
// --mode deep   --depth requests in flight per file, --files files at once
// --rtt-us N    every file request first waits N us (file_port.h detail): a
//               stand-in for a share's round trip on a local disk. Not a NAS:
//               the verify line's numbers come from a real share.
// --keep        leave the copied folder (by default it is removed after).
//
// Base vs new, per CLAUDE.md: same machine, same build configuration, same
// files, runs alternated. Nothing here logs a path beyond what was typed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "io/file_port.h"
#include "io/in_flight.h"
#include "io/verified_copy.h"

namespace {

using clock_type = std::chrono::steady_clock;

int usage() {
  std::fprintf(stderr,
               "usage: copybench <src dir> <dest dir> [--mode auto|seq|deep] [--depth N]\n"
               "                 [--files N] [--chunk-kib N] [--no-verify] [--rtt-us N]\n"
               "                 [--json <file>] [--keep]\n"
               "       copybench --make-fixture <dir> --count N --size-kib N [--seed N]\n");
  return 2;
}

unsigned long long number(const char* s) { return std::strtoull(s, nullptr, 10); }

// Incompressible bytes, so a share that compresses on the wire shows no
// flattering number.
void fill(std::vector<std::uint8_t>& buf, std::uint64_t& state) {
  for (std::size_t i = 0; i + 8 <= buf.size(); i += 8) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    std::memcpy(buf.data() + i, &state, 8);
  }
}

int make_fixture(const std::string& dir, unsigned long long count, unsigned long long size_kib,
                 unsigned long long seed) {
  if (!mv::io::make_directories(dir)) return 1;
  std::vector<std::uint8_t> buf(1u << 20);
  std::uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ull;
  for (unsigned long long i = 0; i < count; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "f%06llu.bin", i);
    mv::io::file_writer out;
    auto made = out.create_new(mv::io::join_path(dir, name));
    if (!made || *made != mv::io::rename_outcome::renamed) return 1;
    unsigned long long left = size_kib * 1024;
    while (left > 0) {
      fill(buf, state);
      const std::size_t n = static_cast<std::size_t>(std::min<unsigned long long>(left, buf.size()));
      if (!out.write(std::span<const std::uint8_t>(buf.data(), n))) return 1;
      left -= n;
    }
    if (!out.close()) return 1;
  }
  std::printf("{\"fixture\":true,\"files\":%llu,\"size_kib\":%llu}\n", count, size_kib);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--make-fixture") == 0) {
    if (argc < 3) return usage();
    unsigned long long count = 0, size_kib = 0, seed = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
      if (!std::strcmp(argv[i], "--count")) count = number(argv[i + 1]);
      else if (!std::strcmp(argv[i], "--size-kib")) size_kib = number(argv[i + 1]);
      else if (!std::strcmp(argv[i], "--seed")) seed = number(argv[i + 1]);
      else return usage();
    }
    if (count == 0) return usage();
    return make_fixture(argv[2], count, size_kib, seed);
  }
  if (argc < 3) return usage();
  const std::string src = argv[1];
  const std::string dest_root = argv[2];
  std::string mode = "auto";
  unsigned depth = mv::io::kNetworkIoDepth;
  unsigned files = mv::io::kNetworkFilesInFlight;
  std::size_t chunk = mv::io::kNetworkChunkBytes;
  bool verify = true;
  bool keep = false;
  unsigned rtt_us = 0;
  std::string json_path;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--mode" && has) mode = argv[++i];
    else if (a == "--depth" && has) depth = static_cast<unsigned>(number(argv[++i]));
    else if (a == "--files" && has) files = static_cast<unsigned>(number(argv[++i]));
    else if (a == "--chunk-kib" && has) chunk = static_cast<std::size_t>(number(argv[++i])) * 1024;
    else if (a == "--rtt-us" && has) rtt_us = static_cast<unsigned>(number(argv[++i]));
    else if (a == "--json" && has) json_path = argv[++i];
    else if (a == "--no-verify") verify = false;
    else if (a == "--keep") keep = true;
    else return usage();
  }

  mv::io::copy_profile profile;
  if (mode == "auto") {
    profile = mv::io::batch_copy_profile(src, dest_root);
  } else if (mode == "deep") {
    profile.read_depth = std::clamp(depth, 1u, mv::io::kMaxIoDepth);
    profile.write_depth = profile.read_depth;
    profile.files_in_flight = std::max(files, 1u);
    profile.buffer_bytes = chunk;
  } else if (mode != "seq") {
    return usage();
  }

  std::vector<mv::io::tree_entry> entries;
  if (!mv::io::walk_files(src, 32, [&](const mv::io::tree_entry& e) {
        entries.push_back(e);
        return true;
      })) {
    std::fprintf(stderr, "copybench: cannot list the source folder\n");
    return 1;
  }

  // A folder of this run's own, so nothing already there is ever touched and
  // the clean-up removes only what this run wrote.
  char run[64];
  std::snprintf(run, sizeof(run), "copybench-%lld",
                static_cast<long long>(clock_type::now().time_since_epoch().count()));
  const std::string dest = mv::io::join_path(dest_root, run);
  if (!mv::io::make_directories(dest)) {
    std::fprintf(stderr, "copybench: cannot create the destination folder\n");
    return 1;
  }
  for (const auto& e : entries) {
    const std::string_view parent = mv::io::parent_of(e.relative_utf8);
    if (!parent.empty()) {
      (void)mv::io::make_directories(mv::io::join_path(dest, mv::io::native_relative(parent)));
    }
  }

  mv::io::detail::set_simulated_round_trip_us(rtt_us);
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> failed{0};
  const auto t0 = clock_type::now();
  mv::io::for_each_in_flight(entries.size(), profile.files_in_flight, [&](std::size_t i) {
    mv::io::copy_options o;
    profile.apply(o);
    o.read_back = verify;
    const std::string targets[] = {
        mv::io::join_path(dest, mv::io::native_relative(entries[i].relative_utf8))};
    auto r = mv::io::verified_copy(entries[i].path_utf8, targets, o);
    if (r && mv::io::copy_succeeded(r->targets[0].outcome)) {
      bytes += r->bytes;
    } else {
      ++failed;
    }
  });
  const double seconds = std::chrono::duration<double>(clock_type::now() - t0).count();
  mv::io::detail::set_simulated_round_trip_us(0);
  if (!keep) (void)mv::io::remove_tree(dest);

  char out[768];
  std::snprintf(out, sizeof(out),
                "{\"mode\":\"%s\",\"read_depth\":%u,\"write_depth\":%u,\"files_in_flight\":%u,"
                "\"chunk_bytes\":%zu,\"verify\":%s,\"rtt_us\":%u,\"files\":%zu,\"bytes\":%llu,"
                "\"failed\":%llu,\"seconds\":%.3f,\"mb_per_s\":%.1f,\"files_per_s\":%.1f}\n",
                mode.c_str(), profile.read_depth, profile.write_depth, profile.files_in_flight,
                profile.buffer_bytes, verify ? "true" : "false", rtt_us, entries.size(),
                static_cast<unsigned long long>(bytes.load()),
                static_cast<unsigned long long>(failed.load()), seconds,
                seconds > 0 ? static_cast<double>(bytes.load()) / 1e6 / seconds : 0.0,
                seconds > 0 ? static_cast<double>(entries.size()) / seconds : 0.0);
  std::fputs(out, stdout);
  if (!json_path.empty()) {
    if (std::FILE* f = std::fopen(json_path.c_str(), "wb")) {
      std::fputs(out, f);
      std::fclose(f);
    }
  }
  return failed.load() == 0 ? 0 : 1;
}
