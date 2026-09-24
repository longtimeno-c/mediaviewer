// SPDX-License-Identifier: GPL-2.0-or-later
// Not part of the meta API: the two readers read() dispatches between.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "meta/meta.h"

namespace mv::meta::detail {

// Exiv2 over an in-memory prefix/whole file. Fills `out`; leaves it untouched
// on a file Exiv2 cannot parse. Never throws (Exiv2's exceptions stop here).
// `is_raw` is the decoder's own answer (codec::looks_like_raw) so that
// display_orientation matches what was actually drawn.
void read_still(std::span<const std::uint8_t> bytes, bool is_raw, metadata& out) noexcept;

// Date-taken only, for sort (a prefix is enough). nullopt when absent.
[[nodiscard]] std::optional<std::int64_t> still_date_key(std::span<const std::uint8_t> bytes) noexcept;

// libavformat: container facts, per-stream inspector, chapters, tags.
// Returns false when FFmpeg cannot open the file as a clip.
[[nodiscard]] bool read_clip(std::string_view utf8_path, metadata& out) noexcept;
[[nodiscard]] std::optional<std::int64_t> clip_date_key(std::string_view utf8_path) noexcept;

}  // namespace mv::meta::detail
