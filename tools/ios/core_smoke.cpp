// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Proves the static iOS link at run time (docs/plans/mobile.md 8.1):
// tools/ios/run-simulator-smoke.sh runs this inside a Simulator.
//
//   - ONNX Runtime, reached without dlopen, runs a real two-node graph.
//   - The search engine indexes three files through a fake model and answers a
//     text search.
//   - The Driftwire engine opens over loopback, and its iOS ports answer.
//
// Exit 0 only if every check held; one line per check on stdout.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addons/ai/engine.h"
#include "addons/driftwire/engine.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/power.h"
#include "addons/driftwire/presence.h"
#include "addons/driftwire/secrets.h"
#include "infer/models.h"
#include "infer/ort.h"

namespace fs = std::filesystem;

namespace {

int g_failed = 0;

void check(bool held, const char* what) {
  std::printf("%s %s\n", held ? "ok  " : "FAIL", what);
  std::fflush(stdout);
  if (!held) ++g_failed;
}

// ---- a tiny ONNX file, written by hand: y = x + x, x a float[3] --------------

void varint(std::string& out, std::uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}
void field_int(std::string& out, int field, std::uint64_t v) {
  varint(out, static_cast<std::uint64_t>(field) << 3);
  varint(out, v);
}
void field_bytes(std::string& out, int field, std::string_view v) {
  varint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
  varint(out, v.size());
  out.append(v);
}

std::string value_info(const char* name) {
  std::string dim, shape, tensor, type, info;
  field_int(dim, 1, 3);        // Dimension.dim_value
  field_bytes(shape, 1, dim);  // TensorShapeProto.dim
  field_int(tensor, 1, 1);     // Tensor.elem_type = FLOAT
  field_bytes(tensor, 2, shape);
  field_bytes(type, 1, tensor);  // TypeProto.tensor_type
  field_bytes(info, 1, name);
  field_bytes(info, 2, type);
  return info;
}

std::string add_model() {
  std::string node, graph, opset, model;
  field_bytes(node, 1, "x");
  field_bytes(node, 1, "x");
  field_bytes(node, 2, "y");
  field_bytes(node, 4, "Add");
  field_bytes(graph, 1, node);
  field_bytes(graph, 2, "g");
  field_bytes(graph, 11, value_info("x"));
  field_bytes(graph, 12, value_info("y"));
  field_int(opset, 2, 13);  // OperatorSetIdProto.version
  field_int(model, 1, 8);   // ir_version
  field_bytes(model, 7, graph);
  field_bytes(model, 8, opset);
  return model;
}

void check_runtime(const fs::path& dir) {
  auto rt = mv::infer::runtime::load("");
  check(rt.has_value(), "ONNX Runtime reached through the static seam");
  if (!rt) return;
  std::printf("     runtime %s, CoreML provider %s\n", (*rt)->version().c_str(),
              (*rt)->has_provider(mv::infer::backend::coreml) ? "linked" : "absent");
  check(!(*rt)->version().empty(), "the runtime reports its version");
  const char* opt_out = std::getenv("ORT_DISABLE_TELEMETRY");
  // Only that the seam ran first. What the variable does is pinned at build
  // time for this ONNX Runtime version (tools/ios/ort_telemetry_pin.py).
  check(opt_out && std::string_view(opt_out) == "1", "the telemetry opt-out is set before the runtime starts");

  const fs::path file = dir / "add.onnx";
  std::ofstream(file, std::ios::binary) << add_model();
  auto s = mv::infer::session::open(**rt, file.string(), {});
  if (!s) std::printf("     %s\n", mv::infer::session::last_error().c_str());
  check(s.has_value(), "a session opens on a real graph");
  if (!s) return;
  const std::vector<mv::infer::tensor_f32> in = {{{3}, {1, 2, 3.5f}}};
  auto out = (*s)->run(in);
  check(out.has_value() && out->size() == 1 && (*out)[0].data == std::vector<float>{2, 4, 7},
        "the graph runs and its answer is right");
}

// ---- the search engine, through a fake model ---------------------------------

constexpr std::uint32_t kDim = 8;

std::vector<float> colour_vec(float r, float g, float b) {
  std::vector<float> v{r, g, b, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
  mv::infer::l2_normalise(v);
  return v;
}

class fake_embedder final : public mv::infer::embedder {
 public:
  std::uint32_t dim() const noexcept override { return kDim; }
  const std::string& spec_key() const noexcept override { return key_; }
  mv::infer::backend on() const noexcept override { return mv::infer::backend::cpu; }
  mv::expected embed_images(std::span<const mv::infer::rgb_view> images, std::vector<float>& out) override {
    out.clear();
    for (const auto& img : images) {
      const auto v = colour_vec(img.rgb[0] / 255.f, img.rgb[1] / 255.f, img.rgb[2] / 255.f);
      out.insert(out.end(), v.begin(), v.end());
    }
    return {};
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override {
    if (text.find("red") != std::string_view::npos) return colour_vec(1, 0, 0);
    if (text.find("blue") != std::string_view::npos) return colour_vec(0, 0, 1);
    return colour_vec(0.3f, 0.3f, 0.3f);
  }

 private:
  std::string key_ = "fake/fp16/pre1";
};

// The host function table, as far as stills in one folder need it.
struct smoke_host {
  fs::path data;
};

int32_t MV_CALL no_yield(void*) { return 0; }
void MV_CALL drop_event(void*, const mv_addon_event*) {}
uint64_t MV_CALL next_id(void*) {
  static std::atomic<uint64_t> id{1};
  return id++;
}
void MV_CALL drop_log(void*, int32_t, const char*) {}

mv_status MV_CALL data_dir(void* h, char* out, uint32_t cap) {
  const std::string d = static_cast<smoke_host*>(h)->data.string();
  if (d.size() + 1 > cap) return MV_ERR_INVALID_ARG;
  std::memcpy(out, d.c_str(), d.size() + 1);
  return MV_OK;
}

mv_status MV_CALL make_dirs(void*, const char* dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  return ec ? MV_ERR_IO : MV_OK;
}

mv_status MV_CALL remove_one(void*, const char* path) {
  std::error_code ec;
  fs::remove(path, ec);
  return ec ? MV_ERR_IO : MV_OK;
}

mv_status MV_CALL stat_one(void*, const char* path, uint64_t* size, int64_t* mtime, uint32_t* is_dir) {
  std::error_code ec;
  const bool dir = fs::is_directory(path, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *size = dir ? 0 : fs::file_size(path, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *mtime = 1700000000;
  *is_dir = dir ? 1 : 0;
  return MV_OK;
}

mv_status MV_CALL walk(void*, const char* root, int32_t, mv_addon_walk_fn visit, void* user) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (!e.is_regular_file()) continue;
    const std::string path = e.path().string();
    const std::string name = e.path().filename().string();
    const mv_addon_file_entry entry{path.c_str(), name.c_str(), name.c_str(),
                                    static_cast<uint64_t>(fs::file_size(e.path())), 1700000000};
    if (!visit(user, &entry)) break;
  }
  return ec ? MV_ERR_IO : MV_OK;
}

// A solid picture in the colour its name says.
mv_status MV_CALL decode_still(void*, const char* path, uint32_t, uint8_t* out, uint64_t cap, uint32_t* w,
                               uint32_t* h) {
  *w = 32;
  *h = 24;
  if (cap < 32u * 24u * 3u) return MV_ERR_INVALID_ARG;
  const std::string_view name = path;
  const bool red = name.find("red") != std::string_view::npos;
  const bool blue = name.find("blue") != std::string_view::npos;
  for (std::size_t i = 0; i < 32u * 24u * 3u; i += 3) {
    out[i] = red ? 220 : 20;
    out[i + 1] = !red && !blue ? 220 : 20;
    out[i + 2] = blue ? 220 : 20;
  }
  return MV_OK;
}

// No clips here, but the engine reads a table without a sampler as one with no pixels.
mv_status MV_CALL no_sampler(void*, const char*, const mv_addon_sampler_options*, mv_addon_video_info*, void**) {
  return MV_ERR_UNSUPPORTED_FORMAT;
}

mv::ai::engine_deps search_deps(std::shared_ptr<fake_embedder> model) {
  mv::ai::engine_deps d;
  d.qualities = [] { return std::vector<std::uint32_t>{1}; };
  d.model_name = [](std::uint32_t) { return std::string("Fake"); };
  d.backend_available = [](mv::infer::backend) { return false; };
  d.clip_spec = [model](std::uint32_t) { return model->spec_key(); };
  d.open_clip = [model](std::uint32_t quality, std::uint32_t) -> mv::result<mv::ai::loaded_clip> {
    mv::ai::loaded_clip c;
    c.model = model;
    c.on = mv::infer::backend::cpu;
    c.meta.name = "Fake";
    c.meta.spec_key = model->spec_key();
    c.meta.quality = quality;
    c.meta.dim = kDim;
    c.meta.input_edge = 16;
    c.meta.dedupe = 0.97f;
    c.meta.query_margin = 0.04f;
    c.meta.result_margin = 0.015f;
    c.meta.similar_min = 0.9f;
    c.meta.generic_prompts = {"a photo.", "an image."};
    return c;
  };
  d.open_faces = [](std::uint32_t) -> mv::result<std::unique_ptr<mv::ai::face_analyzer>> {
    return mv::err(mv::status::io);
  };
  d.open_sound = [](std::uint32_t) -> mv::result<mv::ai::loaded_sound> { return mv::err(mv::status::io); };
  d.open_speech = [](std::uint32_t, std::uint32_t) -> mv::result<mv::ai::loaded_speech> {
    return mv::err(mv::status::io);
  };
  d.open_docs = []() -> mv::result<mv::ai::loaded_docs> { return mv::err(mv::status::io); };
  d.runtime_version = [] { return std::string("fake"); };
  return d;
}

void check_search(const fs::path& dir) {
  smoke_host state{dir / "data"};
  const fs::path photos = dir / "Photos";
  fs::create_directories(photos);
  for (const char* n : {"red_car.jpg", "green_field.jpg", "blue_sea.jpg"}) {
    std::ofstream(photos / n, std::ios::binary) << n;
  }

  mv_host_api api{};
  api.struct_size = sizeof api;
  api.host_api = MV_ADDON_HOST_API;
  api.host = &state;
  api.walk_files = &walk;
  api.stat_file = &stat_one;
  api.make_directories = &make_dirs;
  api.remove_file = &remove_one;
  api.decode_still_rgb = &decode_still;
  api.sampler_open = &no_sampler;
  api.should_yield = &no_yield;
  api.post_event = &drop_event;
  api.next_correlation_id = &next_id;
  api.data_dir = &data_dir;
  api.log = &drop_log;

  mv::ai::engine_options options;
  options.background_threads = false;  // a Simulator under load should not time out
  mv::ai::engine eng(&api, search_deps(std::make_shared<fake_embedder>()), options);
  check(eng.start().has_value(), "the search engine starts (SQLite, the platform stubs)");
  auto root = eng.index_folder(photos.string(), false);
  check(root.has_value(), "a folder is remembered");
  check(eng.wait_idle(60000), "three files index through the fake model");

  mv_ai_status status{};
  status.struct_size = sizeof status;
  eng.status(status);
  std::printf("     indexed %llu of %llu assets, %llu failed, %llu frames\n",
              static_cast<unsigned long long>(status.assets_done), static_cast<unsigned long long>(status.assets_total),
              static_cast<unsigned long long>(status.assets_failed),
              static_cast<unsigned long long>(status.frames_indexed));
  const std::uint64_t id = eng.search_text("something red", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  check(eng.wait_search(id, 30000), "a text search finishes");
  auto count = eng.result_count(id);
  std::printf("     %u results\n", count ? *count : 0u);
  auto first = eng.result_path(id, 0);
  auto hit = eng.result_at(id, 0);
  check(first.has_value() && fs::path(*first).filename() == "red_car.jpg" && hit.has_value() && hit->match != 0,
        "the text search finds the red picture");
  eng.search_release(id);
  eng.stop();
}

// ---- Driftwire ----------------------------------------------------------------

class smoke_engine_host final : public mv::driftwire::engine_host {
 public:
  void post_tree(const std::string&) override { ++trees; }
  void show_screen(bool) override {}
  std::string default_destination() override { return {}; }
  std::optional<std::uint64_t> free_bytes(const std::string&) override { return std::nullopt; }
  bool should_yield() override { return false; }
  std::vector<std::uint32_t> pair_partners(const std::vector<std::string>&) override { return {}; }
  void log(int, const char*) override {}
  std::atomic<int> trees{0};
};

class silent_discovery final : public mv::driftwire::discovery {
 public:
  bool start(const mv::driftwire::beacon&, seen_fn, tick_fn) override { return true; }
  void stop() noexcept override {}
};

void check_driftwire(const fs::path& dir) {
  namespace dw = mv::driftwire;
  check(dw::net::machine_name() == "iPhone" || dw::net::machine_name() == "iPad", "this device has a name");
  check(!dw::net::default_gateway().has_value(), "the routing table is not asked for on iOS");
  check(dw::power::hold(true) == nullptr, "stay-awake leaves it to the app");

  // TCP through net_posix.cpp over loopback, both ends.
  auto listener = dw::net::listen(true);
  check(listener.has_value(), "the TCP port listens on loopback");
  if (listener) {
    auto out = dw::net::connect({"127.0.0.1", (*listener)->port()}, 5000);
    auto in = (*listener)->accept(5000);
    bool moved = false;
    if (out && in) {
      const std::uint8_t sent[] = {7, 8, 9};
      std::uint8_t got[3] = {};
      moved = (*out)->write_all(sent).has_value();
      if (moved) {
        auto n = (*in)->read_some(got, 5000);
        moved = n.has_value() && *n > 0 && got[0] == 7;
      }
    }
    check(moved, "bytes cross a loopback TCP link");
  }

  // The keychain needs an app's entitlements, which a bare test program has
  // not got: report what it did and move on (a real device check).
  const std::uint8_t secret[] = {1, 2, 3};
  auto store = dw::secrets::os_store("MediaViewer Driftwire smoke");
  check(store != nullptr, "the keychain store exists");
  if (store) {
    const bool put = store->put("smoke", secret);
    std::printf("     keychain put from a bare test program: %s\n", put ? "stored" : "refused (no entitlement)");
    if (put) (void)store->erase("smoke");
  }

  smoke_engine_host host;
  dw::engine::options o;
  o.my_name = "smoke";
  o.make_discovery = [] { return std::unique_ptr<dw::discovery>(new silent_discovery()); };
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.internet_mapping = std::nullopt;
  o.make_secrets = [dir] { return dw::secrets::file_store((dir / "secrets").string()); };
  {
    dw::engine eng(host, std::move(o));
    eng.screen_opened("");
    for (int i = 0; i < 500 && host.trees.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(host.trees.load() > 0, "the Driftwire engine opens and posts its view");
    eng.screen_closed();
  }
  check(true, "the Driftwire engine stops");
}

}  // namespace

int main() {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / ("mv-core-smoke-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  check_runtime(dir);
  check_search(dir);
  check_driftwire(dir);
  fs::remove_all(dir, ec);
  std::printf(g_failed ? "SMOKE FAILED (%d)\n" : "SMOKE PASSED\n", g_failed);
  return g_failed ? 1 : 0;
}
