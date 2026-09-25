// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "abi/clip_session.h"

#include <algorithm>
#include <cstring>


namespace mv::abi {
namespace clip = edit::clip;

namespace {

// Answers kept for mv_clip_index_get: the clip on screen and a couple the
// user just walked past.
constexpr std::size_t kKeptAnswers = 4;

void copy_str(char* dst, std::size_t cap, const std::string& s) noexcept {
  if (cap == 0) return;
  const std::size_t n = std::min(cap - 1, s.size());
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

}  // namespace

clip_session::clip_session(std::function<void(const mv_completion&)> push)
    : push_(std::move(push)), index_thread_([this] { index_worker(); }) {
  queue_.set_listener(on_job_event, this);
}

clip_session::~clip_session() {
  {
    std::lock_guard lock(index_mu_);
    stop_ = true;
  }
  index_cv_.notify_all();
  if (index_thread_.joinable()) index_thread_.join();
  // queue_ is destroyed after this body: it cancels and joins its worker,
  // whose last events still reach push_, which outlives it (declared first).
}

void clip_session::on_job_event(void* user, std::uint64_t id, clip::job_state state) noexcept {
  auto* self = static_cast<clip_session*>(user);
  mv_completion c{};
  c.kind = MV_COMPLETION_CLIP_JOB;
  c.job_id = id;
  c.payload = static_cast<std::int64_t>(state);
  c.status = MV_OK;
  if (state == clip::job_state::failed || state == clip::job_state::cancelled) {
    clip::job_snapshot snap;
    if (self->queue_.snapshot(id, snap)) c.status = static_cast<std::uint32_t>(snap.error);
  }
  if (self->push_) self->push_(c);
}

status clip_session::request_index(std::string path, std::uint64_t& out_id) {
  if (path.empty()) return status::invalid_arg;
  std::vector<std::uint64_t> superseded;
  {
    std::lock_guard lock(index_mu_);
    out_id = next_index_id_++;
    // Only the newest request matters: arrowing through ten clips must not
    // read ten indexes. Superseded ones are answered as cancelled.
    for (auto& [id, p] : index_pending_) {
      index_answer a;
      a.id = id;
      a.ready = true;
      a.result = status::cancelled;
      index_answers_.push_back(std::move(a));
      superseded.push_back(id);
    }
    index_pending_.clear();
    index_pending_.emplace_back(out_id, std::move(path));
    while (index_answers_.size() > kKeptAnswers) index_answers_.pop_front();
  }
  index_cv_.notify_one();
  for (const std::uint64_t id : superseded) {
    mv_completion c{};
    c.kind = MV_COMPLETION_CLIP_INDEX;
    c.job_id = id;
    c.status = static_cast<std::uint32_t>(status::cancelled);
    if (push_) push_(c);
  }
  return status::ok;
}

status clip_session::index_get(std::uint64_t id, std::int64_t* keyframes, std::uint32_t cap,
                               std::uint32_t* out_count, std::int64_t* out_duration) const {
  std::lock_guard lock(index_mu_);
  for (const index_answer& a : index_answers_) {
    if (a.id != id || !a.ready) continue;
    if (a.result != status::ok) return a.result;
    const auto n = static_cast<std::uint32_t>(a.keyframes.size());
    if (out_count) *out_count = n;
    if (out_duration) *out_duration = a.duration;
    if (keyframes != nullptr) std::copy_n(a.keyframes.begin(), std::min(n, cap), keyframes);
    return status::ok;
  }
  return status::invalid_arg;
}

void clip_session::index_worker() noexcept {
  while (true) {
    std::pair<std::uint64_t, std::string> next;
    {
      std::unique_lock lock(index_mu_);
      index_cv_.wait(lock, [this] { return stop_ || !index_pending_.empty(); });
      if (stop_) return;
      next = std::move(index_pending_.front());
      index_pending_.pop_front();
    }
    index_answer a;
    a.id = next.first;
    a.ready = true;
    auto info = clip::probe(next.second);
    if (info) {
      a.keyframes = std::move(info->keyframes_ns);
      a.duration = info->duration_ns;
    } else {
      a.result = info.error();
    }
    mv_completion c{};
    c.kind = MV_COMPLETION_CLIP_INDEX;
    c.job_id = a.id;
    c.status = static_cast<std::uint32_t>(a.result);
    c.payload = static_cast<std::int64_t>(a.keyframes.size());
    {
      std::lock_guard lock(index_mu_);
      index_answers_.push_back(std::move(a));
      while (index_answers_.size() > kKeptAnswers) index_answers_.pop_front();
    }
    if (push_) push_(c);
  }
}

bool clip_session::to_request(const mv_clip_request& in, std::string source, clip::request& out) noexcept {
  if (in.struct_size < sizeof(mv_clip_request)) return false;
  if (in.op < MV_CLIP_TRIM_KEYFRAME || in.op > MV_CLIP_ANIMATION) return false;
  clip::request r;
  r.kind = static_cast<clip::op>(in.op);
  r.source = std::move(source);
  r.in_ns = in.in_ns;
  r.out_ns = in.out_ns;
  switch (r.kind) {
    case clip::op::rotate: r.rotate_degrees = in.option == 2 ? 270 : in.option == 3 ? 180 : 90; break;
    case clip::op::remux: r.remux = in.option == 2 ? clip::remux_target::mkv : clip::remux_target::mp4; break;
    case clip::op::frame: r.frame = in.option == 2 ? clip::frame_format::jpeg : clip::frame_format::png; break;
    case clip::op::audio:
      r.audio = in.option == 2 ? clip::audio_format::wav
                : in.option == 3 ? clip::audio_format::flac
                                 : clip::audio_format::copy;
      break;
    case clip::op::animation:
      r.animation = in.option == 2 ? clip::anim_format::webp : clip::anim_format::gif;
      if (in.animation_width != 0) r.animation_width = in.animation_width;
      if (in.animation_fps != 0) r.animation_fps = in.animation_fps;
      break;
    default: break;
  }
  out = std::move(r);
  return true;
}

status clip_session::submit(std::string source, const mv_clip_request& req, std::uint64_t& out_id) {
  if (source.empty()) return status::invalid_arg;
  clip::request r;
  if (!to_request(req, std::move(source), r)) return status::invalid_arg;
  out_id = queue_.submit(std::move(r));
  return status::ok;
}

status clip_session::cancel(std::uint64_t id) noexcept {
  return queue_.cancel(id) ? status::ok : status::invalid_arg;
}

status clip_session::retry(std::uint64_t id, std::uint64_t& out_id) {
  out_id = queue_.retry(id);
  return out_id != 0 ? status::ok : status::invalid_arg;
}

status clip_session::jobs(std::uint64_t* ids, std::uint32_t cap, std::uint32_t* out_count) const {
  const std::vector<std::uint64_t> all = queue_.ids();
  if (out_count) *out_count = static_cast<std::uint32_t>(all.size());
  if (ids != nullptr) std::copy_n(all.begin(), std::min<std::size_t>(cap, all.size()), ids);
  return status::ok;
}

status clip_session::progress(std::uint64_t id, mv_clip_progress& out) const {
  clip::job_snapshot s;
  if (!queue_.snapshot(id, s)) return status::invalid_arg;
  out = mv_clip_progress{};
  out.job_id = s.id;
  out.state = static_cast<std::uint32_t>(s.state);
  out.op = static_cast<std::uint32_t>(s.kind);
  out.fraction = s.fraction;
  out.elapsed_ms = s.elapsed_ms;
  out.eta_ms = s.eta_ms;
  out.error = static_cast<std::uint32_t>(s.error);
  out.output_count = static_cast<std::uint32_t>(s.outputs.size());
  copy_str(out.title_utf8, sizeof(out.title_utf8), clip::job_title(s.kind, s.encoder));
  copy_str(out.source_name_utf8, sizeof(out.source_name_utf8), s.source_name);
  return status::ok;
}

status clip_session::output(std::uint64_t id, std::uint32_t index, char* utf8, std::uint32_t cap,
                            std::uint32_t* out_bytes) const {
  clip::job_snapshot s;
  if (!queue_.snapshot(id, s) || index >= s.outputs.size()) return status::invalid_arg;
  const std::string& p = s.outputs[index];
  if (out_bytes) *out_bytes = static_cast<std::uint32_t>(p.size() + 1);
  if (utf8 == nullptr || cap == 0) return status::invalid_arg;
  copy_str(utf8, cap, p);
  return status::ok;
}

void clip_session::clear_finished() noexcept { queue_.clear_finished(); }

}  // namespace mv::abi
