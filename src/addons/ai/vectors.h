// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search matrix (plan/17 "Search"): every stored frame of the model that
// answers queries, int8 with a per-row scale, contiguous, scanned brute force.
// No ANN index until a measured p95 says brute force is too slow (plan/17);
// tools/ai-bench measures it at 100 k frames for the PR 22 verify.
//
// Rows are appended as the indexer commits and tombstoned when an asset is
// dropped; compaction rebuilds the arrays when a quarter are dead. Readers
// (queries) share the lock; the indexer's appends take it briefly.
//
// A deviation from plan/17's "mapped, not copied": the rows live in memory
// (0.5-0.75 KB a frame), loaded from index.db at start. At the 100 k frames
// the verify names that is 50-77 MB, inside plan/02's budget for a feature
// that is off by default; a mapped file is the upgrade if a library of
// millions of frames needs it.
#pragma once

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
  };
  // The `k` best rows whose asset passes `allow` (may be empty: all), with
  // score - generic >= min_margin when `use_margin`, and score >= min_score.
  [[nodiscard]] std::vector<hit> scan(std::span<const float> query,
                                      const std::function<bool(std::int64_t)>& allow,
                                      std::size_t k, bool use_margin, float min_margin,
                                      float min_score, scan_stats* stats = nullptr) const;
  // Every live row of one asset, in time order (a clip's matches).
  [[nodiscard]] std::vector<hit> rows_of(std::int64_t asset, std::span<const float> query) const;
  // The stored vector of a row nearest `pts_ms` in `asset`, dequantised.
  [[nodiscard]] bool vector_of(std::int64_t asset, std::int64_t pts_ms, std::vector<float>& out) const;

 private:
  void compact_locked();

  mutable std::shared_mutex m_;
  std::uint32_t dim_ = 0;
  std::vector<std::int64_t> asset_;
  std::vector<std::int64_t> pts_;
  std::vector<float> generic_;
  std::vector<float> scale_;
  std::vector<std::uint8_t> alive_;
  std::vector<std::int8_t> data_;  // rows * dim
  std::size_t dead_ = 0;
};

// int8 . int8 over `n` (exposed for the benchmark and tests).
[[nodiscard]] std::int32_t dot_i8(const std::int8_t* a, const std::int8_t* b, std::size_t n) noexcept;

}  // namespace mv::ai
