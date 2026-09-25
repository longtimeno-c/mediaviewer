// SPDX-License-Identifier: GPL-2.0-or-later
// Internals shared by the clip translation units (clip_*.cpp) and the tests.
// FFmpeg types appear here and in those .cpp files only; clip.h names none.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "core/result.h"
#include "edit/clip.h"

namespace mv::edit::clip::detail {

// ---- RAII ---------------------------------------------------------------------

struct input_deleter {
  void operator()(AVFormatContext* f) const noexcept { avformat_close_input(&f); }
};
using input_ptr = std::unique_ptr<AVFormatContext, input_deleter>;

struct output_deleter {
  void operator()(AVFormatContext* f) const noexcept {
    if (f == nullptr) return;
    if (f->pb != nullptr && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb);
    avformat_free_context(f);
  }
};
using output_ptr = std::unique_ptr<AVFormatContext, output_deleter>;

struct codec_deleter {
  void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
using codec_ptr = std::unique_ptr<AVCodecContext, codec_deleter>;

struct packet_deleter {
  void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
using packet_ptr = std::unique_ptr<AVPacket, packet_deleter>;

struct frame_deleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
using frame_ptr = std::unique_ptr<AVFrame, frame_deleter>;

inline constexpr AVRational kNs{1, 1'000'000'000};

// ---- input --------------------------------------------------------------------

struct source {
  input_ptr format;
  int video = -1;  // best video stream, -1 if none
  int audio = -1;  // best audio stream, -1 if none
  time_ns origin_ns = 0;  // container start_time, as player/video_source.cpp
  time_ns duration_ns = 0;
};

// Opens read-only with an interrupt callback on `cancel`, finds the streams.
[[nodiscard]] result<source> open_source(std::string_view utf8_path,
                                         const std::atomic<bool>* cancel);

// Stream timestamp <-> player timeline.
[[nodiscard]] time_ns to_timeline(const source& s, const AVStream* st, std::int64_t ts) noexcept;
[[nodiscard]] std::int64_t from_timeline(const source& s, const AVStream* st, time_ns t) noexcept;

// Seek so that the next video packet read is the keyframe at or before `t`.
// No-op at t <= 0.
void seek_before(source& s, time_ns t) noexcept;

// Clockwise display rotation of a stream (0/90/180/270).
[[nodiscard]] int stream_rotation(const AVStream* st) noexcept;

// ---- output -------------------------------------------------------------------

// Muxer name for a family, and the extension an output gets.
[[nodiscard]] const char* muxer_for_family(std::string_view family) noexcept;
[[nodiscard]] const char* extension_for_muxer(std::string_view muxer) noexcept;
// The source's own family ("mp4" for .mp4/.m4v, "mov" for .mov, ...), from
// the demuxer that opened it (magic bytes) plus the extension only to tell
// mp4 from mov and webm from mkv, which share a demuxer.
[[nodiscard]] std::string family_of(const source& s, std::string_view utf8_path);

// Hidden temp beside the final name; cleaned up unless published.
class staged_output {
 public:
  staged_output() = default;
  staged_output(std::string dir, std::string final_name);
  ~staged_output();
  staged_output(staged_output&&) noexcept;
  staged_output& operator=(staged_output&&) noexcept;
  staged_output(const staged_output&) = delete;
  staged_output& operator=(const staged_output&) = delete;

  [[nodiscard]] const std::string& temp_path() const noexcept { return temp_; }
  // Renames the temp to the first free "name", "name (2)", ... and returns
  // the published path. Never overwrites.
  [[nodiscard]] result<std::string> publish();
  void discard() noexcept;

 private:
  std::string dir_;
  std::string name_;
  std::string temp_;
  bool live_ = false;
};

// Opens an output context writing to `path` with `muxer`.
[[nodiscard]] result<output_ptr> open_output(const char* muxer, const std::string& path,
                                             const std::atomic<bool>* cancel);

// Copies the format-level metadata (creation_time, make/model) that a remux
// keeps; never adds anything.
void copy_metadata(const AVFormatContext* in, AVFormatContext* out) noexcept;

// Sets / replaces the display matrix on an output stream (clockwise degrees).
[[nodiscard]] bool set_rotation(AVStream* out, int clockwise) noexcept;

[[nodiscard]] inline bool cancelled(const std::atomic<bool>* c) noexcept {
  return c != nullptr && c->load(std::memory_order_relaxed);
}

// ---- operations (one per translation unit family) -------------------------------

struct segment {
  time_ns start_ns = 0;  // a keyframe (or 0)
  time_ns end_ns = -1;   // a keyframe, or < 0 for the end of the clip
};

enum class stream_pick : std::uint8_t { all_av, audio_only };

struct copy_spec {
  std::vector<segment> segments;  // written back to back into one file
  const char* muxer = nullptr;
  stream_pick pick = stream_pick::all_av;
  int rotation = -1;  // >= 0 replaces the video display matrix
};

// Stream copy (Path 1, rotate, split halves, remove-middle, remux, audio copy).
[[nodiscard]] expected copy_segments(std::string_view src_path, const std::string& out_path,
                                     const copy_spec& spec, const control& ctl,
                                     double progress_base, double progress_span);

struct reencode_spec {
  time_ns in_ns = 0;
  time_ns out_ns = -1;
  const char* muxer = nullptr;
  // FFmpeg encoder names to try in order.
  std::vector<std::string> encoders;
  // Tests only: accept an encoder FFmpeg does not mark hardware/hybrid. The
  // product path (run()) never sets it (plan/11).
  bool allow_software = false;
};

[[nodiscard]] result<std::string> reencode(std::string_view src_path, const std::string& out_path,
                                           const reencode_spec& spec, const control& ctl);

[[nodiscard]] result<std::vector<std::uint8_t>> frame_image(std::string_view src_path, time_ns at,
                                                            frame_format fmt, int jpeg_quality,
                                                            const control& ctl);

[[nodiscard]] expected audio_transcode(std::string_view src_path, const std::string& out_path,
                                       audio_format fmt, time_ns in_ns, time_ns out_ns,
                                       const control& ctl);

[[nodiscard]] expected animation(std::string_view src_path, const std::string& out_path,
                                 const request& req, const control& ctl);

// run() with the encoder list replaced (tests: a software encoder on Linux).
[[nodiscard]] result<outcome> run_with_encoders(const request& req, const control& ctl,
                                                std::vector<std::string> encoders,
                                                bool allow_software);

}  // namespace mv::edit::clip::detail
