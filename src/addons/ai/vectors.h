// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search matrix (docs/design/17 "Search"): every stored frame of the model that
// answers queries, int8 with a per-row scale, contiguous, scanned brute force.
// No ANN index until a measured p95 says brute force is too slow (docs/design/17);
// tools/ai-bench measures it at 100 k frames for the PR 22 verify.
//
// Rows are appended as the indexer commits and tombstoned when an asset is
// dropped; compaction rebuilds the arrays when a quarter are dead. Readers
// (queries) share the lock; the indexer's appends take it briefly.
//
// A deviation from docs/design/17's "mapped, not copied": the rows live in memory
// (0.5-0.75 KB a frame), loaded from index.db at start. At the 100 k frames
// the verify names that is 50-77 MB, inside docs/design/02's budget for a feature
// that is off by default; a mapped file is the upgrade if a library of
// millions of frames needs it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <span>
#include <vector>

namespace mv::ai {

class vector_store {
 public:
  explicit vector_store(std::uint32_t dim = 0) : dim_(dim) {}

  void reset(std::uint32_t dim);
  void clear();  // every row, keeping the dimension and the labels
  [[nodiscard]] std::uint32_t dim() const noexcept { return dim_; }
  [[nodiscard]] std::size_t rows() const;
  [[nodiscard]] std::size_t live_rows() const;

  void add(std::int64_t asset, std::int64_t pts_ms, float generic, float scale,
           std::span<const std::int8_t> q);
  void remove_asset(std::int64_t asset);

  struct hit {
    std::int64_t asset = 0;
    std::int64_t pts_ms = -1;
    float score = 0;     // cosine
    float generic = 0;   // the row's best generic-prompt cosine
  };
  // How far a query's best assets stand out from the rest of the allowed
  // index: the mean of the ten best per-asset scores, in standard deviations
  // of every asset's best score. Per asset, so a long clip's many similar
  // frames count once. Too few assets to say: `assets` < kMinAssets, z = 0.
  struct scan_stats {
    static constexpr std::size_t kMinAssets = 30;
    std::size_t assets = 0;
    float top10_z = 0;
    float mean = 0;  // of the per-asset best scores
    float sd = 0;
    // What top10_z is for a query that matches nothing (the ten best of
    // `assets` draws of noise): it grows with the library, ~2.2 at 300 assets,
    // 2.6 at 1,000, 3.3 at 10,000, so a fixed z threshold stops meaning
    // "stands out" as the index grows (docs/design/17, 2026-09-27).
    [[nodiscard]] float null_top10_z() const noexcept;
    // The "nothing found" rule on these stats (docs/design/17; recalibrated at 1,000
    // to 25,000 COCO photos, both towers, issue #85). A real library's best
    // scores have a heavier tail than noise, so gibberish sits at 0.9-1.35 x
    // noise's z and real subjects at 1.15-1.8: z alone cannot tell them apart,
    // nor can the margin, and both drift up with the library. A query passes
    // on its best margin alone at margin_alone(), or when it stands out at
    // model.json's query_z or 18 % over noise (whichever is higher) with a
    // best margin of stand_out_margin(). Both margins move with log10 of the
    // assets over 1,000 (from 100 assets up).
    static constexpr float kStandOutOverNoise = 1.18f;
    [[nodiscard]] float log_scale() const noexcept;  // log10(assets / 1,000), >= -1
    [[nodiscard]] float margin_alone(float query_margin) const noexcept {
      return query_margin * (1.375f + 0.125f * log_scale());
    }
    [[nodiscard]] float stand_out_margin(float query_margin) const noexcept {
      return query_margin * 0.375f * (1.0f + log_scale());
    }
    // Assets whose best row beats the generic prompts by the calibrated query
    // margin (find_text fills it; scan() leaves 0). A subject that fills much
    // of the library cannot stand out from it: its z sits below noise's
    // because it is the mean (2026-09-28, the owner's "mountain": 34 of 503
    // assets over the margin, z 1.75 against noise's 2.40, best margin 0.059
    // against the 0.06 that z asked for). Nonsense had 0-3 such assets of
    // 503 (0.6 %), real subjects 5-78: at least 5, and 1 % of the assets, as
    // lucky rows grow with the library.
    std::size_t over_margin = 0;
    [[nodiscard]] std::size_t broad_assets() const noexcept { return std::max<std::size_t>(5, assets / 100); }
    [[nodiscard]] bool stands_out(float query_z, float over_noise = kStandOutOverNoise) const noexcept {
      return query_z > 0 && top10_z >= std::max(query_z, over_noise * null_top10_z());
    }
    // What the best margin must reach for a query that does not stand out.
    // Too few assets to say, or a subject in 1 % of the library ("mountain"),
    // the calibrated margin; else margin_alone.
    [[nodiscard]] float margin_needed(float query_margin) const noexcept {
      const bool broad = assets < kMinAssets || over_margin >= broad_assets();
      return broad ? query_margin : margin_alone(query_margin);
    }
  };
  // The `k` best rows whose asset passes `allow` (may be empty: all), with
  // score - generic >= min_margin when `use_margin`, and score >= min_score.
  // With `use_labels`, only rows the query scores at least like their
  // label bar (set_labels); a row whose bar is not known yet passes.
  [[nodiscard]] std::vector<hit> scan(std::span<const float> query,
                                      const std::function<bool(std::int64_t)>& allow,
                                      std::size_t k, bool use_margin, float min_margin,
                                      float min_score, scan_stats* stats = nullptr,
                                      bool use_labels = false) const;
  // The label vocabulary (vocabulary.h) embedded by this store's tower:
  // `count` unit vectors of dim() floats. Each row's bar is the score of its
  // (kLabelsAbove + 1)-th best label; rows added from now on get theirs as
  // they come, and the rows already here are filled in chunks, so searches
  // and the indexer never wait on the whole pass (~50 us a row). reset()
  // drops the labels.
  void set_labels(std::span<const float> vocab, std::size_t count);
  [[nodiscard]] bool has_labels() const;
  // Every live row of one asset, in time order (a clip's matches).
  [[nodiscard]] std::vector<hit> rows_of(std::int64_t asset, std::span<const float> query) const;
  // The stored vector of a row nearest `pts_ms` in `asset`, dequantised.
  [[nodiscard]] bool vector_of(std::int64_t asset, std::int64_t pts_ms, std::vector<float>& out) const;

 private:
  void compact_locked();
  [[nodiscard]] float label_bar_locked(std::size_t row) const noexcept;

  mutable std::shared_mutex m_;
  std::uint32_t dim_ = 0;
  std::vector<std::int64_t> asset_;
  std::vector<std::int64_t> pts_;
  std::vector<float> generic_;
  std::vector<float> scale_;
  std::vector<float> bar_;  // per row: the label bar, NaN until known
  std::vector<std::int8_t> labels_;  // label count * dim, int8
  std::vector<float> label_scale_;
  std::uint64_t layout_ = 0;  // bumped when rows move (compaction, reset)
  std::vector<std::uint8_t> alive_;
  std::vector<std::int8_t> data_;  // rows * dim
  std::size_t dead_ = 0;
};

// Settings -> Local search -> Precision (docs/design/17 "Precision scale
// (2026-09-27)"): 0 broader, 2 the calibrated rule above, 4 stricter. Read by
// each search as it starts; nothing is re-indexed.
inline constexpr std::uint32_t kPrecisionLevels = 5;
inline constexpr std::uint32_t kPrecisionDefault = 2;

// What one Precision level does to the rule. Level 2 is every factor at 1 (and
// the calibrated stand-out, no cutoff): exactly the rule without the setting.
struct precision_scale {
  float stand_out = vector_store::scan_stats::kStandOutOverNoise;  // x noise's top-ten z
  float query_margin = 1.0f;   // x the margin a query needs (margin_needed)
  float result_margin = 1.0f;  // x the margin a row needs
  float result_z = 1.0f;       // x the z a row needs once the query stands out
  // Stricter levels: only rows within this many SDs (of every asset's best
  // score) of the best row, so a near-miss category under a real one drops
  // off. 0: no cutoff.
  float within = 0.0f;
  [[nodiscard]] static precision_scale at(std::uint32_t level) noexcept;
};

// A tower's calibrated thresholds (model.json). query_z 0: the margin rule
// alone, with no noise scaling (CLAP, until it has its own z calibration).
struct text_thresholds {
  float query_margin = 0.04f;
  float result_margin = 0.015f;
  float query_z = 2.5f;
  float result_z = 2.0f;
};

// A text query's rows from an index, best first (engine::search_text, and the
// calibration test runs the same function): the rows that beat the
// generic prompts by result_margin, and once the query stands out from the
// index (scan_stats) the rows as far above the mean as a match is. None when
// the query describes nothing here ("nothing found"); `gate` false skips that
// test (a person's own photos). At most `k` rows per pass.
[[nodiscard]] std::vector<vector_store::hit> find_text(const vector_store& store, std::span<const float> query,
                                                       const std::function<bool(std::int64_t)>& allow,
                                                       const text_thresholds& t, const precision_scale& p,
                                                       bool gate, std::size_t k = 5000);

// int8 . int8 over `n` (exposed for the benchmark and tests).
[[nodiscard]] std::int32_t dot_i8(const std::int8_t* a, const std::int8_t* b, std::size_t n) noexcept;

}  // namespace mv::ai
