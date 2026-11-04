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
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/media.h"
#include "core/job_system.h"
#include "core/result.h"
#include "edit/edit_stack.h"

namespace mv::addon {

namespace media {
[[nodiscard]] result<rgb_image> decode_still(const std::string& path, std::uint32_t max_long_edge);
[[nodiscard]] result<std::unique_ptr<video_sampler>> open_sampler(const std::string& path,
                                                                  const sampler_options& options);
// A clip's frame at `pts_ms`; for a document (PDF, DOCX; #354) the page whose
// 0-based index is `pts_ms`, drawn by the viewer's renderer.
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
// #297 (Enhance): a file's sound at `sample_rate`, `channels` 1..8 or 0 for the
// source's own; and a new file beside a source with the add-on's sound
// (edit/sound_writer.h).
[[nodiscard]] result<std::unique_ptr<sound_stream>> open_sound(const std::string& path, std::uint32_t sample_rate,
                                                               std::uint32_t channels);
[[nodiscard]] result<std::unique_ptr<sound_sink>> open_sound_writer(const std::string& source,
                                                                    std::uint32_t sample_rate,
                                                                    std::uint32_t channels,
                                                                    const std::string& suffix);
// #297 slice 5: a clip's frames (edit/picture_io.h's frame_reader), and a new
// clip beside it from new frames (picture_writer, hardware encoders only).
[[nodiscard]] result<std::unique_ptr<picture_source>> open_frames(const std::string& path);
[[nodiscard]] result<std::unique_ptr<picture_sink>> open_clip_writer(const std::string& source, const picture_out& out);
// #297 slice 3: a photo at full resolution as the export path decodes it
// (edit::full_picture), and a new photo beside a source with its metadata
// carried (edit::export_pixels; Exiv2 for HEIC, TIFF, RAW, WebP), named
// "<stem>-<suffix>.jpg" (".png" for a PNG) under the first free name.
[[nodiscard]] result<rgb_image> full_still(const std::string& path);
// Owner 2026-11-04: edits of a Photos-library item ("photos:<id>", docs/design/26).
// The Mac host sets these at launch: `resolve` gives the item's file to read
// (the original, fetched from iCloud when only it has it; the host shows the
// fetch's progress, and `ctx`, when given, cancels it), and `add` puts a new
// file into the library as a new photo (nothing existing changes), removes
// the file, and returns the new item's key ("photos:<id>"); `source` is the
// key it was made from (the host lists the new photo beside it). Unset
// elsewhere: a key then reads as not found. Worker threads only.
using photos_resolve_fn = std::function<result<std::string>(const std::string& key, const job_context* ctx)>;
using photos_add_fn = std::function<result<std::string>(const std::string& file, const std::string& source)>;
void set_photos_hooks(photos_resolve_fn resolve, photos_add_fn add);
// The file to read for `path`: itself, or a Photos key's file.
[[nodiscard]] result<std::string> readable_path(const std::string& path, const job_context* ctx = nullptr);
[[nodiscard]] bool is_photos_key(std::string_view path) noexcept;
// S0 (docs/plans/edit-refresh.md §5): every writer of a library item's result
// goes through these two. The folder a result is written to first (ours,
// under the temporary folder; never beside a library file), ending in a
// separator; and the hand-over: `file` (in that folder) becomes a new photo
// made from `source`, and is gone afterwards whatever the outcome. Returns the
// new item's key. unsupported_format with no hook.
[[nodiscard]] result<std::string> staging_dir();
[[nodiscard]] result<std::string> add_to_library(const std::string& file, const std::string& source);
// Save copy of a library item (the hosts' Ctrl+S / ⌘S): `source`'s file
// exported by `exporter` into staging_dir(), then added. `exporter(file,
// out_dir)` returns the path it wrote.
using library_exporter = std::function<result<std::string>(const std::string& file, const std::string& out_dir)>;
[[nodiscard]] result<std::string> save_copy_to_library(const std::string& source, const library_exporter& exporter,
                                                       const job_context* ctx = nullptr);
// What a note says of a written result: its file name, or, for a library
// item's key, that it is in the library (a key is never shown, rule 6).
[[nodiscard]] std::string result_name(const std::string& written);
// docs/plans/edit-adjust.md slice 5: the edit of the photo being edited on
// screen, which full_still then applies (Enhance's copy carries the crop,
// colour and detail). The UI thread sets it as the edit changes; clear() when
// nothing is being edited. A mutex guards a copy of a few hundred bytes; it is
// never held across the decode.
void set_still_edit(const std::string& path, const edit::geometry& g, const edit::colour& c);
void clear_still_edit();
// Slice 7: the Remove brush over that photo, as u, v, r triples in the edited
// picture full_still answers with (empty: nothing brushed). The UI thread
// sets it before Enhance's sheet opens; still_mask answers the add-on.
void set_still_mask(const std::string& path, std::vector<float> dabs);
[[nodiscard]] std::vector<float> still_mask(const std::string& path);
// docs/plans/edit-adjust.md slice 6: eyes open from the same moment.
// The frames of the same moment as `path`: the other files of its folder
// modified within kBurstSeconds of it, nearest first, at most kBurstFrames.
inline constexpr double kBurstSeconds = 10.0;
inline constexpr std::size_t kBurstFrames = 12;
[[nodiscard]] std::vector<std::string> burst_neighbours(const std::string& path);
// What Open closed eyes did.
struct eyes_outcome {
  int faces = 0;        // faces the pack found in the photo
  int fixed = 0;        // of them, given a donor's eyes
  std::string written;  // the new file's path, empty when nothing was fixed
};
// For each face of the pack's eyes_json answer (mv_ai_api) whose eyes are
// less open than a donor's by kEyesBetter, the most open donor's eyes
// (edit/eyes.h), at full resolution; then a new photo beside the original,
// "<stem>-eyes.jpg", its metadata carried as write_still does. The original
// is never written. Worker only.
// The pane's note for an outcome, or for the pack's refusal (`pack` the
// status of ai_eyes_json: not_found no pack, invalid_arg People off).
[[nodiscard]] std::string eyes_note(status pack, const result<eyes_outcome>& r);
[[nodiscard]] result<eyes_outcome> open_closed_eyes(const std::string& path, std::string_view pack_json,
                                                    const job_context* ctx = nullptr);
// Owner 2026-11-04, the wand menu's "Improve faces from your photos": for each
// face, the best donor of the same person (the pack's answer: the same moment
// and their other photos in People) turned as it is; when that face is clearly
// better (the pack's face quality, and open eyes over shut), the whole inner
// face from it (edit::transfer_face), in this photo's light. A new photo
// "<stem>-faces.jpg" beside the original; `fixed` counts the faces replaced.
[[nodiscard]] result<eyes_outcome> improve_faces(const std::string& path, std::string_view pack_json,
                                                 const job_context* ctx = nullptr);
[[nodiscard]] std::string faces_note(status pack, const result<eyes_outcome>& r);
[[nodiscard]] result<std::string> write_still(const std::string& source, const rgb_image& pixels,
                                              const std::string& suffix);
// #354: a document's text in reading order with the page each paragraph
// starts on: a DOCX through codec::docx_text. unsupported_format for anything
// else (PDF text is the AI pack's own, through PDFium). `visit` false stops
// the read (status::cancelled). Worker threads: it lays the document out.
[[nodiscard]] expected document_text(const std::string& path,
                                     const std::function<bool(const document_line&)>& visit);
// #606: a clip's GPS route from its telemetry (meta::read_route), at most
// `max_points`, for the host table's route_of. Empty for a still or a clip
// without a fix. Worker threads: it reads every telemetry packet.
[[nodiscard]] result<std::vector<mv_addon_route_point>> route(const std::string& path, std::uint32_t max_points);
}  // namespace media

}  // namespace mv::addon
