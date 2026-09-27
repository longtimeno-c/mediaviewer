// SPDX-License-Identifier: GPL-2.0-or-later
#include "addons/ai/vectors.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "addons/ai/index_db.h"

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
  alive_.clear();
  data_.clear();
  dead_ = 0;
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
  alive_.resize(w);
  data_.resize(w * dim_);
  dead_ = 0;
}

std::vector<vector_store::hit> vector_store::scan(std::span<const float> query,
                                                  const std::function<bool(std::int64_t)>& allow,
                                                  std::size_t k, bool use_margin, float min_margin,
                                                  float min_score) const {
  std::vector<hit> out;
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
  for (std::size_t r = 0; r < asset_.size(); ++r) {
    if (!alive_[r]) continue;
    if (asset_[r] != last_asset) {
      last_asset = asset_[r];
      last_ok = !allow || allow(last_asset);
    }
    if (!last_ok) continue;
    const float score =
        static_cast<float>(dot_i8(q.data(), data_.data() + r * dim_, dim_)) * qs * scale_[r];
    if (score < min_score) continue;
    if (use_margin && score - generic_[r] < min_margin) continue;
    all.push_back(hit{asset_[r], pts_[r], score, generic_[r]});
  }
  if (all.size() > k) {
    std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k), all.end(),
                     [](const hit& a, const hit& b) { return a.score > b.score; });
    all.resize(k);
  }
  std::sort(all.begin(), all.end(), [](const hit& a, const hit& b) { return a.score > b.score; });
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

}  // namespace mv::ai
