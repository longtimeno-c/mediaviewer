// SPDX-License-Identifier: GPL-2.0-or-later
#include "addons/ai/pack.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>

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
  std::unique_ptr<infer::runtime> rt;
  bool from_piece = false;  // ORT came from the ai-cuda piece
  std::map<std::uint32_t, infer::clip_spec> towers;  // by mv_ai_quality

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
  m.result_margin = s.result_margin;
  m.similar_min = s.similar_min;
  m.generic_prompts = s.generic_prompts;
  return m;
}

class ort_faces final : public face_analyzer {
 public:
  explicit ort_faces(std::unique_ptr<infer::face_models> m) : m_(std::move(m)) {}
  result<std::vector<face_in>> analyze(const rgb_frame& img) override {
    const infer::rgb_view view{img.rgb.data(), img.width, img.height};
    MV_TRY(auto boxes, m_->detect(view));
    std::vector<face_in> out;
    // The clearest dozen: a crowd shot does not need every face clustered.
    if (boxes.size() > 12) boxes.resize(12);
    for (const infer::face_box& b : boxes) {
      auto e = m_->embed(view, b);
      if (!e) continue;
      face_in f;
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
  d.qualities = [p] {
    p->ensure();
    std::vector<std::uint32_t> q;
    for (const auto& [k, v] : p->towers) q.push_back(k);
    return q;
  };
  d.model_name = [p](std::uint32_t q) {
    p->ensure();
    auto it = p->towers.find(q);
    return it == p->towers.end() ? std::string() : it->second.name;
  };
  d.backend_available = [p](infer::backend b) {
    p->ensure();
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
      acc.cache_dir_utf8 = join(p->data_dir, "coreml-cache");
      infer::provider_fault fault = infer::provider_fault::none;
      auto fast = infer::clip_model::open(*p->rt, spec, acc, &fault);
      if (fast) {
        // The self-test (plan/17): the provider must agree with CPU, and under
        // Auto it must also beat CPU, else CPU runs it and the status says why.
        auto slow = infer::clip_model::open(*p->rt, spec, cpu, nullptr);
        std::vector<float> a, b;
        const double t_fast = time_batch(**fast, a);
        double t_slow = -1;
        if (slow) t_slow = time_batch(**slow, b);
        if (t_fast < 0) {
          fault = infer::provider_fault::failed;
        } else if (slow && !a.empty() && a.size() == b.size() &&
                   infer::dot(std::span<const float>(a.data(), spec.dim),
                              std::span<const float>(b.data(), spec.dim)) < 0.99f) {
          fault = infer::provider_fault::mismatch;
        } else if (compute == MV_AI_COMPUTE_AUTO && t_slow > 0 && t_fast > t_slow) {
          fault = infer::provider_fault::slower;
        } else {
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
    p->ensure();
    // Cheap on purpose (Settings asks often): is the piece's folder there?
    // <addons>/ai/<version> is the Core pack; the piece sits at <addons>/ai-cuda
    // (Windows only: the Mac has no vendor piece).
    const std::string root = join(join(p->core_dir, ".."), "..");
    auto st = p->h->stat(join(root, "ai-cuda"));
    const bool piece = st && st->is_directory;
    return p->rt && piece != p->from_piece;
  };
  d.runtime_version = [p] {
    p->ensure();
    return p->rt ? p->rt->version() : std::string();
  };
  return d;
}

}  // namespace mv::ai
