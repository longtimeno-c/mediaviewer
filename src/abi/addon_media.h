// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Host table v2's pixels and sound (addon/media.h's types) from the viewer's
// own decoders: the still first-pixel path, the clip sampler, the JPEG-512
// cache and an audio reader. The host's side of the add-on line, so it lives
// above image/ and edit/ (docs/design/02); abi/addon_abi.cpp (Windows) and
// shell/addons_mac.mm (Mac) install it into host_services. Portable: FFmpeg,
// no GPU, no OS UI. Worker threads only: every call reads and decodes.
#pragma once

#include <array>
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
#include "edit/eye_state.h"
#include "image/linear.h"

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
// The phone's catalogue: a smaller JPEG of a JPEG (the host table's jpeg_shrink).
// Bytes are decoded at a DCT scale and averaged; no I/O, any worker.
[[nodiscard]] result<std::vector<std::uint8_t>> jpeg_shrink(std::span<const std::uint8_t> jpeg, std::uint32_t max_edge,
                                                            std::uint32_t quality);
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
// docs/plans/edit-refresh.md S6: Extend's frame over that photo, the margins
// past its edges (left, top, right, bottom; fractions of the edited picture's
// width or height, the crop part already on the edit). An empty path clears
// it; still_extend answers all 0 for any other photo, and once the edit's
// geometry (set_still_edit) is no longer the one the frame was set over (an
// undo of Fill's crop, a later turn or crop): its fractions were of another
// picture.
void set_still_extend(const std::string& path, const std::array<float, 4>& ltrb);
[[nodiscard]] std::array<float, 4> still_extend(const std::string& path);
// docs/plans/edit-adjust.md slice 6: eyes open from the same moment.
// The frames of the same moment as `path`: the other files of its folder
// taken within kBurstSeconds of it (EXIF; for a photo without a capture
// stamp, written within it), then of the same occasion (kOccasionSeconds,
// by capture stamp only), nearest first, at most kBurstFrames. Two minutes:
// a group's run of shots, each donor still checked for its head's turn.
inline constexpr double kBurstSeconds = 120.0;
// With capture stamps, the frames of the same occasion too (taken within six
// hours: the same evening's photos of the same people, in the same clothes
// and light), nearest first after the moment's: the donors the Faces tool's
// fixes can use even when People has none at this face's pose (2026-11-07).
inline constexpr double kOccasionSeconds = 6.0 * 3600.0;
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
// docs/plans/edit-refresh.md S8: which faces a run changes. Bit i is face i of
// the pack's answer (its "faces" array, in order); a face whose bit is clear
// is left exactly as it was. The Faces tool sends the faces chosen on the
// canvas (faces_mask_of); every other caller every face.
inline constexpr std::uint64_t kAllFaces = ~std::uint64_t{0};
[[nodiscard]] result<eyes_outcome> open_closed_eyes(const std::string& path, std::string_view pack_json,
                                                    const job_context* ctx = nullptr,
                                                    std::uint64_t faces_mask = kAllFaces);
// Owner 2026-11-04, the wand menu's "Improve faces from your photos": for each
// face, the best donor of the same person (the pack's answer: the same moment
// and their other photos in People) turned as it is; when that face is clearly
// better (the pack's face quality, and open eyes over shut), the whole inner
// face from it (edit::transfer_face), in this photo's light. A new photo
// "<stem>-faces.jpg" beside the original; `fixed` counts the faces replaced.
[[nodiscard]] result<eyes_outcome> improve_faces(const std::string& path, std::string_view pack_json,
                                                 const job_context* ctx = nullptr,
                                                 std::uint64_t faces_mask = kAllFaces);
[[nodiscard]] std::string faces_note(status pack, const result<eyes_outcome>& r);

// S8: the Faces tool's fixes, one table both hosts and the C entry
// (mv_adjust_faces) share. The per-face actions are data: a fix is appended
// here (S10's "Restore faces" is the next value), with its row in
// shell/faces_tool.h kFaceActions and the chromes' labels. do_all runs, per
// chosen face, the best fix there is: a clearly better face of the same
// person, else open eyes where they are shut; one file, "<stem>-faces.jpg".
enum class face_fix : std::int32_t { do_all = -1, open_eyes = 0, improve = 1 };
// The fix's file suffix ("eyes", "faces").
[[nodiscard]] const char* face_fix_suffix(face_fix fix) noexcept;
// `fix` on the faces of `pack_json` whose bit is set in `faces_mask`, in
// `target` itself (the photo's working image, upright): no file. Every pixel
// outside the chosen faces' edit regions (edit::face_region) stays bit for bit
// what it was; inside, the fix fades to nothing at the region's edge. Donors
// are read through readable_path. Returns the faces fixed; `found` (may be
// null) the faces of the answer. invalid_arg for an answer that is not one.
[[nodiscard]] result<int> fix_faces_in(image::linear_image& target, std::string_view pack_json, face_fix fix,
                                       std::uint64_t faces_mask, const job_context* ctx = nullptr,
                                       int* found = nullptr);
// fix_faces_in on the photo at `path` (a Photos key too), then a new photo
// beside it, "<stem>-<suffix>.jpg", its metadata carried as write_still does.
// The original is never written. Nothing chosen, or nothing a fix helped: no
// file (`written` empty).
[[nodiscard]] result<eyes_outcome> fix_faces(const std::string& path, std::string_view pack_json, face_fix fix,
                                             std::uint64_t faces_mask, const job_context* ctx = nullptr);
// 2026-11-07 (owner: "detect closed eyes and fix them naturally"): YuNet's
// eye points can be a quarter of an eye distance off on a tilted face, on the
// brow or the cheek. A face is read again upright: a square of it, turned so
// its eye line is level, given back to the pack's detector (set_face_redetect:
// mv_ai_api faces_json), and its five points taken back to the picture. The
// points stay as they were when the pack is not there, the face is not found
// again, or the new eyes are not near the old.
using face_redetect_fn = std::function<result<std::string>(const std::uint8_t* rgb, std::uint32_t width,
                                                           std::uint32_t height, std::uint32_t stride)>;
void set_face_redetect(face_redetect_fn fn);
[[nodiscard]] edit::face_landmarks refine_landmarks(const image::linear_image& img, const edit::face_landmarks& lm);
// The pack's answer with each face's "lm" read again upright (refine_landmarks)
// and its eyes read (edit::read_eye_state): "eyes": [right, left], the
// probability each is open, -1 unknown. Donors are left as they are. The
// answer unchanged when it does not parse or `still` is not valid.
[[nodiscard]] std::string with_eyes_read(std::string_view pack_json, const image::linear_image& still);
// 2026-11-07: more donors for a fix. The pack's own pick from People reads
// only faces turned as this one is by YuNet's raw points, which miss on a
// tilted face; most of a person's photos never reach the host. So the fix
// asks the pack twice: once with the moment's frames, then again with the
// chosen faces' people's largest still photos (set_person_photos: the pack's
// person_faces_json) added as candidates, which the pack reads at any pose;
// the host then judges the pose on points read again upright. kPeoplePhotos a
// person, kPeoplePhotosAll in all. `boxes` as faces_mask_of's (empty: every
// face); `ask` is ai_eyes_json. The first answer when there is nothing to add
// or the second fails.
using person_photos_fn = std::function<result<std::string>(std::uint64_t person)>;
void set_person_photos(person_photos_fn fn);
inline constexpr std::size_t kPeoplePhotos = 12;
inline constexpr std::size_t kPeoplePhotosAll = 24;
using eyes_ask_fn = std::function<result<std::string>(const std::string& path, const std::string& candidates)>;
[[nodiscard]] result<std::string> eyes_answer(const std::string& path, const std::string& candidates,
                                              std::span<const float> boxes, const eyes_ask_fn& ask);
// The note for a fix's outcome (eyes_note / faces_note by the fix).
[[nodiscard]] std::string fix_note(face_fix fix, status pack, const result<eyes_outcome>& r);
// The faces of `pack_json` the canvas chose: `boxes` holds x, y, w, h (0..1
// of the upright still) of each chosen face, as the Faces tool read them from
// an earlier answer for the same photo (the run asks the pack again, with the
// same moment's frames). Each box takes the face of this answer it overlaps
// most (intersection over union at least 0.3), each face at most once; 0 when
// none match. Faces past the 64th are never chosen.
[[nodiscard]] std::uint64_t faces_mask_of(std::string_view pack_json, std::span<const float> boxes);
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
