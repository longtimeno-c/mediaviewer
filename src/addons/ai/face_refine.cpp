// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/face_refine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>

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

// Mean of the top k cosines of `v` to `count` rows of `data`, skipping rows
// `skip(i)` says to. kNoEvidence when none is left.
template <class Skip>
float top_mean(const float* v, const float* data, std::size_t count, std::uint32_t dim, std::uint32_t top_k,
               Skip&& skip) {
  float best[16];
  const std::uint32_t k_max = std::min<std::uint32_t>(std::max<std::uint32_t>(top_k, 1), 16);
  std::uint32_t have = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (skip(i)) continue;
    const float s = dot(v, data + i * dim, dim);
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

// Mean of the top k cosines of `v` to the exemplars, skipping the face itself
// and, while the person has exemplars from other photos or clips, those of
// the face's own: a clip's frames resemble each other more than anyone, so
// they vouch for a face only when nothing else can (2026-11-02).
float support(const float* v, const person_proto& p, std::uint32_t dim, std::int64_t self, std::uint32_t top_k,
              std::int64_t self_asset = -1) {
  const std::size_t n = p.exemplar_ids.size();
  const bool assets = self_asset >= 0 && p.exemplar_assets.size() == n;
  bool skip_asset = false;
  if (assets) {
    for (std::size_t i = 0; i < n && !skip_asset; ++i) {
      skip_asset = p.exemplar_ids[i] != self && p.exemplar_assets[i] != self_asset;
    }
  }
  return top_mean(v, p.exemplars.data(), n, dim, top_k, [&](std::size_t i) {
    return p.exemplar_ids[i] == self || (skip_asset && p.exemplar_assets[i] == self_asset);
  });
}

float negative(const float* v, const person_proto& p, std::uint32_t dim, std::uint32_t top_k) {
  if (dim == 0 || p.negatives.empty()) return kNoEvidence;
  return top_mean(v, p.negatives.data(), p.negatives.size() / dim, dim, top_k, [](std::size_t) { return false; });
}

// Builds a person's prototype from its members (indices into in.faces).
person_proto build_proto(const refine_input& in, const refine_params& p, std::int64_t person,
                         std::vector<std::size_t> members, bool named,
                         const std::set<std::int64_t>* distrust = nullptr) {
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
  if (refs.empty()) {
    // All weak: the best of them still describe a person the user named;
    // anyone else has nothing that vouches for them (refine_params).
    out.weak_only = !named;
    refs = members;
  }
  if (refs.size() > p.max_refs) refs.resize(p.max_refs);
  const std::size_t n = refs.size();
  out.members = static_cast<std::uint32_t>(n);
  if (n == 0) return out;

  std::vector<float> sim(n * n, 0.0f);
  for (std::size_t a = 0; a < n; ++a) {
    sim[a * n + a] = 1.0f;
    for (std::size_t b = a + 1; b < n; ++b) {
      const float s = dot(row(refs[a]), row(refs[b]), dim);
      sim[a * n + b] = s;
      sim[b * n + a] = s;
    }
  }
  // Medoid: the ref most like all the others, each photo or clip counted
  // once (its refs share a vote), so a long clip of a lookalike cannot
  // outvote the photos.
  std::map<std::int64_t, std::uint32_t> per_asset;
  for (std::size_t a = 0; a < n; ++a) {
    if (in.faces[refs[a]].asset >= 0) ++per_asset[in.faces[refs[a]].asset];
  }
  std::vector<float> weight(n, 1.0f);
  for (std::size_t a = 0; a < n; ++a) {
    const std::int64_t as = in.faces[refs[a]].asset;
    if (as >= 0) weight[a] = 1.0f / static_cast<float>(per_asset[as]);
  }
  std::size_t medoid = 0;
  float medoid_sum = -1e30f;
  for (std::size_t a = 0; a < n; ++a) {
    float s = 0;
    for (std::size_t b = 0; b < n; ++b) s += sim[a * n + b] * weight[b];
    if (s > medoid_sum) {
      medoid_sum = s;
      medoid = a;
    }
  }
  // The pins the person is defined by: every usable pin (the user's word),
  // except those refine_people found another person explains clearly better
  // (`distrust`). A pin is doubtful (the chrome asks about it) when it is
  // unusable, distrusted, or lone: it agrees with none of the person's other
  // pins nor with the medoid.
  std::vector<std::size_t> pins;
  for (std::size_t a = 0; a < n; ++a) {
    if (in.faces[refs[a]].pinned) pins.push_back(a);
  }
  out.pinned = !pins.empty();
  std::vector<std::size_t> usable;
  for (std::size_t a : pins) {
    if (in.faces[refs[a]].quality >= p.unusable_quality) usable.push_back(a);
  }
  // Only unusable pins and nothing else to go by: still the user's word.
  if (usable.empty() && pins.size() == n) usable = pins;
  std::vector<std::size_t> trusted;
  for (std::size_t a : pins) {
    const std::int64_t id = in.faces[refs[a]].id;
    const bool is_usable = std::find(usable.begin(), usable.end(), a) != usable.end();
    const bool distrusted = distrust && distrust->count(id);
    bool lone = !pins.empty() && sim[medoid * n + a] < p.core;
    for (std::size_t b : usable) {
      if (b != a && sim[a * n + b] >= p.core) lone = false;
    }
    if (is_usable && !distrusted) trusted.push_back(a);
    if (!is_usable || distrusted || lone) out.doubtful_pins.push_back(id);
  }
  std::vector<std::size_t> anchors = trusted;
  if (anchors.empty()) {
    anchors.push_back(medoid);
  } else if (std::find(anchors.begin(), anchors.end(), medoid) == anchors.end()) {
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
  // Exemplars: one per photo or clip, except the anchors the user pinned;
  // the closest and clearest first. Farthest-point choice (the person's
  // range) was tried 2026-11-02: LFW the same, the owner's copy 5 -> 9 faces
  // better matched by a different person.
  std::set<std::int64_t> seen;
  out.exemplars.reserve(std::min<std::size_t>(core.size(), p.max_exemplars) * dim);
  for (const auto& [score, a] : core) {
    if (out.exemplar_ids.size() >= p.max_exemplars) break;
    const refine_face& f = in.faces[refs[a]];
    const bool pinned_anchor = score >= 3.0f;
    if (f.asset >= 0 && !seen.insert(f.asset).second && !pinned_anchor) continue;
    out.exemplar_ids.push_back(f.id);
    out.exemplar_assets.push_back(f.asset);
    const float* v = row(refs[a]);
    out.exemplars.insert(out.exemplars.end(), v, v + dim);
  }
  return out;
}

bool eligible(const person_proto& p) {
  // An unnamed, unpinned one-face person is not evidence of anybody: joining
  // it is regroup's call, not a move's.
  return p.named || p.pinned || (!p.weak_only && p.exemplar_ids.size() >= 2);
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

  // Faces of one photo or frame are different people (a cannot-link), unless
  // they are the same face twice (a mirror, a screen, a photo in the photo):
  // as alike as an unmistakable pair, join + 2 margins (2026-11-02: 13 of
  // the owner's frames hold two faces of one person).
  std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> frames;
  for (std::size_t i = 0; i < nf; ++i) {
    if (in.faces[i].asset >= 0) frames[{in.faces[i].asset, in.faces[i].pts_ms}].push_back(i);
  }
  static const std::vector<std::size_t> kAlone;
  const auto siblings = [&](std::size_t i) -> const std::vector<std::size_t>& {
    if (in.faces[i].asset < 0) return kAlone;
    return frames.find({in.faces[i].asset, in.faces[i].pts_ms})->second;
  };
  const float twice = p.join + 2 * p.margin;
  const auto apart = [&](std::size_t i, std::size_t j) { return j != i && dot(row(i), row(j), dim) < twice; };
  const auto holds_sibling = [&](std::size_t i, std::int64_t person) {
    for (std::size_t j : siblings(i)) {
      if (where[j] == person && apart(i, j)) return true;
    }
    return false;
  };
  // What the user said is not each person ("not this person"): the clearest
  // max_negatives of the faces rejected from it.
  std::map<std::int64_t, person_proto> negs;  // filled in pass 0, once the persons are built
  std::map<std::int64_t, std::vector<std::size_t>> neg_rows;
  for (std::size_t i = 0; i < nf; ++i) {
    for (std::int64_t r : in.faces[i].rejected) neg_rows[r].push_back(i);
  }
  for (auto& [person, xs] : neg_rows) {
    std::stable_sort(xs.begin(), xs.end(),
                     [&](std::size_t a, std::size_t b) { return in.faces[a].quality > in.faces[b].quality; });
    if (xs.size() > p.max_negatives) xs.resize(p.max_negatives);
  }
  const auto neg = [&](const float* v, std::int64_t person) {
    const auto it = negs.find(person);
    return it == negs.end() ? kNoEvidence : negative(v, it->second, dim, p.top_k);
  };

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
  // Lone pins another person explains clearly better: not anchors (pass 0).
  std::map<std::int64_t, std::set<std::int64_t>> distrust;
  const auto distrust_of = [&](std::int64_t person) -> const std::set<std::int64_t>* {
    const auto it = distrust.find(person);
    return it == distrust.end() ? nullptr : &it->second;
  };
  std::map<std::int64_t, std::size_t> index_of;
  for (std::size_t i = 0; i < nf; ++i) index_of[in.faces[i].id] = i;
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
    std::map<std::int64_t, std::vector<std::size_t>> kept_members;
    for (auto& [person, m] : members) {
      if (pass == 0) kept_members[person] = m;
      built[person] = build_proto(in, p, person, std::move(m), is_named(person), distrust_of(person));
    }
    stale.clear();
    std::vector<const person_proto*> cands;
    for (const auto& [person, pr] : built) {
      if (eligible(pr)) cands.push_back(&pr);
    }
    for (const person_proto& pr : in.fixed) {
      if (!built.count(pr.person) && eligible(pr)) cands.push_back(&pr);
    }
    const auto best_rival = [&](const float* v, const refine_face& f, std::int64_t own) {
      std::vector<std::pair<float, const person_proto*>> sl;
      for (const person_proto* c : cands) {
        if (c->person != own && !rejected_from(f, c->person)) sl.emplace_back(dot(v, c->core_mean.data(), dim), c);
      }
      const std::size_t k = std::min<std::size_t>(sl.size(), std::max<std::uint32_t>(p.shortlist, 1));
      std::partial_sort(sl.begin(), sl.begin() + static_cast<std::ptrdiff_t>(k), sl.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; });
      float best = kNoEvidence;
      for (std::size_t j = 0; j < k; ++j) best = std::max(best, support(v, *sl[j].second, dim, f.id, p.top_k, f.asset));
      return best;
    };
    if (pass == 0) {
      // A lone pin (it agrees with no other pin of the person, nor its
      // medoid) that another person clearly explains is a cover pinned by
      // mistake, typically by a merge of a lookalike: it stops anchoring the
      // person, so the faces only it vouched for are judged on their own.
      // Only while the person keeps another pin: the user's word still
      // defines it. The face stays pinned where it is; the chrome asks.
      for (auto& [person, pr] : built) {
        if (pr.doubtful_pins.empty() || !pr.pinned) continue;
        std::set<std::int64_t> lone(pr.doubtful_pins.begin(), pr.doubtful_pins.end());
        std::size_t others = 0;
        for (std::size_t i : kept_members[person]) {
          others += in.faces[i].pinned && !lone.count(in.faces[i].id) ? 1 : 0;
        }
        if (others == 0) continue;
        const person_proto rest = build_proto(in, p, person, kept_members[person], is_named(person), &lone);
        std::set<std::int64_t> out_pins;
        for (std::int64_t id : lone) {
          const std::size_t i = index_of[id];
          const float* v = row(i);
          const float mine = support(v, rest, dim, id, p.top_k, in.faces[i].asset);
          const float other = best_rival(v, in.faces[i], person);
          // Only a stranger to the rest of the person (below join - margin):
          // a lone pin the rest half-recognises is the person in another
          // look, and a fragment of them may explain it better.
          if (other >= p.join && other - mine >= p.margin && mine < p.join - p.margin) out_pins.insert(id);
        }
        if (out_pins.empty()) continue;
        out.distrusted.insert(out.distrusted.end(), out_pins.begin(), out_pins.end());
        distrust[person] = std::move(out_pins);
        pr = build_proto(in, p, person, kept_members[person], is_named(person), distrust_of(person));
      }
      // A rejection counts against a person unless the person vouches for
      // the rejected face as strongly as for an unmistakable face of theirs
      // (join + 2 margins): a slip of the user's is not evidence against
      // everyone who looks like them. A lookalike that was misfiled at about
      // `join` stays a negative: that is what negatives are for.
      for (auto& [person, xs] : neg_rows) {
        const person_proto* pr = nullptr;
        if (auto it = built.find(person); it != built.end()) pr = &it->second;
        for (const person_proto& fx : in.fixed) {
          if (!pr && fx.person == person) pr = &fx;
        }
        person_proto& np = negs[person];
        np.negatives.clear();
        for (std::size_t i : xs) {
          const float vouched = pr ? support(row(i), *pr, dim, in.faces[i].id, p.top_k, in.faces[i].asset) : kNoEvidence;
          if (vouched >= p.join + 2 * p.margin) continue;
          np.negatives.insert(np.negatives.end(), row(i), row(i) + dim);
        }
      }
    }

    std::vector<std::pair<std::size_t, verdict>> changes;
    for (std::size_t i = 0; i < nf; ++i) {
      const refine_face& f = in.faces[i];
      if (f.pinned || frozen[i]) continue;
      if (in.focus != 0 && f.person != in.focus) continue;
      if (pass > 0 && !changed.count(where[i]) && !changed.count(last[i].best_person)) continue;
      if ((i & 1023) == 0 && cancelled()) return refine_output{};
      const float* v = row(i);
      const std::int64_t own = where[i];
      verdict vd;
      const person_proto* own_proto = nullptr;
      if (own > 0) {
        if (auto it = built.find(own); it != built.end()) {
          own_proto = &it->second;
          vd.own = support(v, it->second, dim, f.id, p.top_k, f.asset);
        }
      }
      // Unusable, or filed under a person nothing vouches for, or one the
      // user said it is not (a merge or an old row can bring it back):
      // unassigned, and never admitted anywhere.
      if (f.quality < p.unusable_quality || (own_proto && own_proto->weak_only) ||
          (own > 0 && rejected_from(f, own))) {
        if (own > 0) {
          vd.change = true;
          vd.to = 0;
          vd.why = refine_why::evict;
          last[i] = vd;
          changes.emplace_back(i, vd);
        }
        continue;
      }
      // A better face of its own frame in the same person: one of them is
      // someone else, and it is this one (pinned faces always win).
      bool outranked = false;
      if (own > 0 && own_proto) {
        for (std::size_t j : siblings(i)) {
          if (where[j] != own || !apart(i, j)) continue;
          const refine_face& g = in.faces[j];
          const float sj = g.pinned ? 2.0f : support(row(j), *own_proto, dim, g.id, p.top_k, g.asset);
          if (sj > vd.own || (sj == vd.own && g.id < f.id)) outranked = true;
        }
      }
      // Prefilter by the core mean (mean pairwise cosine), then exemplars.
      shortlist.clear();
      for (const person_proto* c : cands) {
        if (c->person == own || rejected_from(f, c->person) || holds_sibling(i, c->person)) continue;
        // A smaller person whose core is no stranger to this face's own
        // (mean pairwise cosine across the cores >= join - margin / 2) is a
        // fragment of the same person, not a rival: the merge joins them, a
        // face does not leave its person for it (2026-11-02: 275 of the
        // owner's faces went to his own fragments). join - margin cost LFW one
        // correct move between two lookalikes (settled F 0.9988 -> 0.9985).
        // Into a larger or confirmed one it may: that is the fragment going home.
        const bool confirmed_c = c->named || c->pinned;
        const bool confirmed_own = own_proto && (own_proto->named || own_proto->pinned);
        if (own_proto && own_proto->core_mean.size() == dim && !(confirmed_c && !confirmed_own) &&
            c->members <= own_proto->members &&
            dot(own_proto->core_mean.data(), c->core_mean.data(), dim) >= p.join - p.margin / 2) {
          continue;
        }
        shortlist.emplace_back(dot(v, c->core_mean.data(), dim), c);
      }
      const std::size_t keep_n = std::min<std::size_t>(shortlist.size(), std::max<std::uint32_t>(p.shortlist, 1));
      std::partial_sort(shortlist.begin(), shortlist.begin() + static_cast<std::ptrdiff_t>(keep_n),
                        shortlist.end(), [](const auto& a, const auto& b) {
                          return a.first != b.first ? a.first > b.first : a.second->person < b.second->person;
                        });
      for (std::size_t s = 0; s < keep_n; ++s) {
        const person_proto* cp = shortlist[s].second;
        // By exemplars, confirmed or not: the person's mean instead (online's
        // rule for unconfirmed people) was tried in the settle and left more
        // faces misfiled on the owner's copy (16 -> 18), LFW no better.
        const float sc = support(v, *cp, dim, f.id, p.top_k, f.asset);
        if (sc > vd.best) {
          vd.second = vd.best;
          vd.best = sc;
          vd.best_person = shortlist[s].second->person;
        } else if (sc > vd.second) {
          vd.second = sc;
        }
      }
      const bool weak = f.quality < p.weak_quality;
      const float rival = std::max({outranked ? kNoEvidence : vd.own, vd.second, neg(v, vd.best_person)});
      const float own_neg = own > 0 ? neg(v, own) : kNoEvidence;
      // A face its own person vouches for as for an unmistakable face of
      // theirs (join + 2 margins, from its other photos and clips) is theirs:
      // a rival that scores higher still is a tighter fragment of the same
      // person (one session's clips), which the merge joins, not a reason to
      // move (2026-11-02: 328 of the owner's faces went to such a fragment).
      const bool vouched = own > 0 && !outranked && vd.own >= p.join + 2 * p.margin && own_neg < p.join;
      if (!weak && !vouched && vd.best_person != 0 && vd.best >= p.join && vd.best - rival >= p.margin) {
        vd.change = true;
        vd.to = vd.best_person;
        vd.why = own > 0 ? refine_why::move : refine_why::admit;
      } else if (own > 0 && (outranked || (own_neg >= p.join && own_neg > vd.own) ||
                             (vd.own != kNoEvidence && vd.own < (weak ? p.keep_weak : p.keep)))) {
        vd.change = true;
        vd.to = 0;
        vd.why = refine_why::evict;
      }
      last[i] = vd;
      if (vd.change) changes.emplace_back(i, vd);
    }
    // Two faces of one frame moving into one person in the same pass: the
    // better supported goes, the other stays where it was.
    {
      std::map<std::tuple<std::int64_t, std::int64_t, std::int64_t>, std::size_t> into;  // (asset, pts, to) -> change
      std::vector<bool> drop(changes.size(), false);
      for (std::size_t c = 0; c < changes.size(); ++c) {
        const auto& [i, vd] = changes[c];
        if (vd.to <= 0 || in.faces[i].asset < 0) continue;
        const auto key = std::make_tuple(in.faces[i].asset, in.faces[i].pts_ms, vd.to);
        const auto [it, fresh] = into.emplace(key, c);
        if (fresh || !apart(i, changes[it->second].first)) continue;
        if (vd.best > changes[it->second].second.best) {
          drop[it->second] = true;
          it->second = c;
        } else {
          drop[c] = true;
        }
      }
      std::size_t k = 0;
      for (std::size_t c = 0; c < changes.size(); ++c) {
        if (drop[c]) {
          last[changes[c].first].change = false;
        } else {
          changes[k++] = changes[c];
        }
      }
      changes.resize(k);
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
      if (in.focus != 0 && f.person != in.focus) continue;
      const verdict& vd = last[i];
      if (vd.own == kNoEvidence) continue;
      const bool shaky = vd.own < p.join;
      const bool contested = vd.best != kNoEvidence && vd.best + p.margin > vd.own;
      if (shaky || contested) close.emplace_back(vd.own - std::max(vd.best, 0.0f), f.id);
    }
    std::sort(close.begin(), close.end());
    for (std::size_t i = 0; i < close.size() && i < p.max_recheck; ++i) out.recheck.push_back(close[i].second);
  }

  // Tracks: a weak clip frame nobody has follows the clear frames of its
  // clip near it in time that it resembles at `join`, when they are all in
  // one person (2026-11-02). A weak vector alone never files a face; the
  // same face a few seconds away, seen clearly, does. Never against the
  // user's word (rejected, a frame together, its negatives).
  if (in.focus == 0) {
    std::map<std::int64_t, std::vector<std::size_t>> clips;
    for (std::size_t i = 0; i < nf; ++i) {
      if (in.faces[i].asset >= 0 && in.faces[i].pts_ms >= 0) clips[in.faces[i].asset].push_back(i);
    }
    for (std::size_t i = 0; i < nf; ++i) {
      const refine_face& f = in.faces[i];
      // Only faces nobody had: an eviction this call stands.
      if (f.person != 0 || frozen[i] || f.pinned || f.asset < 0 || f.pts_ms < 0) continue;
      if (f.quality < p.unusable_quality || f.quality >= p.weak_quality) continue;
      std::int64_t to = 0;
      bool split = false;
      for (std::size_t j : clips[f.asset]) {
        const refine_face& g = in.faces[j];
        if (j == i || where[j] <= 0 || g.quality < p.weak_quality) continue;
        if (std::llabs(g.pts_ms - f.pts_ms) > p.track_ms || dot(row(i), row(j), dim) < p.join) continue;
        if (to != 0 && to != where[j]) split = true;
        to = where[j];
      }
      if (to <= 0 || split || rejected_from(f, to) || holds_sibling(i, to) || neg(row(i), to) >= p.join) continue;
      moved[i] = refine_move{f.id, 0, to, refine_why::admit, kNoEvidence, kNoEvidence};
      frozen[i] = true;
      where[i] = to;
      if (given.count(to)) stale.insert(to);
    }
  }

  // Regroup: good unassigned faces that resemble each other become new people.
  if (in.regroup) {
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < nf; ++i) {
      if (where[i] != 0 || in.faces[i].pinned || in.faces[i].quality < p.weak_quality) continue;
      if (in.focus != 0 && in.faces[i].person != in.focus) continue;
      pool.push_back(i);
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
      const auto same_frame = [&](std::size_t x, std::size_t y) {
        const refine_face& fx = in.faces[pool[x]];
        const refine_face& fy = in.faces[pool[y]];
        return fx.asset >= 0 && fx.asset == fy.asset && fx.pts_ms == fy.pts_ms && apart(pool[x], pool[y]);
      };
      for (const auto& [_, b] : near) {
        // Average linkage to everyone already in: a chain cannot form. Never
        // two faces of one frame.
        if (std::any_of(group.begin(), group.end(), [&](std::size_t g) { return same_frame(b, g); })) continue;
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
      built[person] = build_proto(in, p, person, std::move(m), is_named(person), distrust_of(person));
    }
    for (auto& [person, pr] : built) out.protos.push_back(std::move(pr));
  }
  return out;
}

person_proto build_person_proto(std::uint32_t dim, std::span<const float> emb, std::span<const refine_face> faces,
                                std::int64_t person, bool named, const refine_params& p) {
  refine_input in;
  in.dim = dim;
  in.emb = emb;
  in.faces = faces;
  std::vector<std::size_t> members(faces.size());
  std::iota(members.begin(), members.end(), std::size_t{0});
  return build_proto(in, p, person, std::move(members), named);
}

float exemplar_support(std::span<const float> v, const person_proto& p, std::int64_t self, std::uint32_t top_k,
                       std::int64_t self_asset) {
  const auto dim = static_cast<std::uint32_t>(v.size());
  if (dim == 0 || p.exemplars.size() != p.exemplar_ids.size() * dim) return kNoEvidence;
  return support(v.data(), p, dim, self, top_k, self_asset);
}

float negative_support(std::span<const float> v, const person_proto& p, std::uint32_t top_k) {
  const auto dim = static_cast<std::uint32_t>(v.size());
  if (dim == 0 || p.negatives.size() % dim != 0) return kNoEvidence;
  return negative(v.data(), p, dim, top_k);
}

namespace {

// How well b's exemplars vouch for a's: each of a's exemplars takes its
// support by b, and those are averaged. kNoEvidence when either has none.
float vouch(const person_proto& a, const person_proto& b, std::uint32_t dim, std::uint32_t top_k) {
  const std::size_t na = a.exemplar_ids.size();
  if (na == 0 || b.exemplar_ids.empty()) return kNoEvidence;
  float total = 0;
  for (std::size_t i = 0; i < na; ++i) {
    // `self` = 0 skips nothing: a and b are different people.
    total += support(a.exemplars.data() + i * dim, b, dim, 0, top_k);
  }
  return total / static_cast<float>(na);
}

}  // namespace

std::vector<std::vector<std::int64_t>> find_duplicates(const dedupe_input& in, const refine_params& p) {
  const std::uint32_t dim = in.dim;
  std::vector<const person_proto*> ps;
  for (const person_proto& pr : in.protos) {
    if (pr.person > 0 && !pr.exemplar_ids.empty() && pr.core_mean.size() == dim) ps.push_back(&pr);
  }
  std::map<std::int64_t, const dedupe_person*> who;
  for (const dedupe_person& d : in.people) who[d.id] = &d;
  const auto name_of = [&](std::int64_t id) -> const std::string& {
    static const std::string none;
    const auto it = who.find(id);
    return it == who.end() ? none : it->second->name;
  };
  const auto apart = [&](std::int64_t a, std::int64_t b) {
    return std::binary_search(in.apart.begin(), in.apart.end(), std::make_pair(std::min(a, b), std::max(a, b)));
  };
  // A core far from another's is not the same person: skip the exemplar work.
  const float prefilter = p.join - 2 * p.margin;

  struct link {
    std::size_t a, b;
    float support;
  };
  std::vector<link> links;
  std::set<std::pair<std::size_t, std::size_t>> linked;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (in.cancel && in.cancel->load(std::memory_order_relaxed)) return {};
    for (std::size_t j = i + 1; j < ps.size(); ++j) {
      const std::int64_t ia = ps[i]->person, ib = ps[j]->person;
      const std::string& na = name_of(ia);
      const std::string& nb = name_of(ib);
      if (!na.empty() && !nb.empty() && na != nb) continue;
      if (apart(ia, ib)) continue;
      // core_mean is unnormalised: a . b = mean pairwise cosine across the cores.
      if (dot(ps[i]->core_mean.data(), ps[j]->core_mean.data(), dim) < prefilter) continue;
      const float s = std::min(vouch(*ps[i], *ps[j], dim, p.top_k), vouch(*ps[j], *ps[i], dim, p.top_k));
      if (s < p.join) continue;
      links.push_back({i, j, s});
      linked.insert({i, j});
    }
  }
  std::stable_sort(links.begin(), links.end(), [](const link& x, const link& y) { return x.support > y.support; });

  std::vector<std::size_t> group(ps.size());
  std::iota(group.begin(), group.end(), std::size_t{0});
  std::map<std::size_t, std::vector<std::size_t>> members;
  for (std::size_t i = 0; i < ps.size(); ++i) members[i] = {i};
  for (const link& l : links) {
    const std::size_t ga = group[l.a], gb = group[l.b];
    if (ga == gb) continue;
    bool every = true;
    std::set<std::string> names;
    for (std::size_t x : members[ga]) {
      if (const std::string& n = name_of(ps[x]->person); !n.empty()) names.insert(n);
      for (std::size_t y : members[gb]) {
        if (!linked.count({std::min(x, y), std::max(x, y)})) every = false;
      }
    }
    for (std::size_t y : members[gb]) {
      if (const std::string& n = name_of(ps[y]->person); !n.empty()) names.insert(n);
    }
    if (!every || names.size() > 1) continue;
    for (std::size_t y : members[gb]) {
      group[y] = ga;
      members[ga].push_back(y);
    }
    members.erase(gb);
  }

  std::vector<std::vector<std::int64_t>> out;
  for (auto& [g, xs] : members) {
    if (xs.size() < 2) continue;
    std::vector<std::int64_t> ids;
    for (std::size_t x : xs) ids.push_back(ps[x]->person);
    const auto rank = [&](std::int64_t id) {
      const auto it = who.find(id);
      const bool named = it != who.end() && !it->second->name.empty();
      const std::uint32_t n = it == who.end() ? 0 : it->second->faces;
      return std::make_tuple(named ? 1 : 0, n, -id);
    };
    std::sort(ids.begin(), ids.end(), [&](std::int64_t x, std::int64_t y) { return rank(x) > rank(y); });
    out.push_back(std::move(ids));
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
