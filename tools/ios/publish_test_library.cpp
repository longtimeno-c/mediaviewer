// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a real published catalogue from a folder, the same way the desktop's publisher does (real
// catalog::build and catalog::deliver, real signature), without the indexer's models: for proving
// the phone end to end against `driftwire serve`. Thumbnails come from `sips` and `ffmpeg`.
//
//   publish_test_library <folder> <nas destination> <work dir> <generation> [wrong-key] [library=<id>]
//
// Not shipped; docs/plans/mobile.md M3/M4 verify. macOS (sips).
#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "addons/ai/catalog.h"
#include "addons/ai/index_db.h"

namespace fs = std::filesystem;
namespace cat = mv::ai::catalog;
using namespace mv;
using namespace mv::ai;

static std::vector<std::uint8_t> read_bytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool is_video(const fs::path& p) {
  const auto e = p.extension().string();
  return e == ".mp4" || e == ".mov" || e == ".m4v";
}

static std::string quoted(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

/// A clip's length from ffprobe, in milliseconds; 0 when it cannot say.
static std::int64_t clip_ms(const std::string& path) {
  const std::string cmd = "/opt/homebrew/bin/ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 " + quoted(path);
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return 0;
  char line[64] = {};
  const bool read = std::fgets(line, sizeof line, pipe) != nullptr;
  pclose(pipe);
  if (!read) return 0;
  const double seconds = std::strtod(line, nullptr);
  return seconds > 0 ? static_cast<std::int64_t>(seconds * 1000.0) : 0;
}

static std::optional<std::vector<std::uint8_t>> shrink(const std::string& source, bool video, int edge, const fs::path& work) {
  const fs::path out = work / ("thumb-" + std::to_string(std::rand()) + ".jpg");
  const std::string cmd = video
      ? "/opt/homebrew/bin/ffmpeg -y -loglevel error -i " + quoted(source) + " -frames:v 1 -vf scale=" + std::to_string(edge) + ":-2 " + quoted(out.string())
      : "/usr/bin/sips -Z " + std::to_string(edge) + " -s format jpeg --out " + quoted(out.string()) + " " + quoted(source) + " > /dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0) return std::nullopt;
  auto bytes = read_bytes(out);
  std::error_code ec;
  fs::remove(out, ec);
  if (bytes.empty()) return std::nullopt;
  return bytes;
}

int main(int argc, char** argv) {
  if (argc < 5 || sodium_init() < 0) {
    std::cerr << "usage: publish_test_library <folder> <nas destination> <work dir> <generation> [wrong-key] [library=<id>]\n";
    return 2;
  }
  const fs::path folder = fs::absolute(argv[1]);
  const fs::path nas = fs::absolute(argv[2]);
  const fs::path work = fs::absolute(argv[3]);
  const std::uint64_t generation = std::stoull(argv[4]);
  bool wrong_key = false;
  std::string library_id = "e2e-library-1";  // library=<id>: another library, for pairing over one
  for (int i = 5; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "wrong-key") wrong_key = true;
    else if (a.rfind("library=", 0) == 0 && a.size() > 8) library_id = a.substr(8);
  }
  std::error_code ec;
  fs::create_directories(work, ec);
  fs::create_directories(nas, ec);

  // The signing key lives in work/, so generation 2 is signed by the same key.
  const fs::path key_file = work / "publisher.key";
  std::vector<std::uint8_t> pub, secret;
  if (fs::exists(key_file)) {
    secret = read_bytes(key_file);
    pub = cat::public_of(secret);
  } else {
    std::tie(pub, secret) = cat::make_keypair();
    std::ofstream(key_file, std::ios::binary).write(reinterpret_cast<const char*>(secret.data()), static_cast<std::streamsize>(secret.size()));
  }
  // wrong-key: the folder says the same public key, but the signature is someone else's.
  std::vector<std::uint8_t> signing_secret = secret;
  if (wrong_key) signing_secret = cat::make_keypair().second;

  // A fresh index each time: one root, every file below it.
  const fs::path index_path = work / ("index-" + std::to_string(generation) + ".db");
  fs::remove(index_path, ec);
  auto db = index_db::open(index_path.string());
  if (!db) { std::cerr << "cannot make index\n"; return 1; }
  auto root = (*db)->add_root(folder.string(), true);
  if (!root) { std::cerr << "cannot add root\n"; return 1; }

  std::vector<index_db::seen_file> seen;
  std::uint64_t now_tick = 1;
  (void)now_tick;
  for (const auto& entry : fs::recursive_directory_iterator(folder)) {
    if (!entry.is_regular_file() || entry.path().filename().string().front() == '.') continue;
    index_db::seen_file f;
    f.path = entry.path().string();
    f.mtime = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(entry.path()).time_since_epoch()).count());
    f.size = entry.file_size();
    f.kind = is_video(entry.path()) ? asset_kind::video : asset_kind::photo;
    seen.push_back(std::move(f));
  }
  if (auto r = (*db)->see_assets(*root, seen, 1); !r) { std::cerr << "cannot add assets\n"; return 1; }
  db->reset();  // the indexer's scan normally sets the duration
  {
    sqlite3* raw = nullptr;
    if (sqlite3_open(index_path.c_str(), &raw) != SQLITE_OK) { std::cerr << "cannot open index\n"; return 1; }
    // Each clip's real length, and one spoken moment inside it (at 12 s, or halfway in a short one).
    std::vector<std::pair<std::int64_t, std::string>> clips;
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT id, path FROM assets WHERE kind = 2", -1, &q, nullptr) == SQLITE_OK) {
      while (sqlite3_step(q) == SQLITE_ROW) {
        clips.emplace_back(sqlite3_column_int64(q, 0), reinterpret_cast<const char*>(sqlite3_column_text(q, 1)));
      }
    }
    sqlite3_finalize(q);
    for (const auto& [id, path] : clips) {
      const std::int64_t ms = clip_ms(path);
      if (ms <= 0) { std::cerr << "no length for " << path << "\n"; continue; }
      const std::int64_t start = std::min<std::int64_t>(12000, ms / 2);
      const std::string sql = "UPDATE assets SET duration_ms = " + std::to_string(ms) + " WHERE id = " + std::to_string(id) + ";"
          "INSERT INTO speech(asset_id, spec, start_ms, end_ms, text) VALUES (" + std::to_string(id) + ", 'ai-test', " +
          std::to_string(start) + ", " + std::to_string(std::min(ms, start + 1000)) + ", 'The moment to jump to');";
      sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
    }
    sqlite3_close(raw);
  }

  auto thumbs = cat::thumb_store::open((work / "thumbs").string());
  if (!thumbs) { std::cerr << "no thumb store\n"; return 1; }
  std::vector<std::int64_t> roots_id{*root};
  auto wants = (*thumbs)->missing(index_path.string(), roots_id);
  if (wants && !wants->empty()) {
    cat::thumb_fill_io io;
    io.jpeg512 = [&](const cat::thumb_want& w) -> result<std::vector<std::uint8_t>> {
      auto b = shrink(w.path, w.kind == 2, 512, work);
      if (!b) return err(status::io);
      return *b;
    };
    io.tiny = [&](std::span<const std::uint8_t> jpeg) -> result<std::vector<std::uint8_t>> {
      const fs::path in = work / "shrink-in.jpg";
      std::ofstream(in, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      auto b = shrink(in.string(), false, 64, work);
      if (!b) return err(status::io);
      return *b;
    };
    cat::control c;
    (void)(*thumbs)->fill(*wants, io, c, 0);
  }
  if (auto s = (*thumbs)->seal(); !s) { std::cerr << "seal failed\n"; return 1; }

  cat::root_map rm;
  rm.id = *root;
  rm.path = folder.string();
  rm.uuid = "e2e-root-uuid-0001";
  rm.share = "Photos";
  rm.sub = "";

  const fs::path gen_dir = work / ("gen-" + std::to_string(generation));
  fs::remove_all(gen_dir, ec);
  cat::build_options b;
  b.index_db = index_path.string();
  b.out_dir = gen_dir.string();
  b.library_id = library_id;
  b.epoch = 1;
  b.generation = generation;
  b.created_unix = std::time(nullptr);
  b.active_spec = "ai-test";
  b.settings_json = "{}";
  b.roots.push_back(rm);
  b.thumbs = thumbs->get();
  // A large model file makes a copy long enough to interrupt on loopback.
  if (const char* model = std::getenv("MV_E2E_MODEL")) {
    b.model_spec = "ai-test";
    b.model_files.push_back(model);
  }
  cat::control c;
  auto built = cat::build(b, c);
  if (!built) { std::cerr << "build failed " << status_name(built.error()) << "\n"; return 1; }

  cat::deliver_options d;
  d.gen_dir = gen_dir.string();
  d.destination = nas.string();
  d.library_id = b.library_id;
  d.generation = generation;
  d.now_unix = b.created_unix;
  d.epoch = 1;
  d.public_key = pub;
  d.secret_key = signing_secret;
  d.shared = built->shared;
  d.copy = [](const std::string& from, const std::string& to) -> expected {
    std::error_code e;
    fs::copy_file(from, to, e);
    if (e) return err(status::io);
    return {};
  };
  d.make_directories = [](const std::string& dir) -> expected {
    std::error_code e;
    fs::create_directories(dir, e);
    if (e) return err(status::io);
    return {};
  };
  if (auto r = cat::deliver(d, c); !r) { std::cerr << "deliver failed " << status_name(r.error()) << "\n"; return 1; }
  std::cout << "publisher_key=" << cat::to_hex(pub) << "\n";
  std::cout << "library_id=" << b.library_id << "\n";
  std::cout << "assets=" << built->assets << " thumbs=" << built->thumbs << "\n";
  return 0;
}
