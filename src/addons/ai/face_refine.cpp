// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/face_refine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace mv::ai {
namespace {

constexpr float kNoEvidence = -1.0f;

// Eight independent sums so the compiler vectorises it without fast-math
// (one accumulator is a serial dependency it may not reorder). The hot loop:
// every face against every person's core mean.
float dot(const float* a, const float* b, std::uint32_t n) {
  float s[8] = {};
  std::uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    for (std::uint32_t k = 0; k < 8; ++k) s[k] += a[i + k] * b[i + k];
  }
  for (; i < n; ++i) s[0] += a[i] * b[i];
  return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

bool rejected_from(const refine_face& f, std::int64_t person) {
  return std::binary_search(f.rejected.begin(), f.rejected.end(), person);
}

// Mean of the top k cosines of `v` to the exemplars, skipping the face itself.
float support(const float* v, const person_proto& p, std::uint32_t dim, std::int64_t self,
              std::uint32_t top_k) {
  float best[16];
  const std::uint32_t k_max = std::min<std::uint32_t>(std::max<std::uint32_t>(top_k, 1), 16);
  std::uint32_t have = 0;
  const std::size_t n = p.exemplar_ids.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (p.exemplar_ids[i] == self) continue;
    const float s = dot(v, p.exemplars.data() + i * dim, dim);
    if (have < k_max) {
      best[have++] = s;
      std::push_heap(best, best + have, std::greater<>());
    } else if (s > best[0]) {
      std::pop_heap(best, best + have, std::greater<>());
      best[have - 1] = s;
      std::push_heap(best, best + have, std::greater<>());
    }
  }
  if (have == 0) return kNoEvidence;
  float sum = 0;
  for (std::uint32_t i = 0; i < have; ++i) sum += best[i];
  return sum / static_cast<float>(have);
}

// Builds a person's prototype from its members (indices into in.faces).
person_proto build_proto(const refine_input& in, const refine_params& p, std::int64_t person,
                         std::vector<std::size_t> members, bool named) {
  const std::uint32_t dim = in.dim;
  const auto row = [&](std::size_t i) { return in.emb.data() + std::size_t{in.faces[i].row} * dim; };
  person_proto out;
  out.person = person;
  out.named = named;
  // Pinned first, then by quality, then id: deterministic.
  std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
    const refine_face& fa = in.faces[a];
    const refine_face& fb = in.faces[b];
    if (fa.pinned != fb.pinned) return fa.pinned;
    if (fa.quality != fb.quality) return fa.quality > fb.quality;
    return fa.id < fb.id;
  });
  std::vector<std::size_t> refs;
  for (std::size_t i : members) {
    if (in.faces[i].pinned || in.faces[i].quality >= p.weak_quality) refs.push_back(i);
  }
  if (refs.empty()) refs = members;  // all weak: the best of them still describe the person
  if (refs.size() > p.max_refs) refs.resize(p.max_refs);
  const std::size_t n = refs.size();
  if (n == 0) return out;

  // Medoid: the ref most like all the others.
  std::vector<float> sim(n * n, 0.0f);
  for (std::size_t a = 0; a < n; ++a) {
    sim[a * n + a] = 1.0f;
    for (std::size_t b = a + 1; b < n; ++b) {
      const float s = dot(row(refs[a]), row(refs[b]), dim);
      sim[a * n + b] = s;
      sim[b * n + a] = s;
    }
  }
  std::size_t medoid = 0;
  float medoid_sum = -1e30f;
  for (std::size_t a = 0; a < n; ++a) {
    float s = 0;
    for (std::size_t b = 0; b < n; ++b) s += sim[a * n + b];
    if (s > medoid_sum) {
      medoid_sum = s;
      medoid = a;
    }
  }
  std::vector<std::size_t> anchors;
  for (std::size_t a = 0; a < n; ++a) {
    if (in.faces[refs[a]].pinned) anchors.push_back(a);
  }
  out.pinned = !anchors.empty();
  if (anchors.empty()) {
    anchors.push_back(medoid);
  } else if (!in.faces[refs[medoid]].pinned) {
    // The user's faces define a person they touched; the medoid joins them
    // only when it agrees (a glued-on impostor half can hold the majority).
    const bool agrees = std::any_of(anchors.begin(), anchors.end(),
                                    [&](std::size_t a) { return sim[medoid * n + a] >= p.core; });
    if (agrees) anchors.push_back(medoid);
  }
  std::vector<std::pair<float, std::size_t>> core;  // (closeness to an anchor, ref)
  for (std::size_t a = 0; a < n; ++a) {
    float near = -1;
    bool is_anchor = false;
    for (std::size_t k : anchors) {
      near = std::max(near, sim[a * n + k]);
      is_anchor = is_anchor || k == a;
    }
    if (is_anchor) {
      core.emplace_back(2.0f + (in.faces[refs[a]].pinned ? 1.0f : 0.0f), a);
    } else if (near >= p.core) {
      core.emplace_back(near * (0.5f + 0.5f * in.faces[refs[a]].quality), a);
    }
  }
  std::stable_sort(core.begin(), core.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  out.core_mean.assign(dim, 0.0f);
  for (const auto& [_, a] : core) {
    const float* v = row(refs[a]);
    for (std::uint32_t d = 0; d < dim; ++d) out.core_mean[d] += v[d];
  }
  const float inv = 1.0f / static_cast<float>(core.size());
  for (float& v : out.core_mean) v *= inv;
  const std::size_t ex = std::min<std::size_t>(core.size(), p.max_exemplars);
  out.exemplars.reserve(ex * dim);
  for (std::size_t i = 0; i < ex; ++i) {
    const std::size_t f = refs[core[i].second];
    out.exemplar_ids.push_back(in.faces[f].id);
    const float* v = row(f);
    out.exemplars.insert(out.exemplars.end(), v, v + dim);
  }
  return out;
}

bool eligible(const person_proto& p) {
  // An unnamed, unpinned one-face person is not evidence of anybody: joining
  // it is regroup's call, not a move's.
  return p.named || p.pinned || p.exemplar_ids.size() >= 2;
}

struct verdict {
  std::int64_t to = 0;
  refine_why why = refine_why::evict;
  float own = kNoEvidence;
  float best = kNoEvidence;
  float second = kNoEvidence;
  std::int64_t best_person = 0;
  bool change = false;
};

}  // namespace

refine_output refine_people(const refine_input& in, const refine_params& p) {
  refine_output out;
  const std::uint32_t dim = in.dim;
  if (dim == 0 || in.faces.empty()) return out;
  const std::size_t nf = in.faces.size();
  const auto row = [&](std::size_t i) { return in.emb.data() + std::size_t{in.faces[i].row} * dim; };
  const auto is_named = [&](std::int64_t person) {
    return std::binary_search(in.named.begin(), in.named.end(), person);
  };

  std::vector<std::int64_t> where(nf);
  for (std::size_t i = 0; i < nf; ++i) where[i] = in.faces[i].person;
  std::vector<bool> frozen(nf, false);
  std::vector<refine_move> moved(nf);  // valid where frozen
  std::set<std::int64_t> stale;  // persons changed since their prototype was built
  std::vector<verdict> last(nf);
  const auto cancelled = [&] { return in.cancel && in.cancel->load(std::memory_order_relaxed); };

  // Prototypes by person. Pass 0 builds them all; a later pass rebuilds only
  // the persons the previous pass changed, and re-judges only their faces and
  // faces whose best rival was one of them (the rest saw no input change).
  // Only persons given in full are ever built here: a cached (fixed) person
  // that gains a face keeps its cached prototype, never one of that face alone.
  std::set<std::int64_t> given;
  for (const refine_face& f : in.faces) {
    if (f.person > 0) given.insert(f.person);
  }
  std::map<std::int64_t, person_proto> built;
  std::set<std::int64_t> changed;
  std::vector<std::pair<float, const person_proto*>> shortlist;
  for (std::uint32_t pass = 0; pass < std::max<std::uint32_t>(p.passes, 1); ++pass) {
    if (cancelled()) return refine_output{};
    ++out.passes;
    // Jacobi: every face in a pass is judged against the same prototypes,
    // so the order faces are visited in never matters.
    std::map<std::int64_t, std::vector<std::size_t>> members;
    for (std::size_t i = 0; i < nf; ++i) {
      if (given.count(where[i]) && (pass == 0 || changed.count(where[i]))) members[where[i]].push_back(i);
    }
    for (std::int64_t person : changed) built.erase(person);
    for (auto& [person, m] : members) {
      built[person] = build_proto(in, p, person, std::move(m), is_named(person));
    }
    stale.clear();
    std::vector<const person_proto*> cands;
    for (const auto& [person, pr] : built) {
      if (eligible(pr)) cands.push_back(&pr);
    }
    for (const person_proto& pr : in.fixed) {
      if (!built.count(pr.person) && eligible(pr)) cands.push_back(&pr);
    }

    std::vector<std::pair<std::size_t, verdict>> changes;
    for (std::size_t i = 0; i < nf; ++i) {
      const refine_face& f = in.faces[i];
      if (f.pinned || frozen[i]) continue;
      if (pass > 0 && !changed.count(where[i]) && !changed.count(last[i].best_person)) continue;
      if ((i & 1023) == 0 && cancelled()) return refine_output{};
      const float* v = row(i);
      const std::int64_t own = where[i];
      verdict vd;
      if (own > 0) {
        if (auto it = built.find(own); it != built.end()) {
          vd.own = support(v, it->second, dim, f.id, p.top_k);
        }
      }
      // Prefilter by the core mean (mean pairwise cosine), then exemplars.
      shortlist.clear();
      for (const person_proto* c : cands) {
        if (c->person == own || rejected_from(f, c->person)) continue;
        shortlist.emplace_back(dot(v, c->core_mean.data(), dim), c);
      }
      const std::size_t keep_n = std::min<std::size_t>(shortlist.size(), std::max<std::uint32_t>(p.shortlist, 1));
      std::partial_sort(shortlist.begin(), shortlist.begin() + static_cast<std::ptrdiff_t>(keep_n),
                        shortlist.end(), [](const auto& a, const auto& b) {
                          return a.first != b.first ? a.first > b.first : a.second->person < b.second->person;
                        });
      for (std::size_t s = 0; s < keep_n; ++s) {
        const float sc = support(v, *shortlist[s].second, dim, f.id, p.top_k);
        if (sc > vd.best) {
          vd.second = vd.best;
          vd.best = sc;
          vd.best_person = shortlist[s].second->person;
        } else if (sc > vd.second) {
          vd.second = sc;
        }
      }
      const bool weak = f.quality < p.weak_quality;
      const float rival = std::max(vd.own, vd.second);
      if (!weak && vd.best_person != 0 && vd.best >= p.join && vd.best - rival >= p.margin) {
        vd.change = true;
        vd.to = vd.best_person;
        vd.why = own > 0 ? refine_why::move : refine_why::admit;
      } else if (own > 0 && vd.own != kNoEvidence && vd.own < (weak ? p.keep_weak : p.keep)) {
        vd.change = true;
        vd.to = 0;
        vd.why = refine_why::evict;
      }
      last[i] = vd;
      if (vd.change) changes.emplace_back(i, vd);
    }
    if (changes.empty()) break;
    changed.clear();
    for (const auto& [i, vd] : changes) {
      moved[i] = refine_move{in.faces[i].id, where[i], vd.to, vd.why, vd.own, vd.to ? vd.best : kNoEvidence};
      if (where[i] > 0) changed.insert(where[i]);
      if (vd.to > 0) changed.insert(vd.to);
      where[i] = vd.to;
      frozen[i] = true;
    }
    stale = changed;
  }

  // Borderline: kept, but the call was close. A better embedding (flip
  // averaging, a fresh alignment) is what can settle them.
  {
    std::vector<std::pair<float, std::int64_t>> close;
    for (std::size_t i = 0; i < nf; ++i) {
      const refine_face& f = in.faces[i];
      if (f.pinned || frozen[i] || where[i] <= 0) continue;
      const verdict& vd = last[i];
      if (vd.own == kNoEvidence) continue;
      const bool shaky = vd.own < p.join;
      const bool contested = vd.best != kNoEvidence && vd.best + p.margin > vd.own;
      if (shaky || contested) close.emplace_back(vd.own - std::max(vd.best, 0.0f), f.id);
    }
    std::sort(close.begin(), close.end());
    for (std::size_t i = 0; i < close.size() && i < p.max_recheck; ++i) out.recheck.push_back(close[i].second);
  }

  // Regroup: good unassigned faces that resemble each other become new people.
  if (in.regroup) {
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < nf; ++i) {
      if (where[i] == 0 && !in.faces[i].pinned && in.faces[i].quality >= p.weak_quality) pool.push_back(i);
    }
    std::sort(pool.begin(), pool.end(), [&](std::size_t a, std::size_t b) {
      const refine_face& fa = in.faces[a];
      const refine_face& fb = in.faces[b];
      return fa.quality != fb.quality ? fa.quality > fb.quality : fa.id < fb.id;
    });
    if (pool.size() > p.max_regroup) pool.resize(p.max_regroup);
    std::vector<bool> used(pool.size(), false);
    for (std::size_t a = 0; a < pool.size(); ++a) {
      if (used[a]) continue;
      if ((a & 255) == 0 && cancelled()) return refine_output{};
      std::vector<std::pair<float, std::size_t>> near;
      for (std::size_t b = 0; b < pool.size(); ++b) {
        if (b == a || used[b]) continue;
        const float s = dot(row(pool[a]), row(pool[b]), dim);
        if (s >= p.join) near.emplace_back(s, b);
      }
      if (near.size() + 1 < p.min_group) continue;
      std::stable_sort(near.begin(), near.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
      std::vector<std::size_t> group{a};
      for (const auto& [_, b] : near) {
        // Average linkage to everyone already in: a chain cannot form.
        float sum = 0;
        for (std::size_t g : group) sum += dot(row(pool[b]), row(pool[g]), dim);
        if (sum / static_cast<float>(group.size()) >= p.join) group.push_back(b);
      }
      if (group.size() < p.min_group) continue;
      const std::int64_t to = -static_cast<std::int64_t>(++out.groups);
      for (std::size_t g : group) {
        used[g] = true;
        const std::size_t i = pool[g];
        if (frozen[i]) {
          moved[i].to = to;  // evicted this call: straight to the new person
          moved[i].why = refine_why::regroup;
        } else {
          moved[i] = refine_move{in.faces[i].id, 0, to, refine_why::regroup, kNoEvidence, kNoEvidence};
          frozen[i] = true;
        }
        where[i] = to;
      }
    }
  }

  for (std::size_t i = 0; i < nf; ++i) {
    if (frozen[i] && moved[i].to != moved[i].from) out.moves.push_back(moved[i]);
  }
  // The prototypes the caller caches, as of the final assignment: the persons
  // this call was given in full (a fixed person that gained a face is rebuilt
  // by the caller's next call, from all its faces).
  {
    std::map<std::int64_t, std::vector<std::size_t>> members;
    for (std::size_t i = 0; i < nf; ++i) {
      if (given.count(where[i]) && stale.count(where[i])) members[where[i]].push_back(i);
    }
    for (std::int64_t person : stale) built.erase(person);
    for (auto& [person, m] : members) {
      built[person] = build_proto(in, p, person, std::move(m), is_named(person));
    }
    for (auto& [person, pr] : built) out.protos.push_back(std::move(pr));
  }
  return out;
}

float face_quality(float score, float side_px, float sharpness, float frontal) {
  const float s_score = clamp01((score - 0.6f) / 0.35f);
  const float s_size = clamp01((side_px - 40.0f) / 72.0f);  // 40 px useless .. 112 px (SFace's input)
  const float s_sharp = sharpness <= 0 ? 0.0f
                                       : clamp01((std::log(sharpness) - std::log(20.0f)) /
                                                 (std::log(300.0f) - std::log(20.0f)));
  // A face is as good as its worst attribute; the detector's score shades it.
  return std::min({s_size, s_sharp, clamp01(frontal)}) * (0.6f + 0.4f * s_score);
}

float face_quality_proxy(float score, float w, float h) {
  // Stills are analysed at 1024 on the long edge: ~768 on the short one.
  const float side = std::min(w, h) * 768.0f;
  const float s_score = clamp01((score - 0.6f) / 0.35f);
  return clamp01((side - 40.0f) / 72.0f) * (0.6f + 0.4f * s_score);
}

float crop_sharpness(std::span<const float> chw, std::uint32_t side) {
  const std::size_t plane = std::size_t{side} * side;
  if (side < 3 || chw.size() < plane * 3) return 0;
  std::vector<float> y(plane);
  for (std::size_t i = 0; i < plane; ++i) {
    y[i] = 0.299f * chw[i] + 0.587f * chw[plane + i] + 0.114f * chw[2 * plane + i];
  }
  double sum = 0, sq = 0;
  std::size_t n = 0;
  for (std::uint32_t r = 1; r + 1 < side; ++r) {
    for (std::uint32_t c = 1; c + 1 < side; ++c) {
      const std::size_t i = std::size_t{r} * side + c;
      const double l = 4.0 * y[i] - y[i - 1] - y[i + 1] - y[i - side] - y[i + side];
      sum += l;
      sq += l * l;
      ++n;
    }
  }
  const double mean = sum / static_cast<double>(n);
  return static_cast<float>(sq / static_cast<double>(n) - mean * mean);
}

float landmark_frontalness(std::span<const float, 10> lm) {
  const float ex = lm[2] - lm[0], ey = lm[3] - lm[1];
  const float eye = std::sqrt(ex * ex + ey * ey);
  if (eye <= 1e-3f) return 0;
  const float mx = (lm[0] + lm[2]) / 2, my = (lm[1] + lm[3]) / 2;
  // The nose's offset along the eye line, as a share of the eye distance:
  // ~0 frontal, ~0.5 and beyond a profile.
  const float along = ((lm[4] - mx) * ex + (lm[5] - my) * ey) / eye;
  return clamp01(1.0f - std::fabs(along) / (0.5f * eye));
}

}  // namespace mv::ai
