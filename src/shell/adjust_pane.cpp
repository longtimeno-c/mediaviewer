// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/adjust_pane.h"

#include <cstring>

namespace mv::shell {

std::optional<std::uint64_t> adjust_pane::want(bool has_colour) {
  if (item_ == 0) {
    readiness_ = adjust_readiness::none;
    return std::nullopt;
  }
  if (!visible_ && !has_colour) return std::nullopt;
  if (readiness_ == adjust_readiness::preparing || readiness_ == adjust_readiness::ready ||
      readiness_ == adjust_readiness::failed) {
    return std::nullopt;  // already building, built, or known to fail for this item
  }
  readiness_ = adjust_readiness::preparing;
  token_ = next_token_++;
  return token_;
}

std::optional<std::uint64_t> adjust_pane::set_item(std::uint64_t item, bool has_colour) {
  if (item != item_) {
    item_ = item;
    readiness_ = adjust_readiness::none;
    token_ = 0;
    from_raw_ = false;
    histogram_stale_ = true;
    histogram_valid_ = false;
    histogram_token_ = 0;
  }
  return want(has_colour);
}

std::optional<std::uint64_t> adjust_pane::toggle(bool has_colour) {
  return show(!visible_, has_colour);
}

std::optional<std::uint64_t> adjust_pane::show(bool visible, bool has_colour) {
  visible_ = visible;
  if (visible_) histogram_stale_ = true;
  return want(has_colour);
}

std::optional<std::uint64_t> adjust_pane::colour_changed(bool has_colour) {
  histogram_stale_ = true;
  return want(has_colour);
}

bool adjust_pane::working_landed(std::uint64_t token, bool ok, bool from_raw) noexcept {
  if (token == 0 || token != token_ || readiness_ != adjust_readiness::preparing) return false;
  readiness_ = ok ? adjust_readiness::ready : adjust_readiness::failed;
  from_raw_ = ok && from_raw;
  histogram_stale_ = true;
  return ok;
}

void adjust_pane::working_dropped() noexcept {
  if (readiness_ == adjust_readiness::ready || readiness_ == adjust_readiness::preparing) {
    readiness_ = adjust_readiness::none;
  }
  token_ = 0;
  histogram_valid_ = false;
}

std::optional<std::uint64_t> adjust_pane::take_histogram_request() noexcept {
  if (!visible_ || readiness_ != adjust_readiness::ready || !histogram_stale_) return std::nullopt;
  histogram_stale_ = false;
  histogram_token_ = next_token_++;
  return histogram_token_;
}

bool adjust_pane::histogram_landed(std::uint64_t token, const edit::histogram& h) noexcept {
  if (token == 0 || token != histogram_token_) return false;
  const auto packed = edit::pack_histogram(h);
  std::memcpy(bins_, packed.data(), sizeof bins_);
  clip_high_ = h.high_fraction();
  clip_low_ = h.low_fraction();
  histogram_valid_ = true;
  return true;
}

adjust_view adjust_pane::view(const edit::colour& c) const noexcept {
  adjust_view v;
  v.readiness = static_cast<std::int32_t>(readiness_);
  v.from_raw = from_raw_ ? 1 : 0;
  for (int i = 0; i < edit::kAdjustParamCount; ++i) {
    v.values[i] = c.get(static_cast<edit::adjust_param>(i));
  }
  v.histogram_valid = histogram_valid_ ? 1 : 0;
  if (histogram_valid_) {
    v.clip_high = clip_high_;
    v.clip_low = clip_low_;
    std::memcpy(v.bins, bins_, sizeof v.bins);
  }
  return v;
}

}  // namespace mv::shell
