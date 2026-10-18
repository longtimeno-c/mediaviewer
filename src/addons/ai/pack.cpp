// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/ai/platform.h"
#include "addons/ai/tiers.h"
#include "infer/audio_models.h"
#include "infer/text_models.h"
#include "infer/models.h"
#include "core/json.h"
#include "infer/ort.h"

namespace mv::ai {
namespace {

std::string join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// The pack, opened once on first use (the engine's control thread).
struct pack_state {
  const host* h = nullptr;
  std::string core_dir;
  std::string data_dir;
  std::once_flag once;
  std::once_flag cache_pruned;
  std::atomic<bool> ready{false};  // ensure() has run (loaded or not)
  std::unique_ptr<infer::runtime> rt;
  bool from_piece = false;  // ORT came from the ai-cuda piece
  // Model tiers (#482, tiers.h): the profile in force and the towers it
  // names. tier_m guards both; a change of tier (set_tier, any thread) only
  // marks them stale, and the next use on the control thread re-reads them,
  // so the UI thread never reads a model.json.
  std::mutex tier_m;
  bool tiers_read = false;
  bool tier_stale = true;
  tier_set tiers;
  std::string tiers_text;  // models/tiers.json as read, for the picker (tiers_json)
  std::string tier_id;  // the engine's setting; "" = the pack's default
  tier_profile tier_;
  std::map<std::uint32_t, infer::clip_spec> towers;  // by mv_ai_quality, for tier_

  // Core ML compiles (upgrading_clip). One tower compiles once at a time: a
  // first compile of L/14 writes gigabytes for minutes on a MacBook Air, and
  // a second beside it (a reload while the first ran) doubled that and the
  // memory. An open of a tower on a compute choice that is still alive is
  // handed the same instance rather than compiling it again.
  std::mutex compile_m;
  std::condition_variable compile_cv;
  std::set<std::string> compiling;                                  // spec keys
  std::map<std::string, std::weak_ptr<infer::embedder>> upgrading;  // spec key | compute
  std::mutex verdict_m;  // provider.txt (read_verdict / write_verdict)

  void ensure() {
    std::call_once(once, [this] {
      // A vendor piece carries its own ORT build (CUDA builds run CPU too);
      // with none installed, the Core pack's CPU build (docs/design/17 "Runtime").
      std::string ort_dir = core_dir;
      if (auto cuda = h->piece_dir("ai-cuda")) ort_dir = *cuda;
      auto loaded = infer::runtime::load(ort_dir);
      from_piece = loaded && ort_dir != core_dir;
      if (!loaded && ort_dir != core_dir) loaded = infer::runtime::load(core_dir);
      if (!loaded) return;
      rt = std::move(*loaded);
    });
    ready = true;
  }

  // Where the downloader puts a piece's upstream files (fetch.h): the same
  // layout as the piece, under the add-on's data folder.
  std::string fetched(const std::string& piece) const { return join(join(data_dir, "fetched"), piece); }

  // The profile in force, re-read after a change of tier. I/O on first use
  // and after a change only: the control thread's, never mv_addon_get's.
  tier_profile profile() {
    std::lock_guard lock(tier_m);
    refresh_locked();
    return tier_;
  }

  void refresh_locked() {
    if (!tiers_read) {
      tiers = read_tier_set(core_dir);
      std::ifstream in(fs_path(join(join(core_dir, "models"), "tiers.json")), std::ios::binary);
      std::ostringstream text;
      if (in) text << in.rdbuf();
      tiers_text = text.str();
      tiers_read = true;
    }
    if (!tier_stale) return;
    tier_stale = false;
    const std::string want = tier_id.empty() ? tiers.default_id : tier_id;
    tier_ = tiers.tiers.front();
    for (const tier_profile& t : tiers.tiers) {
      if (t.id == want) tier_ = t;
    }
    // The tier names the towers; its slot, not model.json, says which
    // quality each one answers for.
    towers.clear();
    for (const auto& [quality, folder] : {std::pair{MV_AI_QUALITY_FAST, &tier_.picture_fast},
                                          std::pair{MV_AI_QUALITY_HIGH, &tier_.picture_high}}) {
      if (folder->empty()) continue;
      auto spec = picture_spec(*folder);
      if (!spec) continue;
      spec->quality = quality;
      towers[quality] = std::move(*spec);
    }
  }

  // A picture tower's model.json: in the Core pack, or in the ai-large piece
  // (#482: Maxxed's tower, too large for the Core pack). Its upstream files
  // come from the same piece's fetched folder.
  result<infer::clip_spec> picture_spec(const std::string& folder) const {
    std::error_code ec;
    if (std::filesystem::exists(fs_path(join(join(core_dir, folder), "model.json")), ec)) {
      return infer::read_clip_spec(join(core_dir, folder), join(fetched("ai"), folder));
    }
    MV_TRY(std::string large, h->piece_dir("ai-large"));
    return infer::read_clip_spec(join(large, folder), join(fetched("ai-large"), folder));
  }

  void set_tier(const std::string& id) {
    std::lock_guard lock(tier_m);
    if (id == tier_id && !tier_stale) return;
    tier_id = id;
    tier_stale = true;
  }

  // A tower of the tier in force; nullopt when it names none for `quality`.
  // `fresh`: re-read a stale tier first (the control thread); the UI
  // thread's informational calls take what is there.
  std::optional<infer::clip_spec> tower(std::uint32_t quality, bool fresh = true) {
    std::lock_guard lock(tier_m);
    if (fresh) refresh_locked();
    auto it = towers.find(quality);
    if (it == towers.end()) return std::nullopt;
    return it->second;
  }

  std::vector<std::uint32_t> qualities() {
    std::lock_guard lock(tier_m);
    std::vector<std::uint32_t> q;
    for (const auto& [k, v] : towers) q.push_back(k);
    return q;
  }

  // The Whisper folder for a quality: the slot asked for, else the other one.
  result<infer::whisper_spec> speech_spec(const std::string& audio_dir, std::uint32_t quality) {
    const tier_profile t = profile();
    const std::string& first = quality >= MV_AI_QUALITY_HIGH && !t.speech_high.empty() ? t.speech_high : t.speech_fast;
    const std::string& second = &first == &t.speech_high ? t.speech_fast : t.speech_high;
    if (first.empty()) return err(status::unsupported_format);
    auto spec = infer::read_whisper_spec(join(audio_dir, first), join(fetched("ai-audio"), first));
    if (!spec && !second.empty()) spec = infer::read_whisper_spec(join(audio_dir, second), join(fetched("ai-audio"), second));
    return spec;
  }

  // ---- downloads (#482 T3) --------------------------------------------------------
  // The upstream files a piece's signed manifest lists for fetching, which
  // this tier needs, as paths under fetched(): "<piece>/models/<folder>/<file>".
  // The host verified that manifest before the piece's folder was handed out.
  std::vector<remote_file> needed(const std::string& piece, const std::string& dir, const tier_profile& t,
                                  bool pictures_only) const {
    std::vector<remote_file> out;
    std::ifstream in(fs_path(join(dir, "manifest.json")), std::ios::binary);
    if (!in) return out;
    std::ostringstream text;
    text << in.rdbuf();
    const auto doc = json::parse(text.str(), 8);
    const json::value* remote = doc ? doc->find("remote") : nullptr;
    if (!remote || remote->k != json::kind::array) return out;
    std::vector<std::string> folders;
    if (auto f = t.folders.find(piece); f != t.folders.end()) folders = f->second;
    if (pictures_only) {
      // The picture towers and what they share (the tokenizer folder):
      // search opens on these alone.
      std::vector<std::string> keep;
      for (const std::string& f : folders) {
        const bool tower = f == t.picture_high || f == t.picture_fast;
        if (tower || f.find("tokenizer") != std::string::npos) keep.push_back(f);
      }
      folders = std::move(keep);
    }
    for (const json::value& r : remote->a) {
      const std::string* url = r.str("url");
      const std::string* path = r.str("path");
      const std::string* sha = r.str("sha256");
      const auto size = r.integer("size");
      if (!url || !path || !sha || !size || *size <= 0) continue;
      const bool wanted = t.folders.empty() || std::any_of(folders.begin(), folders.end(), [&](const std::string& f) {
                            return path->rfind(f + "/", 0) == 0;
                          });
      if (!wanted) continue;
      out.push_back(remote_file{*url, piece + "/" + *path, *sha, static_cast<std::uint64_t>(*size)});
    }
    return out;
  }

  // Everything the tier in force needs, the pictures first: (pictures, rest).
  std::pair<std::vector<remote_file>, std::vector<remote_file>> downloads() {
    const tier_profile t = profile();
    auto pictures = needed("ai", core_dir, t, true);
    std::vector<remote_file> rest;
    for (const remote_file& f : needed("ai", core_dir, t, false)) {
      if (std::none_of(pictures.begin(), pictures.end(), [&](const remote_file& g) { return g.path == f.path; })) {
        rest.push_back(f);
      }
    }
    if (auto large = h->piece_dir("ai-large")) {
      auto more = needed("ai-large", *large, t, true);
      pictures.insert(pictures.end(), more.begin(), more.end());
    }
    for (const char* piece : {"ai-audio", "ai-faces", "ai-docs"}) {
      if (auto dir = h->piece_dir(piece)) {
        auto more = needed(piece, *dir, t, false);
        rest.insert(rest.end(), more.begin(), more.end());
      }
    }
    return {std::move(pictures), std::move(rest)};
  }
};

infer::backend accelerated(const infer::runtime& rt) {
  for (infer::backend b : {infer::backend::coreml, infer::backend::cuda, infer::backend::openvino}) {
    if (rt.has_provider(b)) return b;
  }
  return infer::backend::cpu;
}

// A fixed, deterministic test card for the self-test.
std::vector<std::uint8_t> test_card(std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
      p[0] = static_cast<std::uint8_t>((x * 7 + y * 3) & 0xFF);
      p[1] = static_cast<std::uint8_t>((x * y) & 0xFF);
      p[2] = static_cast<std::uint8_t>((x ^ y) & 0xFF);
    }
  }
  return px;
}

// The CPU half of the provider self-test (docs/design/17), kept so a later start
// skips opening a CPU session and timing it: about 15 s of a 40 s ViT-L load
// on a GPU. The provider itself still runs every start and is checked against
// this reference embedding and time; a new runtime, provider, piece or model
// is a different key and tests afresh. Holds no path or pixel of the user's.
struct selftest_ref {
  std::string key;
  double cpu_ms = -1;
  std::vector<float> emb;
};

std::optional<selftest_ref> read_selftest(const std::string& path, const std::string& key) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::string tag, k;
  if (!in || !std::getline(in, tag) || tag != "mv-ai-selftest 1" || !std::getline(in, k) || k != key) {
    return std::nullopt;
  }
  selftest_ref r;
  r.key = k;
  std::size_t n = 0;
  if (!(in >> r.cpu_ms >> n) || r.cpu_ms <= 0 || n == 0 || n > 4096) return std::nullopt;
  r.emb.resize(n);
  for (float& f : r.emb) {
    if (!(in >> f)) return std::nullopt;
  }
  return r;
}

void write_selftest(const std::string& path, const selftest_ref& r) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.precision(9);
    out << "mv-ai-selftest 1\n" << r.key << '\n' << r.cpu_ms << ' ' << r.emb.size() << '\n';
    for (float f : r.emb) out << f << ' ';
    out << '\n';
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(fs_path(tmp), fs_path(path), ec);
}

// How an accelerated provider did with a tower on this machine (2026-10-05),
// in <data>/provider.txt, one line per "<runtime>|<provider>|<spec>":
// "ok" or the fault's name, the open's seconds, and the provider's message
// (session::last_error, paths replaced). Auto reads it so a large tower that
// failed here is not run on CPU at a twelfth of the small one's speed, nor
// compiled again (minutes, gigabytes) every start. A new runtime is a new
// key and tries again. Holds nothing of the user's.
struct provider_verdict {
  std::string outcome;  // "ok" or infer::fault_name
  double open_s = -1;
  std::string detail;
};

std::string verdict_key(const infer::runtime& rt, infer::backend on, const std::string& spec_key) {
  return rt.version() + "|" + infer::backend_name(on) + "|" + spec_key;
}

std::map<std::string, provider_verdict> read_verdicts(const std::string& path) {
  std::map<std::string, provider_verdict> out;
  std::ifstream in(fs_path(path), std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != "mv-ai-provider 1") return out;
  while (std::getline(in, line)) {
    const std::size_t a = line.find('\t');
    const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    const std::size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
    if (c == std::string::npos) continue;
    provider_verdict v;
    v.outcome = line.substr(a + 1, b - a - 1);
    v.open_s = std::atof(line.substr(b + 1, c - b - 1).c_str());
    v.detail = line.substr(c + 1);
    out[line.substr(0, a)] = std::move(v);
  }
  return out;
}

void write_verdict(pack_state& p, const std::string& key, provider_verdict v) {
  for (char& ch : v.detail) {
    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  }
  if (v.detail.size() > 400) v.detail.resize(400);
  const std::string path = join(p.data_dir, "provider.txt");
  std::lock_guard lock(p.verdict_m);
  auto all = read_verdicts(path);
  all[key] = std::move(v);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "mv-ai-provider 1\n";
    for (const auto& [k, e] : all) out << k << '\t' << e.outcome << '\t' << e.open_s << '\t' << e.detail << '\n';
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(fs_path(tmp), fs_path(path), ec);
}

void forget_verdict(pack_state& p, const std::string& key) {
  const std::string path = join(p.data_dir, "provider.txt");
  std::lock_guard lock(p.verdict_m);
  auto all = read_verdicts(path);
  if (all.erase(key) == 0) return;
  std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
  out << "mv-ai-provider 1\n";
  for (const auto& [k, e] : all) out << k << '\t' << e.outcome << '\t' << e.open_s << '\t' << e.detail << '\n';
}

std::optional<provider_verdict> read_verdict(pack_state& p, const std::string& key) {
  std::lock_guard lock(p.verdict_m);
  auto all = read_verdicts(join(p.data_dir, "provider.txt"));
  auto it = all.find(key);
  if (it == all.end()) return std::nullopt;
  return it->second;
}

// Median milliseconds of three runs of a four-image batch.
double time_batch(infer::embedder& m, std::vector<float>& out) {
  const std::vector<std::uint8_t> card = test_card(320, 240);
  const infer::rgb_view v{card.data(), 320, 240};
  const infer::rgb_view batch[4] = {v, v, v, v};
  std::vector<double> ms;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!m.embed_images(batch, out)) return -1;
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  return ms[1];
}

// Every file a model needs is on disk (shipped, or fetched and verified):
// the downloader writes a file under its final name only once it matches.
bool files_present(std::initializer_list<std::string> files) {
  std::error_code ec;
  for (const std::string& f : files) {
    if (f.empty() || !std::filesystem::is_regular_file(fs_path(f), ec)) return false;
  }
  return true;
}

bool files_present(const infer::clip_spec& s) { return files_present({s.image_file, s.text_file, s.vocab_file, s.merges_file}); }

clip_meta meta_of(const infer::clip_spec& s) {
  clip_meta m;
  m.name = s.name;
  m.spec_key = s.spec_key();
  m.quality = s.quality;
  m.dim = s.dim;
  m.input_edge = s.norm.size;
  m.dedupe = s.dedupe;
  m.query_margin = s.query_margin;
  m.query_z = s.query_z;
  m.result_z = s.result_z;
  m.result_margin = s.result_margin;
  m.similar_min = s.similar_min;
  m.generic_prompts = s.generic_prompts;
  return m;
}

// Whether ORT finished a cache entry: <hash>/<n>_<shape>_<format>/model is the
// converted package and compiled_model.mlmodelc beside its parts is Core ML's
// compile, moved in whole once it ends. ORT writes model.txt before either and
// takes any existing model folder as converted, so a quit mid-conversion left
// an entry every later start loaded, failed, and recorded as Core ML failing
// the tower here (Auto then runs the small one for good). Mid-compile there
// is no .mlmodelc yet; ORT would compile again, but the package may be cut.
bool coreml_entry_complete(const std::filesystem::path& entry) {
  std::error_code ec;
  bool any = false;
  for (const auto& sub : std::filesystem::directory_iterator(entry, ec)) {
    if (!sub.is_directory(ec)) continue;
    any = true;
    if (!std::filesystem::exists(sub.path() / "model" / "compiled_model.mlmodelc", ec)) return false;
  }
  return any;
}

// Core ML's compiled models (Mac), under data/cache: derived, so Remove drops
// them even when it keeps the index (store.h). ORT keys an entry by the model's
// path, which holds the pack's version: once per start, entries whose model
// no longer exists (an older pack) go, and so do entries a quit cut short
// (before this process opens anything, so none is being written).
std::string coreml_cache(pack_state& p) {
  const std::string dir = join(join(p.data_dir, "cache"), "coreml");
  std::call_once(p.cache_pruned, [&] {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fs_path(dir), ec)) {
      std::ifstream in(e.path() / "model.txt", std::ios::binary);
      std::string model;
      if (in && std::getline(in, model) && std::filesystem::exists(fs_path(model), ec) &&
          coreml_entry_complete(e.path())) {
        continue;
      }
      in.close();
      std::filesystem::remove_all(e.path(), ec);
    }
    // The first Mac builds cached beside the index.
    std::filesystem::remove_all(fs_path(join(p.data_dir, "coreml-cache")), ec);
  });
  return dir;
}

// The provider self-test (docs/design/17): the provider must agree with CPU, and
// under Auto it must also beat CPU, else CPU runs it and the status says why.
// The CPU side comes from the kept reference when there is one.
infer::provider_fault self_test(pack_state& p, const infer::clip_spec& spec, infer::embedder& fast,
                                infer::backend want, std::uint32_t compute,
                                const infer::session_options& cpu) {
  const std::string cache = join(p.data_dir, "selftest.txt");
  const std::string key = p.rt->version() + "|" + std::to_string(static_cast<int>(want)) + "|" +
                          spec.spec_key() + "|" + (p.from_piece ? "piece" : "core");
  std::optional<selftest_ref> ref = read_selftest(cache, key);
  std::vector<float> a, b;
  const double t_fast = time_batch(fast, a);
  double t_slow = -1;
  if (ref) {
    b = std::move(ref->emb);
    t_slow = ref->cpu_ms;
  } else if (t_fast >= 0) {
    if (auto slow = infer::clip_model::open(*p.rt, spec, cpu, nullptr)) {
      t_slow = time_batch(**slow, b);
      if (t_slow > 0 && b.size() >= spec.dim) {
        write_selftest(cache, selftest_ref{key, t_slow, std::vector<float>(b.begin(), b.begin() + spec.dim)});
      }
    }
  }
  if (t_fast < 0) return infer::provider_fault::failed;
  if (a.size() >= spec.dim && b.size() >= spec.dim &&
      infer::dot(std::span<const float>(a.data(), spec.dim), std::span<const float>(b.data(), spec.dim)) < 0.99f) {
    return infer::provider_fault::mismatch;
  }
  if (compute == MV_AI_COMPUTE_AUTO && t_slow > 0 && t_fast > t_slow) return infer::provider_fault::slower;
  return infer::provider_fault::none;
}

// Core ML compiles a tower for this Mac on every open: ~17 s for B/32 and
// ~64 s for L/14 even from its cache (ORT's converter inlines the weights, so
// Core ML re-parses a 1-3.5 GB model each time). Searching and indexing must
// not wait for that: this answers on CPU at once while a background thread
// opens Core ML and self-tests it, then swaps it in. A call in flight keeps
// the model it started on (shared_ptr), and the CPU tower is freed after it.
class upgrading_clip final : public infer::embedder {
 public:
  upgrading_clip(std::shared_ptr<pack_state> state, infer::clip_spec tower, std::unique_ptr<infer::clip_model> now,
                 infer::session_options accelerated_opts, std::uint32_t compute_choice)
      : current_(std::move(now)), dim_(tower.dim), key_(tower.spec_key()) {
    worker_ = std::thread([this, p = std::move(state), spec = std::move(tower), fast = std::move(accelerated_opts),
                           compute = compute_choice] {
      platform::enter_background();
      infer::session_options cpu = fast;
      cpu.on = infer::backend::cpu;
      cpu.cache_dir_utf8.clear();
      struct finished {
        std::atomic<bool>* flag;
        ~finished() { *flag = true; }
      } const done{&done_};
      const std::string key = spec.spec_key();
      {
        // Never two compiles of one tower at once: wait for the other (its
        // cache entry then makes this one the quicker cached open).
        std::unique_lock lock(p->compile_m);
        while (p->compiling.count(key) != 0 && !stop_) {
          p->compile_cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (stop_) return;
        p->compiling.insert(key);
      }
      infer::provider_fault why = infer::provider_fault::none;
      const auto t0 = std::chrono::steady_clock::now();
      auto opened = infer::clip_model::open(*p->rt, spec, fast, &why);
      const double open_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::string detail = opened ? std::string() : infer::session::last_error();
      {
        std::lock_guard lock(p->compile_m);
        p->compiling.erase(key);
      }
      p->compile_cv.notify_all();
      if (stop_) return;
      if (opened) {
        why = self_test(*p, spec, **opened, fast.on, compute, cpu);
        if (why == infer::provider_fault::failed) detail = infer::session::last_error();
      }
      if (!opened && why == infer::provider_fault::none) why = infer::provider_fault::failed;
      write_verdict(*p, verdict_key(*p->rt, fast.on, key),
                    provider_verdict{why == infer::provider_fault::none ? "ok" : infer::fault_name(why), open_s,
                                     why == infer::provider_fault::none ? std::string() : detail});
      std::lock_guard lock(m_);
      if (opened && why == infer::provider_fault::none) {
        current_ = std::shared_ptr<infer::embedder>(std::move(*opened));
      } else {
        fault_ = why;
        detail_ = std::move(detail);
      }
    });
  }
  ~upgrading_clip() override {
    stop_ = true;
    // An open in progress cannot be cancelled; the pack's code must stay
    // mapped until it returns (unload runs off the main thread). The engine
    // lets the last reference go only once settling() is false (retire).
    if (worker_.joinable()) worker_.join();
  }
  bool settling() const noexcept override { return !done_.load(); }
  std::uint32_t dim() const noexcept override { return dim_; }
  const std::string& spec_key() const noexcept override { return key_; }
  infer::backend on() const noexcept override { return model()->on(); }
  infer::provider_fault fault() const noexcept override {
    std::lock_guard lock(m_);
    return fault_;
  }
  std::string fault_detail() const override {
    std::lock_guard lock(m_);
    return detail_;
  }
  expected embed_images(std::span<const infer::rgb_view> images, std::vector<float>& out) override {
    return model()->embed_images(images, out);
  }
  result<std::vector<float>> embed_text(std::string_view utf8) override { return model()->embed_text(utf8); }
  result<std::vector<float>> embed_text_mean(std::span<const std::string> texts) override {
    return model()->embed_text_mean(texts);
  }

 private:
  std::shared_ptr<infer::embedder> model() const {
    std::lock_guard lock(m_);
    return current_;
  }
  mutable std::mutex m_;
  std::shared_ptr<infer::embedder> current_;
  infer::provider_fault fault_ = infer::provider_fault::none;
  std::string detail_;
  std::uint32_t dim_;
  std::string key_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> done_{false};
  std::thread worker_;
};

class ort_sound final : public sound_model {
 public:
  explicit ort_sound(std::unique_ptr<infer::clap_model> m) : m_(std::move(m)) {}
  expected embed_audio(std::span<const std::span<const float>> windows, std::vector<float>& out) override {
    return m_->embed_audio(windows, out);
  }
  result<std::vector<float>> embed_text(std::string_view utf8) override { return m_->embed_text(utf8); }

 private:
  std::unique_ptr<infer::clap_model> m_;
};

class ort_docs final : public passage_model {
 public:
  explicit ort_docs(std::unique_ptr<infer::text_model> m) : m_(std::move(m)) {}
  expected embed_passages(std::span<const std::string> texts, std::vector<float>& out) override {
    return m_->embed_passages(texts, out);
  }
  result<std::vector<float>> embed_query(std::string_view utf8) override { return m_->embed_query(utf8); }

 private:
  std::unique_ptr<infer::text_model> m_;
};

class ort_speech final : public speech_model {
 public:
  explicit ort_speech(std::unique_ptr<infer::whisper_model> m) : m_(std::move(m)) {}
  result<infer::speech_window> transcribe(std::span<const float> pcm, std::int64_t start_ms) override {
    return m_->transcribe(pcm, start_ms);
  }

 private:
  std::unique_ptr<infer::whisper_model> m_;
};

class ort_faces final : public face_analyzer {
 public:
  explicit ort_faces(std::unique_ptr<infer::face_models> m) : m_(std::move(m)) {}
  result<std::vector<face_in>> analyze(const rgb_frame& img) override {
    const infer::rgb_view view{img.rgb.data(), img.width, img.height};
    MV_TRY(auto boxes, m_->detect(view));
    std::vector<face_in> out;
    // The clearest dozen: a crowd shot does not need every face clustered.
    if (boxes.size() > 12) boxes.resize(12);
    std::vector<float> crop;
    for (const infer::face_box& b : boxes) {
      auto e = m_->embed(view, b, &crop);
      if (!e) continue;
      face_in f;
      // How far the refinement may trust this face (docs/design/17 "People refinement").
      f.quality = face_quality(b.score, std::min(b.w, b.h), crop_sharpness(crop, 112),
                               landmark_frontalness(std::span<const float, 10>(b.landmarks)));
      f.tta = true;
      f.x = std::clamp(b.x / static_cast<float>(img.width), 0.0f, 1.0f);
      f.y = std::clamp(b.y / static_cast<float>(img.height), 0.0f, 1.0f);
      f.w = std::clamp(b.w / static_cast<float>(img.width), 0.0f, 1.0f - f.x);
      f.h = std::clamp(b.h / static_cast<float>(img.height), 0.0f, 1.0f - f.y);
      f.score = b.score;
      f.emb = std::move(*e);
      out.push_back(std::move(f));
    }
    return out;
  }
  const std::string& spec_key() const noexcept override { return m_->spec().spec_key; }
  float same_person() const noexcept override { return m_->spec().same_person; }
  std::uint32_t dim() const noexcept override { return m_->spec().dim; }
  std::string name() const override { return m_->spec().name; }
  face_tuning tuning() const noexcept override {
    const infer::face_spec& s = m_->spec();
    face_tuning t;
    t.same_person = s.same_person;
    t.keep = s.keep;
    t.keep_weak = s.keep_weak;
    t.margin = s.margin;
    t.ambiguous = s.ambiguous;
    t.merge_at = s.merge_at;
    return t;
  }

 private:
  std::unique_ptr<infer::face_models> m_;
};

}  // namespace

engine_deps pack_deps(const host& h, const std::string& self_dir, const std::string& data_dir) {
  auto p = std::make_shared<pack_state>();
  p->h = &h;
  p->core_dir = self_dir;
  p->data_dir = data_dir;
  engine_deps d;
  d.prepare = [p] { p->ensure(); };
  d.qualities = [p] {
    if (!p->ready) return std::vector<std::uint32_t>{};
    return p->qualities();
  };
  d.model_name = [p](std::uint32_t q) {
    if (!p->ready) return std::string();
    auto t = p->tower(q, false);
    return t ? t->name : std::string();
  };
  d.clip_spec = [p](std::uint32_t q) {
    if (!p->ready) return std::string();
    auto t = p->tower(q, false);
    return t ? t->spec_key() : std::string();
  };
  d.backend_available = [p](infer::backend b) {
    if (!p->ready) return false;
    return p->rt && p->rt->has_provider(b);
  };
  d.open_clip = [p](std::uint32_t quality, std::uint32_t compute) -> result<loaded_clip> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    const auto tower = p->tower(quality);
    if (!tower) return err(status::unsupported_format);
    const infer::clip_spec& spec = *tower;
    // Still downloading (#482 T3): not_found, which the engine reports as
    // waiting for the files rather than as a pack without models.
    if (!files_present(spec)) return err(status::not_found);
    infer::session_options cpu;
    cpu.on = infer::backend::cpu;
    cpu.threads = 2;
    loaded_clip out;
    out.meta = meta_of(spec);

    infer::backend want = infer::backend::cpu;
    switch (compute) {
      case MV_AI_COMPUTE_AUTO: want = accelerated(*p->rt); break;
      case MV_AI_COMPUTE_CUDA: want = infer::backend::cuda; break;
      case MV_AI_COMPUTE_OPENVINO: want = infer::backend::openvino; break;
      case MV_AI_COMPUTE_COREML: want = infer::backend::coreml; break;
      default: want = infer::backend::cpu; break;
    }
    if (want != infer::backend::cpu) {
      infer::session_options acc = cpu;
      acc.on = want;
      acc.cache_dir_utf8 = coreml_cache(*p);
      if (want == infer::backend::coreml) {
        // Answer on CPU now; Core ML follows when compiled (upgrading_clip).
        // Auto's quality choice goes by the provider it is headed for. The
        // same tower on the same choice, still alive: that one, not a
        // second CPU session and a second compile.
        const std::string key = spec.spec_key() + "|" + std::to_string(compute);
        {
          std::lock_guard lock(p->compile_m);
          auto found = p->upgrading.find(key);
          if (found != p->upgrading.end()) {
            if (auto alive = found->second.lock()) {
              out.model = std::move(alive);
              out.on = want;
              return out;
            }
          }
        }
        auto now = infer::clip_model::open(*p->rt, spec, cpu, nullptr);
        if (!now) return err(now.error());
        out.model = std::make_shared<upgrading_clip>(p, spec, std::move(*now), acc, compute);
        out.on = want;
        std::lock_guard lock(p->compile_m);
        for (auto e = p->upgrading.begin(); e != p->upgrading.end();) {
          e = e->second.expired() ? p->upgrading.erase(e) : std::next(e);
        }
        p->upgrading[key] = out.model;
        return out;
      }
      infer::provider_fault fault = infer::provider_fault::none;
      auto fast = infer::clip_model::open(*p->rt, spec, acc, &fault);
      std::string detail = fast ? std::string() : infer::session::last_error();
      if (fast) {
        fault = self_test(*p, spec, **fast, want, compute, cpu);
        if (fault == infer::provider_fault::none) {
          out.model = std::shared_ptr<infer::embedder>(std::move(*fast));
          out.on = want;
          return out;
        }
        if (fault == infer::provider_fault::failed) detail = infer::session::last_error();
      }
      out.fault = fault;
      out.fault_detail = std::move(detail);
    }
    auto m = infer::clip_model::open(*p->rt, spec, cpu, nullptr);
    if (!m) return err(m.error());
    out.model = std::shared_ptr<infer::embedder>(std::move(*m));
    out.on = infer::backend::cpu;
    return out;
  };
  // Audio (2026-09-27): the ai-audio piece's CLAP and Whisper, on the same
  // runtime and compute choice as the picture towers (an accelerated provider
  // when it opens, CPU underneath).
  const auto audio_options = [p](std::uint32_t compute, infer::session_options& o) {
    o.threads = 2;
    o.cache_dir_utf8 = coreml_cache(*p);
    if (compute != MV_AI_COMPUTE_CPU_ONLY && p->rt) o.on = accelerated(*p->rt);
    // Never Core ML (measured on an M5, ORT 1.30): CLAP's dynamic shapes fail
    // to compile, and Whisper's abort the process inside MPSGraph ("original
    // module failed verification"), which no fallback can catch. The decoder's
    // growing KV cache cannot be made static.
    if (o.on == infer::backend::coreml) o.on = infer::backend::cpu;
  };
  d.open_sound = [p, audio_options](std::uint32_t compute) -> result<loaded_sound> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-audio"));
    const std::string sounds = p->profile().sounds;
    if (sounds.empty()) return err(status::unsupported_format);  // a tier without sound search
    MV_TRY(infer::clap_spec spec, infer::read_clap_spec(join(dir, sounds), join(p->fetched("ai-audio"), sounds)));
    if (!files_present({spec.audio_file, spec.text_file, spec.vocab_file})) return err(status::not_found);
    infer::session_options o;
    audio_options(compute, o);
    auto m = infer::clap_model::open(*p->rt, spec, o, nullptr);
    if (!m && o.on != infer::backend::cpu) {
      o.on = infer::backend::cpu;
      m = infer::clap_model::open(*p->rt, spec, o, nullptr);
    }
    if (!m) return err(m.error());
    loaded_sound s;
    s.model = std::make_shared<ort_sound>(std::move(*m));
    s.name = spec.name;
    s.spec_key = spec.spec_key();
    s.dim = spec.dim;
    s.window_ms = spec.window_ms;
    s.hop_ms = spec.hop_ms;
    s.dedupe = spec.dedupe;
    s.query_margin = spec.query_margin;
    s.result_margin = spec.result_margin;
    s.generic_prompts = spec.generic_prompts;
    return s;
  };
  d.open_speech = [p, audio_options](std::uint32_t quality, std::uint32_t compute) -> result<loaded_speech> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-audio"));
    auto spec = p->speech_spec(dir, quality);
    if (!spec) return err(spec.error());
    if (!files_present({spec->encoder_file, spec->decoder_file, spec->vocab_file})) return err(status::not_found);
    infer::session_options o;
    audio_options(compute, o);
    auto m = infer::whisper_model::open(*p->rt, *spec, o, nullptr);
    if (!m && o.on != infer::backend::cpu) {
      o.on = infer::backend::cpu;
      m = infer::whisper_model::open(*p->rt, *spec, o, nullptr);
    }
    if (!m) return err(m.error());
    loaded_speech s;
    s.model = std::make_shared<ort_speech>(std::move(*m));
    s.name = spec->name;
    s.spec_key = spec->spec_key();
    return s;
  };
  d.open_faces = [p](std::uint32_t compute) -> result<std::unique_ptr<face_analyzer>> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-faces"));
    const std::string faces = p->profile().faces;
    if (faces.empty()) return err(status::unsupported_format);
    MV_TRY(infer::face_spec spec, infer::read_face_spec(join(dir, faces), join(p->fetched("ai-faces"), faces)));
    if (!files_present({spec.detector_file, spec.embedder_file})) return err(status::not_found);
    infer::session_options cpu;
    cpu.threads = 1;
    // The embedder on the towers' compute choice (the detector stays on CPU:
    // it is small, and its shape follows the image).
    infer::session_options acc = cpu;
    switch (compute) {
      case MV_AI_COMPUTE_AUTO: acc.on = accelerated(*p->rt); break;
      case MV_AI_COMPUTE_CUDA: acc.on = infer::backend::cuda; break;
      case MV_AI_COMPUTE_OPENVINO: acc.on = infer::backend::openvino; break;
      case MV_AI_COMPUTE_COREML: acc.on = infer::backend::coreml; break;
      default: acc.on = infer::backend::cpu; break;
    }
    if (!p->rt->has_provider(acc.on)) acc.on = infer::backend::cpu;
    acc.cache_dir_utf8 = coreml_cache(*p);
    MV_TRY(auto models, infer::face_models::open(*p->rt, spec, cpu, &acc));
    return std::unique_ptr<face_analyzer>(new ort_faces(std::move(models)));
  };
  d.restart_needed = [p] {
    if (!p->ready) return false;
    // Cheap on purpose (Settings asks often): is the piece's folder there?
    // <addons>/ai/<version> is the Core pack; the piece sits at <addons>/ai-cuda
    // (Windows only: the Mac has no vendor piece).
    const std::string root = join(join(p->core_dir, ".."), "..");
    auto st = p->h->stat(join(root, "ai-cuda"));
    const bool piece = st && st->is_directory;
    return p->rt && piece != p->from_piece;
  };
  // Core ML compiles a tower once per machine and keeps it under data/cache
  // (ORT writes <cache>/<hash>/model.txt naming the model it compiled). No
  // entry for this tower: this open is the slow first one.
  d.first_compile = [p](std::uint32_t quality, std::uint32_t compute) {
    if (!p->ready || !p->rt) return false;
    if (compute == MV_AI_COMPUTE_CPU_ONLY) return false;
    const infer::backend want = compute == MV_AI_COMPUTE_AUTO ? accelerated(*p->rt)
                                : compute == MV_AI_COMPUTE_COREML ? infer::backend::coreml
                                                                  : infer::backend::cpu;
    if (want != infer::backend::coreml || !p->rt->has_provider(want)) return false;
    const auto tower = p->tower(quality);
    if (!tower) return false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fs_path(coreml_cache(*p)), ec)) {
      std::ifstream in(e.path() / "model.txt", std::ios::binary);
      std::string model;
      if (in && std::getline(in, model) && model == tower->image_file && coreml_entry_complete(e.path())) {
        return false;
      }
    }
    return true;
  };
  d.accelerated_failure = [p](std::uint32_t quality) -> std::optional<std::string> {
    if (!p->ready || !p->rt) return std::nullopt;
    const infer::backend on = accelerated(*p->rt);
    const auto tower = p->tower(quality, false);
    if (on == infer::backend::cpu || !tower) return std::nullopt;
    auto v = read_verdict(*p, verdict_key(*p->rt, on, tower->spec_key()));
    if (!v || v->outcome == "ok") return std::nullopt;
    return v->detail;
  };
  d.forget_accelerated_failure = [p](std::uint32_t quality) {
    if (!p->ready || !p->rt) return;
    if (const auto tower = p->tower(quality, false)) {
      forget_verdict(*p, verdict_key(*p->rt, accelerated(*p->rt), tower->spec_key()));
    }
  };
  // A reader (engine_options::read_only, docs/design/23): text towers only, on CPU.
  d.clip_spec_key = [p](std::uint32_t quality) {
    p->ensure();
    const auto tower = p->tower(quality);
    return tower ? tower->spec_key() : std::string();
  };
  d.open_clip_text = [p](std::uint32_t quality) -> result<loaded_clip> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    const auto tower = p->tower(quality);
    if (!tower) return err(status::unsupported_format);
    infer::session_options cpu;
    cpu.on = infer::backend::cpu;
    cpu.threads = 2;
    MV_TRY(auto m, infer::clip_model::open_text_only(*p->rt, *tower, cpu));
    loaded_clip out;
    out.meta = meta_of(*tower);
    out.model = std::shared_ptr<infer::embedder>(std::move(m));
    out.on = infer::backend::cpu;
    return out;
  };
  d.open_sound_text = [p]() -> result<loaded_sound> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-audio"));
    const std::string sounds = p->profile().sounds;
    if (sounds.empty()) return err(status::unsupported_format);  // a tier without sound search
    MV_TRY(infer::clap_spec spec, infer::read_clap_spec(join(dir, sounds), join(p->fetched("ai-audio"), sounds)));
    if (!files_present({spec.audio_file, spec.text_file, spec.vocab_file})) return err(status::not_found);
    infer::session_options cpu;
    cpu.on = infer::backend::cpu;
    cpu.threads = 2;
    MV_TRY(auto m, infer::clap_model::open_text_only(*p->rt, spec, cpu));
    loaded_sound s;
    s.model = std::make_shared<ort_sound>(std::move(m));
    s.name = spec.name;
    s.spec_key = spec.spec_key();
    s.dim = spec.dim;
    s.window_ms = spec.window_ms;
    s.hop_ms = spec.hop_ms;
    s.dedupe = spec.dedupe;
    s.query_margin = spec.query_margin;
    s.result_margin = spec.result_margin;
    s.generic_prompts = spec.generic_prompts;
    return s;
  };
  // #354 D3: the ai-docs piece's text model, on CPU (2 threads, as audio): a
  // small model whose vectors must not depend on the compute choice.
  d.open_docs = [p]() -> result<loaded_docs> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-docs"));
    const std::string documents = p->profile().documents;
    if (documents.empty()) return err(status::unsupported_format);
    MV_TRY(infer::text_spec spec, infer::read_text_spec(join(dir, documents), join(p->fetched("ai-docs"), documents)));
    if (!files_present({spec.model_file, spec.vocab_file})) return err(status::not_found);
    infer::session_options cpu;
    cpu.on = infer::backend::cpu;
    cpu.threads = 2;
    MV_TRY(auto m, infer::text_model::open(*p->rt, spec, cpu));
    loaded_docs out;
    out.model = std::make_shared<ort_docs>(std::move(m));
    out.name = spec.name;
    out.spec_key = spec.spec_key();
    out.dim = spec.dim;
    out.query_margin = spec.query_margin;
    out.result_margin = spec.result_margin;
    out.rank_offset = spec.rank_offset;
    out.generic_prompts = spec.generic_prompts;
    return out;
  };
  d.speech_spec_key = [p](std::uint32_t quality) {
    auto dir = p->h->piece_dir("ai-audio");
    if (!dir) return std::string();
    auto spec = p->speech_spec(*dir, quality);
    return spec ? spec->spec_key() : std::string();
  };
  // ---- model tiers and downloads (#482) ----
  d.set_tier = [p](const std::string& tier) { p->set_tier(tier); };
  d.tiers_json = [p]() -> std::string {
    // What the control thread read (refresh_locked): Settings asks from the UI thread.
    std::lock_guard lock(p->tier_m);
    return p->tiers_text.empty() ? std::string("{}") : p->tiers_text;
  };
  d.fetch_models = [p](const std::atomic<bool>& stop, const std::function<void(const fetch_progress&)>& progress,
                       const std::function<void()>& pictures) -> expected {
    auto [first, rest] = p->downloads();
    const std::string root = join(p->data_dir, "fetched");
    auto net = make_http_transport();
    if (!net) {
      // No transport here: the pack must carry its models.
      return first.empty() && rest.empty() ? expected{} : expected{err(status::unsupported_format)};
    }
    fetcher f(root, *net);
    std::uint64_t all = 0, first_bytes = 0;
    for (const remote_file& r : first) first_bytes += r.size;
    all = first_bytes;
    for (const remote_file& r : rest) all += r.size;
    const auto files_all = static_cast<std::uint32_t>(first.size() + rest.size());
    const auto relay = [&](std::uint64_t offset, std::uint32_t files_before) {
      return [&progress, offset, files_before, all, files_all](const fetch_progress& q) {
        fetch_progress out = q;
        out.done_bytes = offset + q.done_bytes;
        out.total_bytes = all;
        out.files_done = files_before + q.files_done;
        out.files_total = files_all;
        if (q.state == fetch_state::done && out.files_done < files_all) out.state = fetch_state::downloading;
        if (progress) progress(out);
      };
    };
    MV_TRY_VOID(f.run(first, stop, relay(0, 0)));
    if (pictures) pictures();
    return f.run(rest, stop, relay(first_bytes, static_cast<std::uint32_t>(first.size())));
  };
  d.models_present = [p] {
    auto [first, rest] = p->downloads();
    const std::string root = join(p->data_dir, "fetched");
    for (const auto* list : {&first, &rest}) {
      for (const remote_file& r : *list) {
        if (!fetch_ready(root, r)) return false;
      }
    }
    return true;
  };
  d.prune_models = [p] {
    auto [first, rest] = p->downloads();
    first.insert(first.end(), rest.begin(), rest.end());
    prune_fetched(join(p->data_dir, "fetched"), first);
  };
  // A tower of any tier, by its index key, on CPU: after a change of tier the
  // vectors in the index came from the old one, which answers until the new
  // one covers the folder (load_models), so it must still open.
  d.open_clip_spec = [p](const std::string& spec_key) -> result<loaded_clip> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    tier_set all;
    {
      std::lock_guard lock(p->tier_m);
      p->refresh_locked();
      all = p->tiers;
    }
    for (const tier_profile& t : all.tiers) {
      for (const auto& [quality, folder] : {std::pair{MV_AI_QUALITY_FAST, &t.picture_fast},
                                            std::pair{MV_AI_QUALITY_HIGH, &t.picture_high}}) {
        if (folder->empty()) continue;
        auto spec = p->picture_spec(*folder);
        if (!spec || spec->spec_key() != spec_key || !files_present(*spec)) continue;
        spec->quality = quality;
        infer::session_options cpu;
        cpu.on = infer::backend::cpu;
        cpu.threads = 2;
        MV_TRY(auto m, infer::clip_model::open(*p->rt, *spec, cpu, nullptr));
        loaded_clip out;
        out.meta = meta_of(*spec);
        out.model = std::shared_ptr<infer::embedder>(std::move(m));
        out.on = infer::backend::cpu;
        return out;
      }
    }
    return err(status::not_found);
  };
  d.sounds_off_by_default = [p] { return p->profile().sounds_off_by_default; };
  d.clip_gaps = [p]() -> std::pair<std::uint32_t, std::uint32_t> {
    const tier_profile t = p->profile();
    return {t.min_gap_ms, t.max_gap_ms};
  };
  d.runtime_version = [p] {
    if (!p->ready) return std::string();
    return p->rt ? p->rt->version() : std::string();
  };
  return d;
}

}  // namespace mv::ai
