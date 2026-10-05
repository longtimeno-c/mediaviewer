// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Host table v2's pixels and sound (addon/media.h's types) from the viewer's
// own decoders: the still first-pixel path, the clip sampler, the JPEG-512
// cache and an audio reader. The host's side of the add-on line, so it lives
// above image/ and edit/ (docs/design/02); abi/addon_abi.cpp (Windows) and
// shell/addons_mac.mm (Mac) install it into host_services. Portable: FFmpeg,
// no GPU, no OS UI. Worker threads only: every call reads and decodes.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "addon/media.h"
#include "core/result.h"

namespace mv::addon {

namespace media {
[[nodiscard]] result<rgb_image> decode_still(const std::string& path, std::uint32_t max_long_edge);
[[nodiscard]] result<std::unique_ptr<video_sampler>> open_sampler(const std::string& path,
                                                                  const sampler_options& options);
[[nodiscard]] result<rgb_image> video_frame(const std::string& path, std::int64_t pts_ms,
                                            std::uint32_t max_long_edge);
// The JPEG-512 cache entry for a moment: stores `image` when given, else
// looks one up (status::io on a miss). Returns the JPEG's path.
[[nodiscard]] result<std::string> moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                                   const rgb_image* image);
// The JPEG-512 bytes (cache spec) of the frame on screen at `pts_ms`: one
// software decode from the keyframe before it. For a host that keeps the row
// in its own store under image::moment_thumb_key (a result list's clip tile
// whose moment the pack has not thumbed). `cancel` is polled when given.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_moment_thumb(
    const std::string& path, std::int64_t pts_ms, const std::atomic<bool>* cancel = nullptr);
// Sharing an index (docs/design/17): the cached JPEG-512 of a still (pts_ms < 0)
// or a moment as bytes, looked up only (status::io on a miss); and bytes made
// on another machine stored under this machine's stamp of the file. The bytes
// are untrusted: they are decoded, and only a JPEG whose long edge is at most
// 512 is stored.
[[nodiscard]] result<std::vector<std::uint8_t>> thumbnail_jpeg(const std::string& path,
                                                               std::int64_t pts_ms);
[[nodiscard]] expected store_thumbnail_jpeg(const std::string& path, std::int64_t pts_ms,
                                            std::span<const std::uint8_t> jpeg);
[[nodiscard]] result<std::unique_ptr<audio_stream>> open_audio(const std::string& path,
                                                               std::uint32_t sample_rate,
                                                               std::int64_t start_ms);
}  // namespace media

}  // namespace mv::addon
