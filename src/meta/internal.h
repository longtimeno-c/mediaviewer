// SPDX-License-Identifier: GPL-2.0-or-later
// Not part of the meta API: the two readers read() dispatches between.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "meta/meta.h"

namespace Exiv2 {
class Image;
}

namespace mv::meta::detail {

// Exiv2 over bytes in memory, metadata read. Initialises Exiv2's XMP parser
// once and mutes its log (its messages can name paths, rule 6). Null when
// Exiv2 does not know the format. Throws Exiv2's exceptions: callers catch.
[[nodiscard]] std::unique_ptr<Exiv2::Image> open_image(std::span<const std::uint8_t> bytes);

// Exiv2 over an in-memory prefix/whole file. Fills `out`; leaves it untouched
// on a file Exiv2 cannot parse. Never throws (Exiv2's exceptions stop here).
// `is_raw` is the decoder's own answer (codec::looks_like_raw) so that
// display_orientation matches what was actually drawn.
void read_still(std::span<const std::uint8_t> bytes, bool decoder_orients, metadata& out) noexcept;

// Date-taken only, for sort (a prefix is enough). nullopt when absent.
[[nodiscard]] std::optional<std::int64_t> still_date_key(std::span<const std::uint8_t> bytes) noexcept;

// libavformat: container facts, per-stream inspector, chapters, tags.
// Returns false when FFmpeg cannot open the file as a clip.
[[nodiscard]] bool read_clip(std::string_view utf8_path, metadata& out) noexcept;
[[nodiscard]] std::optional<std::int64_t> clip_date_key(std::string_view utf8_path) noexcept;

}  // namespace mv::meta::detail
