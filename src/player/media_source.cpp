// SPDX-License-Identifier: GPL-2.0-or-later
// One render-thread owner applies transport intent; workers own all decoding.
#include "player/media_source.h"
#include "player/video_internal.h"
#include "player/transport.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include "io/paths.h"
namespace mv::player {
namespace {
class ffmpeg_media_source final : public media_source {
 public:
  ~ffmpeg_media_source() override {
    // The ABI retires sources on its cleanup worker. No disk I/O or thread join on present.
    if (video_) {
      save_resume(); close_video_source(video_);
    }
  }
  expected open(const char* path, void* device) {
    MV_TRY(auto* video, open_video_source(path, device));
    video_ = video; pipe_ = pipeline_of(video_);
    info_.video = video_->info(); info_.duration_ns = info_.video.duration_ns;
    for (unsigned i = 0; i < pipe_->format->nb_streams; ++i) {
      const auto type = pipe_->format->streams[i]->codecpar->codec_type;
      if (type == AVMEDIA_TYPE_AUDIO) ++info_.audio_tracks;
      if (type == AVMEDIA_TYPE_VIDEO) ++info_.video_tracks;
    }
    info_.has_audio = info_.audio_tracks > 0;
    path_ = path;
    load_resume();
    return {};
  }
  media_info info() const noexcept override {
    auto out = info_;
    const auto observed = pipe_->observed_decoder.load();
    if (observed) out.video.decoder = static_cast<decoder_kind>(observed);
    return out;
  }
  play_state state() const noexcept override { return state_; }
  bool needs_present() const noexcept override { return preview_ || state_ == play_state::playing; }
  time_ns position_ns() const noexcept override {
    return state_ == play_state::playing && !preview_ ? pipe_->clock.now_ns() : shown_pts_;
  }
  void play() noexcept override {
    if (state_ == play_state::ended) seek(0, true);
    state_ = play_state::playing;
    if (!preview_) pipe_->clock.set_paused(false);
  }
  void pause() noexcept override {
    pipe_->clock.set_paused(true); state_ = play_state::paused;
  }
  void seek(time_ns pts, bool exact) noexcept override {
    if (candidate_) { video_->release(candidate_); candidate_ = nullptr; }
    const auto target = std::clamp<time_ns>(pts, 0, info_.duration_ns > 0 ? info_.duration_ns - 1 : std::numeric_limits<time_ns>::max());
    pipe_->clock.set_paused(true);
    const auto generation = pipe_->generation.fetch_add(1) + 1;
    pipe_->exact_target_ns.store(exact ? target : -1);
    pipe_->audio_target_ns.store(target);
    pipe_->clock.seeked(target, generation);
    pipe_->seek_exact.store(exact);
    pipe_->seek_request_ns.store(target);
    shown_pts_ = target; preview_ = true; step_before_ = -1;
  }
  void step(int frames) noexcept override {
    if (!frames) return;
    pause();
    if (frames > 0) {
      // Forward is the cheap direction and must stay that way (transport.h:
      // "Forward is cheap: decode the next frame"). It used to seek — a
      // keyframe seek plus a decode forward, a whole GOP of work, and a codec
      // flush that threw away frames already sitting in the ring — to reach the
      // frame that was next in that same ring. Pausing does not stop the demux
      // and decode threads, so the following frames are already queued: take
      // one. The presenter's deadline is bypassed the same way a seek preview
      // does it, which is what makes a step land while paused.
      if (candidate_) { video_->release(candidate_); candidate_ = nullptr; }
      step_before_ = -1;
      preview_ = true;
    } else {
      const auto before = shown_pts_;
      // Backward genuinely is a seek: the frame we want is behind the decoder,
      // so it is a keyframe seek to at-or-before the current position and then
      // a walk forward to the last frame under `before` (see the step_before_
      // branch in acquire_frame). Keyframe rather than exact, because the walk
      // is what finds the frame, not the decoder's target. Bounded by the ring,
      // so a long GOP costs several vblanks rather than one long stall —
      // back-step is intentionally asynchronous.
      seek(std::max<time_ns>(0, before - 1), false); step_before_ = before;
    }
  }
  void set_rate(double rate) noexcept override {
    const auto position = position_ns();
    pipe_->rate.store(clamp_rate(rate)); pipe_->clock.set_rate(clamp_rate(rate));
    seek(position, true); // Flush queued PCM from the old tempo graph.
  }
  void set_volume(float volume) noexcept override { pipe_->clock.set_volume(volume); }
  void set_muted(bool muted) noexcept override { pipe_->clock.set_muted(muted); }
  void select_audio_track(std::uint32_t index) noexcept override {
    unsigned n = 0;
    for (unsigned i = 0; i < pipe_->format->nb_streams; ++i) {
      if (pipe_->format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      if (n++ == index) { pipe_->selected_audio.store(static_cast<int>(i)); seek(position_ns(), true); return; }
    }
  }
  void set_loop(time_ns a, time_ns b) noexcept override { loop_ = {std::max<time_ns>(0, a), b}; }
  video_frame* acquire_frame(std::uint32_t generation, time_ns vblank) noexcept override {
    (void)generation; // View ownership is enforced by the ABI; ring generations are per seek.
    const auto gen = pipe_->generation.load();
    // Drain obsolete frames even when the next old PTS is far in the future.
    (void)pipe_->ring.acquire(gen, std::numeric_limits<time_ns>::min());
    if (preview_) {
      time_ns next = 0;
      if (step_before_ >= 0) {
        while (video_->peek_next_pts(&next)) {
          if (next >= step_before_ && candidate_) break;
          auto* frame = video_->acquire(gen, std::numeric_limits<time_ns>::max());
          if (!frame) break;
          if (candidate_) video_->release(candidate_);
          candidate_ = frame;
          if (step_before_ == 0) break;
        }
        if (!candidate_ || (!pipe_->video_done.load() && (!video_->peek_next_pts(&next) || next < step_before_))) return nullptr;
      } else {
        candidate_ = video_->acquire(gen, std::numeric_limits<time_ns>::max());
        if (!candidate_) {
          // Nothing queued and nothing more coming: a forward step off the last
          // frame. Leave preview rather than sitting in it, or needs_present()
          // stays true and the lab presents every vblank forever on a clip that
          // has ended — the one thing PR 1's idle gate forbids.
          if (pipe_->video_done.load()) preview_ = false;
          return nullptr;
        }
      }
      auto* result = candidate_; candidate_ = nullptr;
      shown_pts_ = result->pts_ns; preview_ = false;

      pipe_->clock.set_paused(state_ != play_state::playing);
      return result;
    }
    if (state_ != play_state::playing) return nullptr;
    time_ns wrap = 0;
    if (loop_wrap(loop_, position_ns(), &wrap)) { seek(wrap, true); return nullptr; }
    presenter_input in;
    in.master_clock_ns = pipe_->clock.now_ns(); in.vblank_ns = vblank;
    in.playback_rate = pipe_->rate.load();
    for (unsigned i = 0; i <= frame_ring_slots; ++i) {
      in.has_next = video_->peek_next_pts(&in.next_pts_ns);
      if (!in.has_next && pipe_->video_done.load() &&
          (info_.duration_ns <= 0 || in.master_clock_ns >= info_.duration_ns)) {
        pipe_->clock.set_paused(true); state_ = play_state::ended; return nullptr;
      }
      auto decision = choose(in);
      if (decision.action == present_action::drop) {
        pipe_->ring.drop_oldest(); pipe_->clock.record_present(decision, false); continue;
      }
      video_frame* result = nullptr;
      if (decision.action == present_action::show) result = video_->acquire(gen, decision.target_ns);
      if (result) shown_pts_ = result->pts_ns;
      pipe_->clock.record_present(decision, result != nullptr);
      return result;
    }
    return nullptr;
  }
  void release_frame(video_frame* frame) noexcept override { video_->release(frame); }
  clock_stats stats() const noexcept override {
    auto out = pipe_->clock.stats(); out.decoder = info().video.decoder;
    out.video_pts_ns = shown_pts_;
    out.decode_errors = pipe_->decode_errors.load();
    out.surface_waits = pipe_->surface_waits.load();
    out.ring_backpressure = pipe_->ring_backpressure.load();
    return out;
  }
 private:
  std::filesystem::path resume_file() const {
    auto cache = io::thumb_cache_dir();
    if (!cache) return {};
    auto dir = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(cache.value().data()), cache.value().size())).parent_path() / "resume";
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    std::uint64_t key = 14695981039346656037ull;
    for (const unsigned char c : path_) { key ^= c; key *= 1099511628211ull; }
    return dir / (std::to_string(key) + ".txt");
  }
  void load_resume() {
    std::error_code ec;
    const auto path = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(path_.data()), path_.size()));
    file_size_ = std::filesystem::file_size(path, ec); if (ec) return;
    file_time_ = std::filesystem::last_write_time(path, ec).time_since_epoch().count(); if (ec) return;
    std::ifstream input(resume_file());
    std::string stored_path; std::getline(input, stored_path);
    std::uint64_t size = 0; std::int64_t stamp = 0; time_ns position = 0;
    if (input >> size >> stamp >> position && stored_path == path_ && size == file_size_ && stamp == file_time_) {
      const auto resume = resume_start_position(position, info_.duration_ns);
      if (resume > 0) seek(resume, true);
    }
  }
  void save_resume() const {
    if (path_.empty()) return;
    const auto file = resume_file(); if (file.empty()) return;
    // Use the last displayed PTS: the worker may retire us after navigation, when
    // the wall clock has already advanced beyond the frame the user actually saw.
    const auto position = should_store_resume(shown_pts_, info_.duration_ns) ? shown_pts_ : 0;
    std::ofstream output(file, std::ios::trunc);
    output << path_ << '\n' << file_size_ << ' ' << file_time_ << ' ' << position << '\n';
  }
  std::string path_;
  std::uint64_t file_size_ = 0;
  std::int64_t file_time_ = 0;
  video_source* video_ = nullptr;
  video_pipeline* pipe_ = nullptr;
  video_frame* candidate_ = nullptr;
  media_info info_{};
  play_state state_ = play_state::paused;
  time_ns shown_pts_ = 0, step_before_ = -1;
  bool preview_ = true;
  ab_loop loop_{};
};
}
result<media_source*> open_media(const char* path, void* device) {
  auto source = std::make_unique<ffmpeg_media_source>();
  MV_TRY_VOID(source->open(path, device));
  return static_cast<media_source*>(source.release());
}
void close_media(media_source* source) noexcept { delete source; }
}  // namespace mv::player
