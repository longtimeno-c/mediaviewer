// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "addons/ai/platform.h"
#include "infer/audio_models.h"
#include "infer/models.h"
#include "infer/ort.h"

namespace mv::ai {
namespace {

std::string join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
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
  std::map<std::uint32_t, infer::clip_spec> towers;  // by mv_ai_quality

  // Core ML compiles (upgrading_clip). One tower compiles once at a time: a
  // first compile of L/14 writes gigabytes for minutes on a MacBook Air, and
  // a second beside it (a reload while the first ran) doubled that and the
  // memory. An open of a tower on a compute choice that is still alive is
  // handed the same instance rather than compiling it again.
  std::mutex compile_m;
  std::condition_variable compile_cv;
  std::set<std::string> compiling;                                  // spec keys
  std::map<std::string, std::weak_ptr<infer::embedder>> upgrading;  // spec key | compute

  void ensure() {
    std::call_once(once, [this] {
      // A vendor piece carries its own ORT build (CUDA builds run CPU too);
      // with none installed, the Core pack's CPU build (plan/17 "Runtime").
      std::string ort_dir = core_dir;
      if (auto cuda = h->piece_dir("ai-cuda")) ort_dir = *cuda;
      auto loaded = infer::runtime::load(ort_dir);
      from_piece = loaded && ort_dir != core_dir;
      if (!loaded && ort_dir != core_dir) loaded = infer::runtime::load(core_dir);
      if (!loaded) return;
      rt = std::move(*loaded);
      for (const char* folder : {"clip-b32", "clip-l14"}) {
        auto spec = infer::read_clip_spec(join(join(core_dir, "models"), folder));
        if (spec) towers[spec->quality] = std::move(*spec);
      }
    });
    ready = true;
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

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// The CPU half of the provider self-test (plan/17), kept so a later start
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

// Core ML's compiled models (Mac), under data/cache: derived, so Remove drops
// them even when it keeps the index (store.h). ORT keys an entry by the model's
// path, which holds the pack's version: once per start, entries whose model
// no longer exists (an older pack) go.
std::string coreml_cache(pack_state& p) {
  const std::string dir = join(join(p.data_dir, "cache"), "coreml");
  std::call_once(p.cache_pruned, [&] {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fs_path(dir), ec)) {
      std::ifstream in(e.path() / "model.txt", std::ios::binary);
      std::string model;
      if (in && std::getline(in, model) && std::filesystem::exists(fs_path(model), ec)) continue;
      std::filesystem::remove_all(e.path(), ec);
    }
    // The first Mac builds cached beside the index.
    std::filesystem::remove_all(fs_path(join(p.data_dir, "coreml-cache")), ec);
  });
  return dir;
}

// The provider self-test (plan/17): the provider must agree with CPU, and
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
      auto opened = infer::clip_model::open(*p->rt, spec, fast, &why);
      {
        std::lock_guard lock(p->compile_m);
        p->compiling.erase(key);
      }
      p->compile_cv.notify_all();
      if (stop_) return;
      if (opened) why = self_test(*p, spec, **opened, fast.on, compute, cpu);
      std::lock_guard lock(m_);
      if (opened && why == infer::provider_fault::none) {
        current_ = std::shared_ptr<infer::embedder>(std::move(*opened));
      } else {
        fault_ = why == infer::provider_fault::none ? infer::provider_fault::failed : why;
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
      // How far the refinement may trust this face (plan/17 "People refinement").
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
  std::uint32_t dim() const noexcept override { return 128; }

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
    std::vector<std::uint32_t> q;
    for (const auto& [k, v] : p->towers) q.push_back(k);
    return q;
  };
  d.model_name = [p](std::uint32_t q) {
    if (!p->ready) return std::string();
    auto it = p->towers.find(q);
    return it == p->towers.end() ? std::string() : it->second.name;
  };
  d.clip_spec = [p](std::uint32_t q) {
    if (!p->ready) return std::string();
    auto it = p->towers.find(q);
    return it == p->towers.end() ? std::string() : it->second.spec_key();
  };
  d.backend_available = [p](infer::backend b) {
    if (!p->ready) return false;
    return p->rt && p->rt->has_provider(b);
  };
  d.open_clip = [p](std::uint32_t quality, std::uint32_t compute) -> result<loaded_clip> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    auto it = p->towers.find(quality);
    if (it == p->towers.end()) return err(status::unsupported_format);
    const infer::clip_spec& spec = it->second;
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
      if (fast) {
        fault = self_test(*p, spec, **fast, want, compute, cpu);
        if (fault == infer::provider_fault::none) {
          out.model = std::shared_ptr<infer::embedder>(std::move(*fast));
          out.on = want;
          return out;
        }
      }
      out.fault = fault;
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
    MV_TRY(infer::clap_spec spec, infer::read_clap_spec(join(join(dir, "models"), "clap-general")));
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
    const char* folder = quality >= MV_AI_QUALITY_HIGH ? "whisper-small" : "whisper-base";
    auto spec = infer::read_whisper_spec(join(join(dir, "models"), folder));
    if (!spec) spec = infer::read_whisper_spec(join(join(dir, "models"), "whisper-base"));
    if (!spec) return err(spec.error());
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
  d.open_faces = [p]() -> result<std::unique_ptr<face_analyzer>> {
    p->ensure();
    if (!p->rt) return err(status::unsupported_format);
    MV_TRY(std::string dir, p->h->piece_dir("ai-faces"));
    MV_TRY(infer::face_spec spec, infer::read_face_spec(join(join(dir, "models"), "faces")));
    infer::session_options cpu;
    cpu.threads = 1;
    MV_TRY(auto models, infer::face_models::open(*p->rt, spec, cpu));
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
    auto it = p->towers.find(quality);
    if (it == p->towers.end()) return false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fs_path(coreml_cache(*p)), ec)) {
      std::ifstream in(e.path() / "model.txt", std::ios::binary);
      std::string model;
      if (in && std::getline(in, model) && model == it->second.image_file) return false;
    }
    return true;
  };
  d.runtime_version = [p] {
    if (!p->ready) return std::string();
    return p->rt ? p->rt->version() : std::string();
  };
  return d;
}

}  // namespace mv::ai
