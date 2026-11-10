// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop half of docs/plans/mobile.md §9's parity test, macOS only.
//
//   publish  catalog::build and deliver from a copy of an index, the desktop's own publisher code,
//            with real tiles (ImageIO, AVFoundation) and real place labels (src/geo).
//   search   the desktop's engine through pack_deps over the real AI pack, reading a generation,
//            and writes one JSON line per query (compare_parity.py).
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <atomic>
#include <span>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_ai.h>

#include "addons/ai/catalog.h"
#include "addons/ai/engine.h"
#include "addons/ai/host.h"
#include "addons/ai/pack.h"
#include "core/json.h"
#include "geo/geo.h"
#include "geo/open.h"
#include "infer/ort.h"

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
  if (fs::exists(addons + "/AI Audio/0.1.41")) state.pieces["ai-audio"] = addons + "/AI Audio/0.1.41";
  if (fs::exists(addons + "/AI Documents/0.1.41")) state.pieces["ai-docs"] = addons + "/AI Documents/0.1.41";
  if (fs::exists(addons + "/AI Faces/0.1.41")) state.pieces["ai-faces"] = addons + "/AI Faces/0.1.41";
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

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "publish") == 0) return publish(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "search") == 0) return search(argc, argv);
  std::fprintf(stderr, "usage: %s publish|search ...\n", argv[0]);
  return 2;
}
