// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop half of docs/plans/mobile.md §9's parity test, macOS only.
//
//   publish  catalog::build and deliver from a copy of an index, the desktop's own publisher code,
//            with real tiles (ImageIO, AVFoundation) and real place labels (src/geo).
//   sample-publish  the sample library: index a folder with the real engine and the app's decoders, then phone_publish.
//   search   the desktop's engine through pack_deps over the real AI pack, reading a generation,
//            and writes one JSON line per query (compare_parity.py).
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <sodium.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <atomic>
#include <span>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_ai.h>

#include "abi/addon_media.h"
#include "addon/host.h"
#include "addon/slots.h"
#include "addons/places/places.h"
#include "addons/ai/catalog.h"
#include "wire/secrets.h"
#include "addons/ai/engine.h"
#include "addons/ai/host.h"
#include "addons/ai/pack.h"
#include "core/json.h"
#include "geo/geo.h"
#include "geo/open.h"
#include "infer/ort.h"
#include "meta/meta.h"
#include "mobile/sample.h"

namespace fs = std::filesystem;
namespace cat = mv::ai::catalog;

namespace {

std::vector<std::uint8_t> encode(CGImageRef image, CGFloat max_edge, CGFloat quality) {
  const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
  const CGFloat scale = std::min<CGFloat>(1, max_edge / std::max(w, h));
  const size_t tw = std::max<size_t>(1, static_cast<size_t>(w * scale)), th = std::max<size_t>(1, static_cast<size_t>(h * scale));
  CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(nullptr, tw, th, 8, 0, rgb, kCGImageAlphaNoneSkipLast);
  CGColorSpaceRelease(rgb);
  std::vector<std::uint8_t> out;
  if (!ctx) return out;
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, tw, th), image);
  CGImageRef scaled = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  NSMutableData* data = [NSMutableData data];
  CGImageDestinationRef d = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
  NSDictionary* props = @{(__bridge id)kCGImageDestinationLossyCompressionQuality : @(quality)};
  CGImageDestinationAddImage(d, scaled, (__bridge CFDictionaryRef)props);
  CGImageDestinationFinalize(d);
  CFRelease(d);
  CGImageRelease(scaled);
  out.assign(static_cast<const std::uint8_t*>(data.bytes), static_cast<const std::uint8_t*>(data.bytes) + data.length);
  return out;
}

mv::result<std::vector<std::uint8_t>> jpeg512_of(const cat::thumb_want& w) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:w.path.c_str()]];
    CGImageRef image = nullptr;
    if (w.kind == 2) {
      AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
      AVAssetImageGenerator* g = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
      g.appliesPreferredTrackTransform = YES;
      g.maximumSize = CGSizeMake(512, 512);
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      __block CGImageRef got = nullptr;
      [g generateCGImageAsynchronouslyForTime:CMTimeMakeWithSeconds(std::min(1.0, w.duration_ms / 2000.0), 600)
                            completionHandler:^(CGImageRef cg, CMTime, NSError*) {
                              if (cg) got = CGImageRetain(cg);
                              dispatch_semaphore_signal(done);
                            }];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      image = got;
    } else {
      CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
      if (src) {
        NSDictionary* o = @{(__bridge id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
                            (__bridge id)kCGImageSourceCreateThumbnailWithTransform : @YES,
                            (__bridge id)kCGImageSourceThumbnailMaxPixelSize : @512};
        image = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)o);
        CFRelease(src);
      }
    }
    if (!image) return mv::err(mv::status::unsupported_format);
    auto out = encode(image, 512, 0.75);
    CGImageRelease(image);
    if (out.empty()) return mv::err(mv::status::io);
    return out;
  }
}

mv::result<std::vector<std::uint8_t>> shrink(std::span<const std::uint8_t> jpeg) {
  @autoreleasepool {
    NSData* data = [NSData dataWithBytesNoCopy:const_cast<std::uint8_t*>(jpeg.data()) length:jpeg.size() freeWhenDone:NO];
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    if (!src) return mv::err(mv::status::corrupt);
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!image) return mv::err(mv::status::corrupt);
    auto out = encode(image, cat::kTinyEdge, cat::kTinyQuality / 100.0);
    CGImageRelease(image);
    return out;
  }
}



// The newest installed version of an add-on piece (`<add-ons>/AI Audio/0.1.41`), as the app's
// store picks it. A piece that is missing stops the run: answering without it would quietly
// compare a different engine.
std::string piece_folder(const std::string& addons, const std::string& name) {
  std::string best;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(addons) / name, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string v = it->path().filename().string();
    if (v.empty() || v[0] < '0' || v[0] > '9' || !fs::is_directory(it->path())) continue;
    const auto parts = [](const std::string& t) {
      std::vector<int> out;
      std::stringstream in(t);
      for (std::string p; std::getline(in, p, '.');) out.push_back(std::atoi(p.c_str()));
      return out;
    };
    if (best.empty() || parts(v) > parts(fs::path(best).filename().string())) best = it->path().string();
  }
  if (best.empty()) {
    std::fprintf(stderr, "the %s piece is not installed under %s: refusing to answer without it\n", name.c_str(), addons.c_str());
    std::exit(3);
  }
  return best;
}

std::map<std::string, std::string> required_pieces(const std::string& addons) {
  return {{"ai-audio", piece_folder(addons, "AI Audio")},
          {"ai-docs", piece_folder(addons, "AI Documents")},
          {"ai-faces", piece_folder(addons, "AI Faces")}};
}

struct noop_host {
  std::string data_dir;
  std::map<std::string, std::string> pieces;
};

mv_status MV_CALL h_data_dir(void* h, char* out, uint32_t cap) {
  const std::string& d = static_cast<noop_host*>(h)->data_dir;
  if (d.size() + 1 > cap) return MV_ERR_INVALID_ARG;
  std::memcpy(out, d.c_str(), d.size() + 1);
  return MV_OK;
}
mv_status MV_CALL h_piece_dir(void* h, const char* piece, char* out, uint32_t cap) {
  auto& pieces = static_cast<noop_host*>(h)->pieces;
  auto it = pieces.find(piece);
  if (it == pieces.end()) return MV_ERR_IO;
  if (it->second.size() + 1 > cap) return MV_ERR_INVALID_ARG;
  std::memcpy(out, it->second.c_str(), it->second.size() + 1);
  return MV_OK;
}
mv_status MV_CALL h_stat(void*, const char* path, uint64_t* size, int64_t* mtime, uint32_t* is_dir) {
  std::error_code ec;
  const bool dir = fs::is_directory(path, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *size = dir ? 0 : fs::file_size(path, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *mtime = 0;
  *is_dir = dir ? 1 : 0;
  return MV_OK;
}
// A table without these two reads as one with no pixels, and then piece_dir answers nothing.
mv_status MV_CALL h_no_still(void*, const char*, uint32_t, uint8_t*, uint64_t, uint32_t*, uint32_t*) { return MV_ERR_UNSUPPORTED_FORMAT; }
mv_status MV_CALL h_no_sampler(void*, const char*, const mv_addon_sampler_options*, mv_addon_video_info*, void**) { return MV_ERR_UNSUPPORTED_FORMAT; }
int32_t MV_CALL h_no_yield(void*) { return 0; }
void MV_CALL h_drop(void*, const mv_addon_event*) {}
uint64_t MV_CALL h_corr(void*) {
  static std::atomic<uint64_t> id{1};
  return id++;
}
void MV_CALL h_log(void*, int32_t, const char*) {}

std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line[0] != '#') out.push_back(line);
  }
  return out;
}

std::string quoted(std::string v) {
  std::string q = "\"";
  for (char c : v) {
    if (c != '"') q += c;
  }
  return q + "\"";
}

// The owner's names, places and camera models never go into the repo: this
// writes them, for this run, beside the catalogue.
void write_local_queries(const std::string& gen, const std::string& out_path) {
  std::ofstream out(out_path);
  const auto each = [&](const std::string& file, const char* sql, const std::function<void(sqlite3_stmt*)>& visit) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK) {
      while (sqlite3_step(s) == SQLITE_ROW) visit(s);
    }
    sqlite3_finalize(s);
    sqlite3_close_v2(db);
  };
  const auto text = [](sqlite3_stmt* s, int c) {
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s, c));
    return std::string(t ? t : "");
  };
  each(gen + "/faces.db", "SELECT name FROM people ORDER BY id LIMIT 8", [&](sqlite3_stmt* s) {
    out << "@" << text(s, 0) << "\n";
    out << "person:" << quoted(text(s, 0)) << " mountains\n";
  });
  each(gen + "/catalog.db", "SELECT place, COUNT(*) FROM assets WHERE place IS NOT NULL GROUP BY place ORDER BY 2 DESC LIMIT 12", [&](sqlite3_stmt* s) {
    const std::string label = text(s, 0);
    if (label.starts_with("At sea")) return;
    const std::string part = label.substr(0, label.find(','));
    out << "place:" << quoted(part) << "\n";
    out << "place:" << quoted(part) << " snow\n";
  });
  each(gen + "/catalog.db", "SELECT camera, COUNT(*) FROM assets WHERE camera IS NOT NULL AND camera != '' GROUP BY camera ORDER BY 2 DESC LIMIT 5", [&](sqlite3_stmt* s) {
    out << "camera:" << quoted(text(s, 0)) << "\n";
  });
  each(gen + "/catalog.db", "SELECT strftime('%Y', taken_at, 'unixepoch'), COUNT(*) FROM assets WHERE taken_at IS NOT NULL GROUP BY 1 ORDER BY 2 DESC LIMIT 4", [&](sqlite3_stmt* s) {
    out << "in:" << text(s, 0) << "\n";
    out << "before:" << text(s, 0) << " is:video\n";
  });
  each(gen + "/catalog.db", "SELECT text FROM speech WHERE length(text) > 16 ORDER BY id LIMIT 10", [&](sqlite3_stmt* s) {
    std::string phrase = text(s, 0);
    phrase = phrase.substr(0, phrase.find(' ', 12));
    out << "said:" << quoted(phrase) << "\n";
  });
  each(gen + "/catalog.db", "SELECT path FROM assets WHERE kind IN (1,2) ORDER BY id LIMIT 3", [&](sqlite3_stmt* s) {
    const std::string path = text(s, 0);
    const std::string name = path.substr(path.rfind('/') + 1);
    out << "file:" << quoted(name.substr(0, std::min<std::size_t>(name.size(), 6))) << "\n";
  });
}

int publish(int argc, char** argv) {
  if (argc != 9) {
    std::fprintf(stderr, "publish <index dir (a copy)> <root path> <pack dir> <fetched dir> <places.mvgeo> <work> <destination>\n");
    return 2;
  }
  const std::string index_dir = argv[2], root_path = argv[3], pack = argv[4], fetched = argv[5], places = argv[6], work = argv[7], dest = argv[8];
  fs::create_directories(work);
  fs::create_directories(dest);
  auto geo = mv::geo::open_database(places);
  if (!geo) {
    std::fprintf(stderr, "no place database\n");
    return 1;
  }
  std::int64_t root_id = 0;
  {
    sqlite3* db = nullptr;
    sqlite3_open_v2((index_dir + "/index.db").c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM roots WHERE path = ?1", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, root_path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) root_id = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    sqlite3_close_v2(db);
  }
  if (!root_id) {
    std::fprintf(stderr, "root not found\n");
    return 1;
  }
  cat::root_map root;
  root.id = root_id;
  root.path = root_path;
  root.uuid = "11111111-2222-4333-8444-555555555555";
  root.share = "Photos";
  root.sub = fs::path(root_path).filename().string();

  const std::string thumbs_dir = work + "/thumbs";
  auto store = cat::thumb_store::open(thumbs_dir);
  if (!store) {
    std::fprintf(stderr, "no thumb store\n");
    return 1;
  }
  std::vector<std::int64_t> ids{root_id};
  auto wants = (*store)->missing(index_dir + "/index.db", ids);
  if (!wants) {
    std::fprintf(stderr, "thumb list failed\n");
    return 1;
  }
  std::printf("%zu tiles to make\n", wants->size());
  cat::thumb_fill_io io;
  io.jpeg512 = [](const cat::thumb_want& w) { return jpeg512_of(w); };
  io.tiny = [](std::span<const std::uint8_t> j) { return shrink(j); };
  io.yield = [] {};
  const cat::control quiet{[] { return false; }, [](double) {}, [] {}};
  auto made = (*store)->fill(*wants, io, quiet, 0);
  if (!made) {
    std::fprintf(stderr, "tiles failed\n");
    return 1;
  }
  std::printf("tiles: %llu made, %llu failed\n", static_cast<unsigned long long>(made->made), static_cast<unsigned long long>(made->failed));
  if (!(*store)->seal()) return 1;

  auto [public_key, secret_key] = cat::make_keypair();
  cat::build_options b;
  b.index_db = index_dir + "/index.db";
  b.out_dir = work + "/gen/1";
  b.library_id = "99999999-aaaa-4bbb-8ccc-dddddddddddd";
  b.generation = 1;
  b.epoch = 1;
  b.created_unix = static_cast<std::int64_t>(std::time(nullptr));
  {
    sqlite3* db = nullptr;
    sqlite3_open_v2(b.index_db.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'active_spec'", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) b.active_spec = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    sqlite3_close_v2(db);
  }
  b.labels_file = index_dir + "/labels.f32";
  b.settings_json = R"({"audio_files":true,"document_files":true,"tier":"default","faces":true})";
  b.roots = {root};
  b.thumbs = store->get();
  b.faces_db = index_dir + "/faces.db";
  const mv::geo::database& places_db = (*geo)->db();
  b.place_labels = [&places_db](std::span<const std::pair<double, double>> pts) -> mv::result<std::vector<std::string>> {
    std::vector<std::string> out;
    for (const auto& p : pts) out.push_back(places_db.lookup(p.first, p.second).label);
    return out;
  };
  b.model_spec = b.active_spec;
  const std::string tower = b.active_spec.starts_with("clip-vit-l14") ? "clip-l14" : "clip-b32";
  b.model_files = {pack + "/models/" + tower + "/model.json", fetched + "/ai/models/" + tower + "/text.onnx",
                   fetched + "/ai/models/clip-tokenizer/vocab.json", fetched + "/ai/models/clip-tokenizer/merges.txt"};
  const auto built = cat::build(b, quiet);
  if (!built) {
    std::fprintf(stderr, "build failed: %d\n", static_cast<int>(built.error()));
    return 1;
  }
  std::printf("built generation: %llu assets, %llu frames, %llu tiles, %llu people, %llu places, %llu bytes\n",
              static_cast<unsigned long long>(built->assets), static_cast<unsigned long long>(built->frames),
              static_cast<unsigned long long>(built->thumbs), static_cast<unsigned long long>(built->people),
              static_cast<unsigned long long>(built->places), static_cast<unsigned long long>(built->bytes));
  cat::deliver_options d;
  d.gen_dir = b.out_dir;
  d.destination = dest;
  d.library_id = b.library_id;
  d.generation = 1;
  d.now_unix = b.created_unix;
  d.public_key = public_key;
  d.secret_key = secret_key;
  d.shared = built->shared;
  d.retention_file = work + "/unused.json";
  d.make_directories = [](const std::string& dir) -> mv::expected {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? mv::expected{mv::err(mv::status::io)} : mv::expected{};
  };
  d.copy = [](const std::string& from, const std::string& to) -> mv::expected {
    std::error_code ec;
    fs::copy_file(from, to, ec);
    return ec ? mv::expected{mv::err(mv::status::io)} : mv::expected{};
  };
  if (!cat::deliver(d, quiet)) {
    std::fprintf(stderr, "delivery failed\n");
    return 1;
  }
  write_local_queries(dest + "/.mediaviewer/gen/000001", work + "/queries.local.txt");
  std::printf("delivered to %s\n", dest.c_str());
  return 0;
}

int search(int argc, char** argv) {
  if (argc != 9) {
    std::fprintf(stderr, "search <gen dir> <pack dir> <add-on dir (AI Audio, ...)> <work dir with fetched/> <kinds> <queries> <out.jsonl>\n");
    return 2;
  }
  const std::string gen = argv[2], pack = argv[3], addons = argv[4], work = argv[5];
  const uint32_t kinds = static_cast<uint32_t>(std::stoul(argv[6]));
  noop_host state;
  state.data_dir = work + "/engine";
  fs::create_directories(state.data_dir);
  for (const char* name : {"settings.json", "labels.f32", "faces.db"}) {
    std::error_code ec;
    fs::create_hard_link(fs::path(gen) / name, fs::path(state.data_dir) / name, ec);
  }
  std::error_code ec;
  fs::create_hard_link(fs::path(gen) / "catalog.db", fs::path(state.data_dir) / "index.db", ec);
  state.pieces = required_pieces(addons);
  mv_host_api api{};
  api.struct_size = sizeof api;
  api.host_api = MV_ADDON_HOST_API;
  api.host = &state;
  api.data_dir = &h_data_dir;
  api.piece_dir = &h_piece_dir;
  api.stat_file = &h_stat;
  api.should_yield = &h_no_yield;
  api.post_event = &h_drop;
  api.next_correlation_id = &h_corr;
  api.log = &h_log;
  api.decode_still_rgb = &h_no_still;
  api.sampler_open = &h_no_sampler;
  mv::ai::host host(&api);
  // The real pack and its fetched models, exactly as the desktop's search agent opens them.
  mv::ai::engine_deps deps = mv::ai::pack_deps(host, pack, work);
  // A phone has no sound text tower (the manifest carries only the picture tower's files), so
  // the desktop half answers without it too: the parity set is what a phone can answer. Sounds
  // on the phone would need CLAP's text model in the manifest (docs/plans/mobile.md §5).
  if (getenv("MV_PARITY_WITH_SOUND") == nullptr) deps.open_sound_text = nullptr;
  mv::ai::engine_options options;
  options.read_only = true;
  mv::ai::engine eng(&api, std::move(deps), options);
  if (!eng.start()) {
    std::fprintf(stderr, "engine failed\n");
    return 1;
  }
  for (;;) {
    mv_ai_status st{};
    st.struct_size = sizeof st;
    eng.status(st);
    if (st.state != MV_AI_STATE_LOADING) {
      std::printf("desktop engine state %u, %llu frames\n", st.state, static_cast<unsigned long long>(st.frames_indexed));
      if (st.state == MV_AI_STATE_ERROR) {
        auto rt = mv::infer::runtime::load(pack);
        std::printf("runtime: %s; %s\n", rt ? (*rt)->version().c_str() : "did not load", mv::infer::session::last_error().c_str());
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::ofstream out(argv[8]);
  for (const std::string& q : read_lines(argv[7])) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t id = eng.search_text(q, std::string(), MV_AI_SCOPE_ALL, kinds == 0 ? MV_AI_KIND_ALL : kinds);
    if (!eng.wait_search(id, 120000)) {
      std::fprintf(stderr, "timed out: %s\n", q.c_str());
      return 1;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mv::json::writer w;
    w.begin_object();
    w.key("query").string(q);
    w.key("ms").number(ms);
    w.key("rows").begin_array();
    auto count = eng.result_count(id);
    const std::uint32_t n = count ? *count : 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const auto r = eng.result_at(id, i);
      auto path = eng.result_path(id, i);
      if (!r || !path) continue;
      w.begin_array();
      w.string(*path);
      w.integer(r->pts_ms);
      w.integer(r->kind);
      w.integer(r->match);
      w.integer(r->more_in_clip);
      {
        char score[32];
        std::snprintf(score, sizeof score, "%.7f", r->score);
        w.raw(score);
      }
      w.end_array();
    }
    w.end_array();
    w.end_object();
    out << w.take() << '\n';
    eng.search_release(id);
  }
  eng.stop();
  return 0;
}


// ---- publish-engine: the Mac's own publisher, over a copy of its index ---------------------------
// engine::phone_publish, the code the app's "Publish now" runs: the root picked by volume, the
// signing key from a store, thumbnails through the host table, catalog::build and deliver with the
// host's verified copy. The host table is the real one (addon/host.h) over this machine's io; only
// what the app's viewer and Places add-on serve is supplied here (tiles from ImageIO and
// AVFoundation, place labels from src/geo).

struct tile_cache {
  std::string dir;
  std::mutex m;
  std::string file(const std::string& path, std::int64_t ms) const {
    std::size_t h = std::hash<std::string>{}(path + "#" + std::to_string(ms));
    return dir + "/" + std::to_string(h) + ".jpg";
  }
};

mv::result<mv::addon::rgb_image> frame_at(const std::string& path, std::int64_t ms, std::uint32_t max_edge) {
  @autoreleasepool {
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]] options:nil];
    AVAssetImageGenerator* g = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
    g.appliesPreferredTrackTransform = YES;
    g.maximumSize = CGSizeMake(max_edge, max_edge);
    g.requestedTimeToleranceBefore = kCMTimeZero;
    g.requestedTimeToleranceAfter = CMTimeMakeWithSeconds(0.5, 600);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block CGImageRef got = nullptr;
    [g generateCGImageAsynchronouslyForTime:CMTimeMakeWithSeconds(ms / 1000.0, 600)
                          completionHandler:^(CGImageRef cg, CMTime, NSError*) {
                            if (cg) got = CGImageRetain(cg);
                            dispatch_semaphore_signal(done);
                          }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (!got) return mv::err(mv::status::unsupported_format);
    const size_t w = CGImageGetWidth(got), h = CGImageGetHeight(got);
    mv::addon::rgb_image out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    std::vector<std::uint8_t> rgba(w * h * 4);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, rgb, kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(rgb);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), got);
    CGContextRelease(ctx);
    CGImageRelease(got);
    out.rgb.resize(w * h * 3);
    for (size_t i = 0; i < w * h; ++i) {
      out.rgb[i * 3] = rgba[i * 4];
      out.rgb[i * 3 + 1] = rgba[i * 4 + 1];
      out.rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return out;
  }
}

std::vector<std::uint8_t> jpeg_of_rgb(const mv::addon::rgb_image& f, CGFloat max_edge, CGFloat quality) {
  std::vector<std::uint8_t> rgba(f.width * f.height * 4, 255);
  for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i) {
    rgba[i * 4] = f.rgb[i * 3];
    rgba[i * 4 + 1] = f.rgb[i * 3 + 1];
    rgba[i * 4 + 2] = f.rgb[i * 3 + 2];
  }
  CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(rgba.data(), f.width, f.height, 8, f.width * 4, rgb, kCGImageAlphaNoneSkipLast);
  CGColorSpaceRelease(rgb);
  CGImageRef image = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  auto out = encode(image, max_edge, quality);
  CGImageRelease(image);
  return out;
}

bool write_all(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

mv::result<std::vector<std::uint8_t>> read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return mv::err(mv::status::io);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_answers(std::ofstream& out, mv::ai::engine& eng, const std::string& q, std::uint64_t id, double ms) {
  mv::json::writer w;
  w.begin_object();
  w.key("query").string(q);
  w.key("ms").number(ms);
  w.key("rows").begin_array();
  auto count = eng.result_count(id);
  const std::uint32_t n = count ? *count : 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    auto r = eng.result_at(id, i);
    auto path = eng.result_path(id, i);
    if (!r || !path) continue;
    w.begin_array();
    w.string(*path);
    w.integer(r->pts_ms);
    w.integer(r->kind);
    w.integer(r->match);
    w.integer(r->more_in_clip);
    char score[32];
    std::snprintf(score, sizeof score, "%.7f", r->score);
    w.raw(score);
    w.end_array();
  }
  w.end_array();
  w.end_object();
  out << w.take() << '\n';
}

// The host table both publish commands run over: the real one (addon/host.h) over this
// machine's io, with only what the app's viewer and Places add-on serve supplied here.
mv::addon::host_services harness_services(const std::string& data, const std::string& work, const std::string& addons,
                                          const mv::geo::database& places_db) {
  auto cache = std::make_shared<tile_cache>();
  cache->dir = work + "/tiles";

  mv::addon::host_services svc;
  svc.data_dir = data;
  svc.post = [](const mv_addon_event&) {};
  svc.should_yield = [] { return false; };
  svc.thumbnail_jpeg = [cache](const std::string& path, std::int64_t ms) -> mv::result<std::vector<std::uint8_t>> {
    return read_all(cache->file(path, ms));
  };
  svc.thumbnail = [cache](const std::string& path) -> mv::result<std::string> {
    cat::thumb_want w;
    w.path = path;
    w.kind = 1;
    MV_TRY(auto jpeg, jpeg512_of(w));
    const std::string file = cache->file(path, -1);
    if (!write_all(file, jpeg)) return mv::err(mv::status::io);
    return file;
  };
  svc.video_frame = [](const std::string& path, std::int64_t ms, std::uint32_t edge) { return frame_at(path, ms, edge); };
  svc.moment_thumbnail = [cache](const std::string& path, std::int64_t ms, const mv::addon::rgb_image* f) -> mv::result<std::string> {
    const std::string file = cache->file(path, ms);
    if (!f) return fs::exists(file) ? mv::result<std::string>(file) : mv::result<std::string>(mv::err(mv::status::io));
    if (!write_all(file, jpeg_of_rgb(*f, 512, 0.75))) return mv::err(mv::status::io);
    return file;
  };
  svc.jpeg_shrink = [](std::span<const std::uint8_t> jpeg, std::uint32_t edge, std::uint32_t quality) -> mv::result<std::vector<std::uint8_t>> {
    @autoreleasepool {
      NSData* d = [NSData dataWithBytesNoCopy:const_cast<std::uint8_t*>(jpeg.data()) length:jpeg.size() freeWhenDone:NO];
      CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, nullptr);
      if (!src) return mv::err(mv::status::corrupt);
      CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
      CFRelease(src);
      if (!image) return mv::err(mv::status::corrupt);
      auto out = encode(image, edge, quality / 100.0);
      CGImageRelease(image);
      return out;
    }
  };
  svc.place_labels = [&places_db](std::span<const std::pair<double, double>> pts) -> mv::result<std::vector<std::string>> {
    std::vector<std::string> out;
    for (const auto& p : pts) out.push_back(places_db.lookup(p.first, p.second).label);
    return out;
  };
  // `place:` as the Places add-on answers it (places.h place_query over each indexed file's
  // coordinates), not a rule of the harness's own. Clip routes (#606) are not walked here.
  svc.place_matches = [&places_db, index = data + "/index.db"](const std::string& place, const std::function<bool()>&)
      -> mv::result<std::vector<std::string>> {
    const auto q = mv::places::place_query::compile(places_db, mv::addon::fold_query(place));
    std::vector<std::string> out;
    if (q.empty()) return out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(index.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return mv::err(mv::status::io);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT path, lat, lon FROM assets WHERE lat IS NOT NULL AND lon IS NOT NULL", -1, &s, nullptr);
    while (s && sqlite3_step(s) == SQLITE_ROW) {
      if (!q.why(places_db, sqlite3_column_double(s, 1), sqlite3_column_double(s, 2), {}).empty()) {
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
      }
    }
    sqlite3_finalize(s);
    sqlite3_close_v2(db);
    return out;
  };
  svc.piece_dir = [pieces = required_pieces(addons)](const std::string& piece) -> mv::result<std::string> {
    auto it = pieces.find(piece);
    if (it == pieces.end()) return mv::err(mv::status::io);  // ai-cuda, ai-large: not on this Mac
    return it->second;
  };
  return svc;
}

// publish-engine <data dir: a copy of the index> <pack> <add-ons> <places.mvgeo> <work> <destination>
//                <kinds> <queries> <mac answers.jsonl>
int publish_engine(int argc, char** argv) {
  if (argc != 11) {
    std::fprintf(stderr, "publish-engine <data> <pack> <add-ons> <places.mvgeo> <work> <destination> <kinds> <queries> <out.jsonl>\n");
    return 2;
  }
  const std::string data = argv[2], pack = argv[3], addons = argv[4], places = argv[5], work = argv[6], dest = argv[7];
  const uint32_t kinds = static_cast<uint32_t>(std::stoul(argv[8]));
  fs::create_directories(work + "/tiles");
  fs::create_directories(work + "/pack-data");
  fs::create_directories(dest);
  std::error_code ec;
  fs::create_symlink(fs::path(pack).parent_path() / "data" / "fetched", fs::path(work) / "pack-data" / "fetched", ec);
  auto geo = mv::geo::open_database(places);
  if (!geo) {
    std::fprintf(stderr, "no place database\n");
    return 1;
  }
  mv::addon::host_services svc = harness_services(data, work, addons, (*geo)->db());
  mv::addon::host_table table(std::move(svc));
  mv::ai::host host(table.api());

  mv::ai::engine_deps deps = mv::ai::pack_deps(host, pack, work + "/pack-data");
  deps.fetch_models = nullptr;  // nothing downloads: the pack's fetched files are linked in
  deps.photos = [] { return std::unique_ptr<mv::ai::photos_source>(); };
  deps.cloud = [] { return std::unique_ptr<mv::ai::cloud_files>(); };
  const std::string keys = work + "/keys";
  fs::create_directories(keys);
  deps.key_store = [keys] { return mv::driftwire::secrets::file_store(keys); };
  mv::ai::engine eng(table.api(), std::move(deps));
  if (!eng.start()) {
    std::fprintf(stderr, "engine failed\n");
    return 1;
  }
  // Nothing new to index: the copy is indexed already. Paused, so no pass changes the index
  // between the snapshot the catalogue is built from and the Mac's answers below.
  eng.pause(true);
  // answer-engine: the Mac's answers only, over what an earlier publish-engine made.
  const bool publish = std::strcmp(argv[1], "publish-engine") == 0;
  if (publish && !eng.phone_publish(dest)) {
    std::fprintf(stderr, "publish refused\n");
    return 1;
  }
  for (; publish;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto doc = mv::json::parse(eng.phone_json(), 8);
    if (doc && doc->boolean("running") == false) {
      std::printf("publish: %s\n", eng.phone_json().c_str());
      const std::string* error = doc->str("error");
      if (!error || !error->empty()) return 1;
      break;
    }
  }
  if (auto secret = mv::driftwire::secrets::file_store(keys)->get("catalogue-signing-key")) {
    std::printf("publisher_key=%s\n", cat::to_hex(cat::public_of(*secret)).c_str());
  }

  // The Mac's own answers, from the engine that published (its models, its index).
  for (;;) {
    mv_ai_status st{};
    st.struct_size = sizeof st;
    eng.status(st);
    if (st.state != MV_AI_STATE_LOADING) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::printf("mac engine: %s\n", eng.settings_json().c_str());
  // The shipped words, then this library's own names, places, cameras and years (never in the
  // repo); the whole list goes beside the answers for the phone's run.
  std::string gen_dir;
  for (fs::directory_iterator it(fs::path(dest) / ".mediaviewer" / "gen", ec), end; !ec && it != end; it.increment(ec)) {
    if (fs::exists(it->path() / "manifest.json")) gen_dir = std::max(gen_dir, it->path().string());
  }
  write_local_queries(gen_dir, work + "/queries.local.txt");
  std::vector<std::string> queries = read_lines(argv[9]);
  for (std::string& q : read_lines(work + "/queries.local.txt")) {
    if (std::find(queries.begin(), queries.end(), q) == queries.end()) queries.push_back(std::move(q));
  }
  {
    std::ofstream all(work + "/queries.txt");
    for (const std::string& q : queries) all << q << '\n';
  }
  std::ofstream out(argv[10]);
  for (const std::string& q : queries) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t id = eng.search_text(q, std::string(), MV_AI_SCOPE_ALL, kinds == 0 ? MV_AI_KIND_ALL : kinds);
    if (!eng.wait_search(id, 120000)) return 1;
    write_answers(out, eng, q, id, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    eng.search_release(id);
  }
  eng.stop();
  return 0;
}


// ---- sample-publish: the sample library (docs/plans/mobile.md §11.1) ------------------------------
// Indexes a folder with the real engine over the app's own decoders (abi/addon_media.h), as the
// Mac app would, then runs the same engine::phone_publish, signed with the sample library's own key
// and under its fixed library id. tools/ios/sample-library.sh drives it.
//
// sample-publish <folder> <pack> <add-ons> <places.mvgeo> <work> <destination> <key file> <library id> <generation>
int sample_publish(int argc, char** argv) {
  if (argc != 11) {
    std::fprintf(stderr, "sample-publish <folder> <pack> <add-ons> <places.mvgeo> <work> <destination> <key file> <library id> <generation>\n");
    return 2;
  }
  const std::string folder = argv[2], pack = argv[3], addons = argv[4], places = argv[5], work = argv[6], dest = argv[7];
  const std::string key_file = argv[8], library_id = argv[9];
  const std::uint64_t generation = std::stoull(argv[10]);
  const std::string data = work + "/data";
  fs::create_directories(work + "/tiles");
  fs::create_directories(work + "/pack-data");
  fs::create_directories(data + "/phone");
  fs::create_directories(dest);
  std::error_code ec;
  fs::create_symlink(fs::path(pack).parent_path() / "data" / "fetched", fs::path(work) / "pack-data" / "fetched", ec);

  // The small tower (CLIP ViT-B/32: a 127 MB text half on the phone) on the CPU, where it is fast
  // and the same on every Mac; no people; nothing but photos and clips.
  {
    std::ofstream(data + "/settings.json")
        << R"({"compute":1,"quality":1,"faces":false,"read_text":false,"audio_files":false,"document_files":false,"tier":"default"})";
    mv::json::writer w;
    w.begin_object();
    w.key("library_id").string(library_id);
    w.key("epoch").integer(1);
    // Numbered from here, so a regenerated library always reads as newer.
    w.key("high_water").integer(static_cast<std::int64_t>(generation - 1));
    w.end_object();
    std::ofstream(data + "/phone/state.json") << w.take();
  }
  auto secret = read_all(key_file);
  if (!secret || secret->size() != 64) {
    std::fprintf(stderr, "no signing key in %s\n", key_file.c_str());
    return 1;
  }
  const std::string keys = work + "/keys";
  fs::create_directories(keys);
  if (!mv::driftwire::secrets::file_store(keys)->put("catalogue-signing-key", *secret)) return 1;

  auto geo = mv::geo::open_database(places);
  if (!geo) {
    std::fprintf(stderr, "no place database\n");
    return 1;
  }
  mv::addon::host_services svc = harness_services(data, work, addons, (*geo)->db());
  // The app's own pixels for the indexer, and its capture date and camera (the grid's order).
  svc.still_rgb = &mv::addon::media::decode_still;
  svc.open_sampler = &mv::addon::media::open_sampler;
  svc.video_frame = &mv::addon::media::video_frame;
  svc.capture = [](const std::string& path, mv_addon_capture& out) {
    auto m = mv::meta::read(path);
    if (!m) return false;
    if (m->s.date_taken_key != 0) {
      out.taken_unix = m->s.date_taken_key;
      out.has_date = 1;
    }
    const std::size_t n = std::min(m->s.camera.size(), sizeof(out.camera_utf8) - 1);
    std::memcpy(out.camera_utf8, m->s.camera.data(), n);
    out.camera_utf8[n] = '\0';
    return true;
  };
  mv::addon::host_table table(std::move(svc));
  mv::ai::host host(table.api());
  mv::ai::engine_deps deps = mv::ai::pack_deps(host, pack, work + "/pack-data");
  deps.fetch_models = nullptr;
  deps.photos = [] { return std::unique_ptr<mv::ai::photos_source>(); };
  deps.cloud = [] { return std::unique_ptr<mv::ai::cloud_files>(); };
  deps.key_store = [keys] { return mv::driftwire::secrets::file_store(keys); };
  mv::ai::engine eng(table.api(), std::move(deps));
  if (!eng.start()) {
    std::fprintf(stderr, "engine failed\n");
    return 1;
  }
  if (!eng.index_folder(folder, true)) {
    std::fprintf(stderr, "cannot index %s\n", folder.c_str());
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int idle = 0;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    mv_ai_status st{};
    st.struct_size = sizeof st;
    eng.status(st);
    if (st.state == MV_AI_STATE_ERROR) {
      std::fprintf(stderr, "the model would not load\n");
      return 1;
    }
    const bool done = st.state == MV_AI_STATE_IDLE && st.assets_total > 0 && st.assets_done + st.assets_failed >= st.assets_total;
    idle = done ? idle + 1 : 0;
    if (idle >= 4) {  // two seconds idle: the scan and every pass are over
      std::printf("indexed: %llu assets, %llu failed, %llu frames, %s\n", (unsigned long long)st.assets_done,
                  (unsigned long long)st.assets_failed, (unsigned long long)st.frames_indexed, st.model_utf8);
      if (st.assets_failed != 0) return 1;
      break;
    }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(30)) {
      std::fprintf(stderr, "indexing took too long\n");
      return 1;
    }
  }
  eng.pause(true);  // nothing changes the index under the snapshot
  if (!eng.phone_publish(dest)) {
    std::fprintf(stderr, "publish refused\n");
    return 1;
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto doc = mv::json::parse(eng.phone_json(), 8);
    if (doc && doc->boolean("running") == false) {
      std::printf("publish: %s\n", eng.phone_json().c_str());
      const std::string* error = doc->str("error");
      if (!error || !error->empty()) return 1;
      break;
    }
  }
  std::printf("publisher_key=%s\n", cat::to_hex(cat::public_of(*secret)).c_str());
  eng.stop();
  return 0;
}

// sample-sign <files.json> <key file>: the sample library's signed list (src/mobile/sample.h), in
// place: the list becomes {"signature": Ed25519 over kFilesContext and its bytes, "list": its text}.
int sample_sign(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "sample-sign <files.json> <key file>\n");
    return 2;
  }
  auto text = read_all(argv[2]);
  auto secret = read_all(argv[3]);
  if (!text || !secret || secret->size() != crypto_sign_SECRETKEYBYTES || sodium_init() < 0) return 1;
  const std::string list(text->begin(), text->end());
  std::string msg(mv::mobile::sample::kFilesContext);
  msg += list;
  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), secret->data());
  mv::json::writer w;
  w.begin_object();
  w.key("signature").string(cat::to_hex(std::span<const std::uint8_t>(sig, sizeof sig)));
  w.key("list").string(list);
  w.end_object();
  std::ofstream(argv[2], std::ios::binary | std::ios::trunc) << w.take();
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // lines reach a pipe as they are written
  if (argc >= 2 && std::strcmp(argv[1], "publish") == 0) return publish(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "search") == 0) return search(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "sample-publish") == 0) return sample_publish(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "sample-sign") == 0) return sample_sign(argc, argv);
  if (argc >= 2 && (std::strcmp(argv[1], "publish-engine") == 0 || std::strcmp(argv[1], "answer-engine") == 0)) {
    return publish_engine(argc, argv);
  }
  std::fprintf(stderr, "usage: %s publish|search ...\n", argv[0]);
  return 2;
}
