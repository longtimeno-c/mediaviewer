// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b — the A/V drift soak, as a CSV-producing mode of the present lab.
//
// A CSV rather than a screenshot because the verify line is "drift flat over 30
// minutes, with the overlay to prove it": a graph someone can re-plot is proof,
// an ImGui screenshot is an assertion. The declaration lives here, owned by the
// shell; the implementation is av_soak.cpp and belongs to 5b.
#pragma once

#include <cstdint>

namespace mv::shell {

struct av_soak_options {
  const char*   clip_utf8   = nullptr;
  std::uint32_t seconds     = 0;
  const char*   csv_out_utf8 = nullptr;
};

// Returns process exit code: 0 pass, non-zero fail. A run whose stats carry
// position discontinuities or host-clock gaps (the machine slept, or power
// throttled) must FAIL rather than average over them — same honesty as
// gfx::pacer refusing its gate on discontinuities.
[[nodiscard]] int run_av_soak(const av_soak_options& options);

}  // namespace mv::shell
