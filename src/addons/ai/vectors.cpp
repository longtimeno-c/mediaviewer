// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/vectors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#include "addons/ai/index_db.h"
#include "addons/ai/vocabulary.h"

namespace mv::ai {

std::int32_t dot_i8(const std::int8_t* a, const std::int8_t* b, std::size_t n) noexcept {
  // Four independent int32 accumulators: the compilers vectorise this
  // (int8 -> int16 multiply-add) at their default target on x64 and arm64.
  std::int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += static_cast<std::int32_t>(a[i]) * b[i];
    s1 += static_cast<std::int32_t>(a[i + 1]) * b[i + 1];
    s2 += static_cast<std::int32_t>(a[i + 2]) * b[i + 2];
    s3 += static_cast<std::int32_t>(a[i + 3]) * b[i + 3];
  }
  for (; i < n; ++i) s0 += static_cast<std::int32_t>(a[i]) * b[i];
  return s0 + s1 + s2 + s3;
}

void vector_store::reset(std::uint32_t dim) {
  std::unique_lock lock(m_);
  dim_ = dim;
  asset_.clear();
  pts_.clear();
  generic_.clear();
  scale_.clear();
  bar_.clear();
  labels_.clear();
  label_scale_.clear();
  alive_.clear();
  data_.clear();
  dead_ = 0;
  ++layout_;
}

void vector_store::clear() {
  std::unique_lock lock(m_);
  asset_.clear();
  pts_.clear();
  generic_.clear();
  scale_.clear();
  bar_.clear();
  alive_.clear();
  data_.clear();
  dead_ = 0;
  ++layout_;
}

std::size_t vector_store::rows() const {
  std::shared_lock lock(m_);
  return asset_.size();
}

std::size_t vector_store::live_rows() const {
  std::shared_lock lock(m_);
  return asset_.size() - dead_;
}

void vector_store::add(std::int64_t asset, std::int64_t pts_ms, float generic, float scale,
                       std::span<const std::int8_t> q) {
  std::unique_lock lock(m_);
  if (q.size() != dim_ || dim_ == 0) return;
  asset_.push_back(asset);
  pts_.push_back(pts_ms);
  generic_.push_back(generic);
  scale_.push_back(scale);
  alive_.push_back(1);
  data_.insert(data_.end(), q.begin(), q.end());
  bar_.push_back(label_bar_locked(asset_.size() - 1));
}

void vector_store::remove_asset(std::int64_t asset) {
  std::unique_lock lock(m_);
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (asset_[r] == asset && alive_[r]) {
      alive_[r] = 0;
      ++dead_;
    }
  }
  if (dead_ > 1024 && dead_ * 4 > asset_.size()) compact_locked();
}

void vector_store::compact_locked() {
  std::size_t w = 0;
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (!alive_[r]) continue;
    if (w != r) {
      asset_[w] = asset_[r];
      pts_[w] = pts_[r];
      generic_[w] = generic_[r];
      scale_[w] = scale_[r];
      bar_[w] = bar_[r];
      std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(r * dim_), dim_,
                  data_.begin() + static_cast<std::ptrdiff_t>(w * dim_));
    }
    alive_[w] = 1;
    ++w;
  }
  asset_.resize(w);
  pts_.resize(w);
  generic_.resize(w);
  scale_.resize(w);
  bar_.resize(w);
  alive_.resize(w);
  data_.resize(w * dim_);
  dead_ = 0;
  ++layout_;
}

float vector_store::label_bar_locked(std::size_t row) const noexcept {
  const std::size_t n = label_scale_.size();
  if (n <= kLabelsAbove) return std::numeric_limits<float>::quiet_NaN();
  // The (kLabelsAbove + 1)-th best label score: a short insertion list.
  std::array<float, kLabelsAbove + 1> best;
  best.fill(-2.0f);
  const std::int8_t* r = data_.data() + row * dim_;
  for (std::size_t l = 0; l < n; ++l) {
    const float s = static_cast<float>(dot_i8(r, labels_.data() + l * dim_, dim_)) * scale_[row] * label_scale_[l];
    if (s <= best.back()) continue;
    std::size_t i = best.size() - 1;
    for (; i > 0 && best[i - 1] < s; --i) best[i] = best[i - 1];
    best[i] = s;
  }
  return best.back();
}

bool vector_store::has_labels() const {
  std::shared_lock lock(m_);
  return !label_scale_.empty();
}

void vector_store::set_labels(std::span<const float> vocab, std::size_t count) {
  std::uint64_t layout = 0;
  std::size_t rows = 0;
  {
    std::unique_lock lock(m_);
    if (dim_ == 0 || vocab.size() != count * dim_) return;
    labels_.clear();
    label_scale_.clear();
    std::vector<std::int8_t> q;
    for (std::size_t l = 0; l < count; ++l) {
      float scale = 1;
      quantise(vocab.subspan(l * dim_, dim_), q, scale);
      labels_.insert(labels_.end(), q.begin(), q.end());
      label_scale_.push_back(scale);
    }
    std::fill(bar_.begin(), bar_.end(), std::numeric_limits<float>::quiet_NaN());
    layout = layout_;
    rows = asset_.size();
  }
  // The rows already here, a chunk at a time under the shared lock (searches
  // go on; the indexer's add waits one chunk at most), each chunk stored under
  // a brief exclusive one. Rows moved meanwhile (a compaction): start again.
  constexpr std::size_t kChunk = 512;
  std::vector<float> bars;
  for (std::size_t at = 0; at < rows;) {
    const std::size_t end = std::min(rows, at + kChunk);
    bars.clear();
    {
      std::shared_lock lock(m_);
      if (layout_ != layout) {
        rows = asset_.size();
        layout = layout_;
        at = 0;
        continue;
      }
      for (std::size_t r = at; r < end; ++r) bars.push_back(label_bar_locked(r));
    }
    std::unique_lock lock(m_);
    if (layout_ != layout) continue;
    std::copy(bars.begin(), bars.end(), bar_.begin() + static_cast<std::ptrdiff_t>(at));
    at = end;
  }
}

std::vector<vector_store::hit> vector_store::scan(std::span<const float> query,
                                                  const std::function<bool(std::int64_t)>& allow,
                                                  std::size_t k, bool use_margin, float min_margin,
                                                  float min_score, scan_stats* stats, bool use_labels) const {
  std::vector<hit> out;
  if (stats) *stats = scan_stats{};
  std::shared_lock lock(m_);
  if (query.size() != dim_ || dim_ == 0 || asset_.empty() || k == 0) return out;
  std::vector<std::int8_t> q;
  float qs = 1;
  quantise(query, q, qs);
  // One allow() call per asset run: rows of an asset are mostly contiguous.
  std::int64_t last_asset = -1;
  bool last_ok = false;
  std::vector<hit> all;
  all.reserve(std::min<std::size_t>(asset_.size(), 1u << 16));
  // Each asset's best score (a run of its rows; rows are mostly contiguous).
  std::vector<float> bests;
  std::int64_t run_asset = -1;
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (!alive_[r]) continue;
    if (asset_[r] != last_asset) {
      last_asset = asset_[r];
      last_ok = !allow || allow(last_asset);
    }
    if (!last_ok) continue;
    const float score =
        static_cast<float>(dot_i8(q.data(), data_.data() + r * dim_, dim_)) * qs * scale_[r];
    if (stats) {
      if (asset_[r] != run_asset) {
        run_asset = asset_[r];
        bests.push_back(score);
      } else {
        bests.back() = std::max(bests.back(), score);
      }
    }
    if (score < min_score) continue;
    if (use_margin && score - generic_[r] < min_margin) continue;
    if (use_labels && score < bar_[r]) continue;  // false for NaN: not known yet
    all.push_back(hit{asset_[r], pts_[r], score, generic_[r]});
  }
  if (all.size() > k) {
    std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k), all.end(),
                     [](const hit& a, const hit& b) { return a.score > b.score; });
    all.resize(k);
  }
  std::sort(all.begin(), all.end(), [](const hit& a, const hit& b) { return a.score > b.score; });
  if (stats) {
    stats->assets = bests.size();
    if (bests.size() >= scan_stats::kMinAssets) {
      double mean = 0;
      for (float b : bests) mean += b;
      mean /= static_cast<double>(bests.size());
      double var = 0;
      for (float b : bests) var += (b - mean) * (b - mean);
      const double sd = std::sqrt(var / static_cast<double>(bests.size()));
      std::partial_sort(bests.begin(), bests.begin() + 10, bests.end(), std::greater<float>());
      double top = 0;
      for (std::size_t i = 0; i < 10; ++i) top += bests[i];
      stats->mean = static_cast<float>(mean);
      stats->sd = static_cast<float>(sd);
      if (sd > 0) stats->top10_z = static_cast<float>((top / 10 - mean) / sd);
    }
  }
  return all;
}

std::vector<vector_store::hit> vector_store::rows_of(std::int64_t asset,
                                                     std::span<const float> query) const {
  std::vector<hit> out;
  std::shared_lock lock(m_);
  if (query.size() != dim_ || dim_ == 0) return out;
  std::vector<std::int8_t> q;
  float qs = 1;
  quantise(query, q, qs);
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (!alive_[r] || asset_[r] != asset) continue;
    const float score = static_cast<float>(dot_i8(q.data(), data_.data() + r * dim_, dim_)) * qs * scale_[r];
    out.push_back(hit{asset, pts_[r], score, generic_[r]});
  }
  std::sort(out.begin(), out.end(), [](const hit& a, const hit& b) { return a.pts_ms < b.pts_ms; });
  return out;
}

bool vector_store::vector_of(std::int64_t asset, std::int64_t pts_ms, std::vector<float>& out) const {
  std::shared_lock lock(m_);
  std::size_t best = asset_.size();
  std::int64_t best_d = 0;
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (!alive_[r] || asset_[r] != asset) continue;
    const std::int64_t d = pts_ms < 0 ? 0 : std::llabs(pts_[r] - pts_ms);
    if (best == asset_.size() || d < best_d) {
      best = r;
      best_d = d;
    }
  }
  if (best == asset_.size()) return false;
  out.resize(dim_);
  for (std::uint32_t i = 0; i < dim_; ++i) out[i] = data_[best * dim_ + i] * scale_[best];
  float n = 0;
  for (float v : out) n += v * v;
  n = std::sqrt(n);
  if (n > 0) {
    for (float& v : out) v /= n;
  }
  return true;
}

namespace {
// Acklam's rational approximation of the standard normal quantile (|error|
// below 1.2e-9), enough for a threshold.
double normal_quantile(double p) {
  static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                 1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
  static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                 6.680131188771972e+01,  -1.328068155288572e+01};
  static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                 -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
  static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                 3.754408661907416e+00};
  constexpr double lo = 0.02425;
  if (p < lo) {
    const double q = std::sqrt(-2 * std::log(p));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
           ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  }
  if (p > 1 - lo) return -normal_quantile(1 - p);
  const double q = p - 0.5, r = q * q;
  return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
         (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
}
}  // namespace

precision_scale precision_scale::at(std::uint32_t level) noexcept {
  // Calibrated on 300 and 1,000 COCO photos with both towers (plan/17
  // "Precision scale (2026-09-27)", mv_ai_tests "[.calibration]"). Stricter:
  // 3 is where L/14 stops answering "helicopter" with planes, 4 where B/32
  // does too (its "a helicopter" stands out at 1.33 x noise, like its "a
  // dog"), with captions still found >= 88 %. Looser: more rows, and a few
  // more nonsense queries answered.
  switch (level) {
    // Stand-out factors moved up with the calibrated one (1.15 -> 1.18,
    // issue #85); the rest as calibrated.
    case 0: return {1.13f, 0.80f, 0.33f, 0.80f, 0.0f};
    case 1: return {1.15f, 0.90f, 0.67f, 0.90f, 0.0f};
    case 3: return {1.26f, 1.125f, 1.5f, 1.2f, 3.0f};
    case 4: return {1.38f, 1.25f, 2.0f, 1.4f, 2.0f};
    default: return {};
  }
}

std::vector<vector_store::hit> find_text(const vector_store& store, std::span<const float> query,
                                         const std::function<bool(std::int64_t)>& allow,
                                         const text_thresholds& t, const precision_scale& p, bool gate,
                                         std::size_t k) {
  vector_store::scan_stats stats;
  // One pass at the lower of the calibrated and the level's row margin: the
  // "nothing found" test below always reads the calibrated rows, so a level
  // moves only its own thresholds, and every stricter answer is a subset of
  // the calibrated one (every looser one a superset).
  const float row_margin = t.result_margin * p.result_margin;
  // Rows the query does not beat its labels on are never results (vocabulary.h).
  auto hits = store.scan(query, allow, k, true, std::min(t.result_margin, row_margin), -1.0f, &stats, true);
  float best = -1;  // the best margin among the ten best calibrated rows
  for (std::size_t i = 0, seen = 0; i < hits.size() && seen < 10; ++i) {
    const float margin = hits[i].score - hits[i].generic;
    if (margin < t.result_margin) continue;
    best = std::max(best, margin);
    ++seen;
  }
  {
    // How many assets clear the calibrated query margin (scan_stats::
    // over_margin), counted up to what margin_needed asks for.
    const std::size_t enough = stats.broad_assets();
    std::set<std::int64_t> seen;
    for (const vector_store::hit& h : hits) {
      if (h.score - h.generic < t.query_margin) continue;
      if (seen.insert(h.asset).second && seen.size() >= enough) break;
    }
    stats.over_margin = seen.size();
  }
  if (row_margin > t.result_margin) {
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](const vector_store::hit& h) { return h.score - h.generic < row_margin; }),
               hits.end());
  }
  // query_z 0: the margin rule alone (CLAP, until it has its own z calibration).
  const bool z_rule = t.query_z > 0;
  // Against what noise scores in an index this size (scan_stats): at 1,000
  // COCO photos every nonsense query cleared a fixed z of 2.5 (2.56-2.88)
  // while real ones sat at 3.49 and up; one nonsense query's best margin was
  // 0.050 at 300 (plan/17, 2026-09-27). The calibrated query_z moves with the
  // level's stand-out factor, so a small index is as strict as a large one.
  const float query_z = t.query_z * (p.stand_out / vector_store::scan_stats::kStandOutOverNoise);
  const bool stands_out = z_rule && stats.stands_out(query_z, p.stand_out);
  const float margin_needed = (z_rule ? stats.margin_needed(t.query_margin) : t.query_margin) * p.query_margin;
  // Standing out is not enough on its own at scale (issue #85): gibberish
  // stands out at up to 1.35 x noise, so the best margin must show as well.
  const bool answered = stands_out && best >= stats.stand_out_margin(t.query_margin) * p.query_margin;
  if (stands_out && stats.sd > 0) {
    // A short query clears few rows by the margin; the rows that stand out as
    // far as a match does are results as well (plan/17).
    const float result_z = t.result_z * p.result_z;
    std::set<std::pair<std::int64_t, std::int64_t>> have;
    for (const auto& h : hits) have.insert({h.asset, h.pts_ms});
    for (const auto& h : store.scan(query, allow, k, false, 0, stats.mean + result_z * stats.sd, nullptr, true)) {
      if (have.insert({h.asset, h.pts_ms}).second) hits.push_back(h);
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  }
  if (hits.empty()) return hits;
  // "Nothing found" (PR 20 calibration): a query that no top-ten row beats
  // the generic prompts by the margin describes nothing in the index, unless
  // its best assets stand out from the rest: a one-word subject ("dog") sits
  // close to "a photo." and misses the margin while ranking correctly
  // (2026-09-27, plan/17).
  if (gate && !answered && best < margin_needed) return {};
  if (z_rule && p.within > 0 && stats.sd > 0) {
    // Stricter: only what scores close to the best match, so the near-miss
    // category under a real one (the buses under "a truck") drops off.
    const float cutoff = hits.front().score - p.within * stats.sd;
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const vector_store::hit& h) { return h.score < cutoff; }),
               hits.end());
  }
  return hits;
}

float vector_store::scan_stats::log_scale() const noexcept {
  // Below 1,000 assets too: at 300 COCO photos "a dog" stands out with a best
  // margin of 0.014. From 100 up (smaller indexes use the margin alone).
  return static_cast<float>(std::log10(std::max(static_cast<double>(assets), 100.0) / 1000.0));
}

float vector_store::scan_stats::null_top10_z() const noexcept {
  if (assets < 10) return 0.0f;
  // The mean of the ten largest of n standard normals (Blom's positions).
  const double n = static_cast<double>(assets);
  double sum = 0;
  for (int i = 1; i <= 10; ++i) sum += normal_quantile(1.0 - (i - 0.375) / (n + 0.25));
  return static_cast<float>(sum / 10.0);
}

}  // namespace mv::ai
