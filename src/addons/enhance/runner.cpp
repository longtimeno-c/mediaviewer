// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/runner.h"

#include <algorithm>
#include <condition_variable>
#include <deque>

#include "addons/enhance/screens.h"
#include "addons/enhance/stream_model.h"

namespace mv::enhance {
namespace {

std::string name_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Why a run stopped, in the sheet's words. Never a path (rule 6 holds for
// the screen too: the sheet already names the file).
std::string why(status s) {
  switch (s) {
    case status::cancelled: return "Stopped. Nothing was written.";
    case status::unsupported_format: return "Enhance cannot read this file's sound.";
    case status::io: return "The new file could not be written. Check the folder has room and can be written to.";
    case status::out_of_memory: return "There was not enough memory to finish.";
    default: return "Enhance stopped before it finished. Nothing was written.";
  }
}

double strength_of(const job_request::part& p) {
  const auto it = p.values.find("strength");
  if (it == p.values.end() || it->second.k != json::kind::number) return 1.0;
  return std::clamp(it->second.is_integer ? static_cast<double>(it->second.i) : it->second.d, 0.0, 1.0);
}

std::uint32_t scale_of(const job_request::part& p) {
  const auto it = p.values.find("scale");
  if (it == p.values.end() || it->second.k != json::kind::string) return 2;
  return it->second.s == "1" ? 1 : it->second.s == "4" ? 4 : 2;
}

const job_request::part* part_of(const job_request& r, model_kind k) {
  for (const auto& p : r.parts) {
    if (p.kind == k && p.spec) return &p;
  }
  return nullptr;
}

// Sound made on one thread for a writer on another: at most `cap` frames
// waiting, so a long clip's sound is never all in memory.
class sound_queue {
 public:
  sound_queue(std::uint32_t channels, std::size_t cap) : ch_(channels), cap_(cap * channels) {}
  // Producer. false when the consumer has stopped.
  bool push(std::span<const float> v) {
    std::unique_lock lock(m_);
    room_.wait(lock, [&] { return stopped_ || q_.size() < cap_; });
    if (stopped_) return false;
    q_.insert(q_.end(), v.begin(), v.end());
    ready_.notify_all();
    return true;
  }
  void close(bool ok) {
    std::lock_guard lock(m_);
    done_ = true;
    ok_ = ok;
    ready_.notify_all();
  }
  void stop() {
    std::lock_guard lock(m_);
    stopped_ = true;
    room_.notify_all();
    ready_.notify_all();
  }
  // Consumer: up to `frames` frames, waiting for them unless the sound is done.
  std::vector<float> take(std::size_t frames) {
    std::unique_lock lock(m_);
    const std::size_t want = frames * ch_;
    ready_.wait(lock, [&] { return stopped_ || done_ || q_.size() >= std::min(want, cap_); });
    const std::size_t n = std::min(want, q_.size() - q_.size() % ch_);
    std::vector<float> out(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
    q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
    room_.notify_all();
    return out;
  }
  bool finished() {
    std::lock_guard lock(m_);
    return done_ && q_.empty();
  }
  bool ok() {
    std::lock_guard lock(m_);
    return ok_;
  }

 private:
  std::uint32_t ch_;
  std::size_t cap_;
  std::mutex m_;
  std::condition_variable ready_, room_;
  std::deque<float> q_;
  bool done_ = false, ok_ = true, stopped_ = false;
};

}  // namespace

job_runner::job_runner(run_port port) : port_(std::move(port)) {}

job_runner::~job_runner() {
  cancel();
}

bool job_runner::supports(model_kind k, item_kind item) const {
  // Sound, from an audio file or a clip (slice 4); a clip's picture (slice
  // 5). A still is a later slice.
  if (k == model_kind::audio) {
    return (item == item_kind::audio || item == item_kind::media) && port_.open_sound && port_.create &&
           port_.load_model;
  }
  if (k == model_kind::video) {
    return item == item_kind::media && port_.open_frames && port_.create_clip && port_.load_tiles;
  }
  if (k == model_kind::still) {
    return item == item_kind::still && port_.open_still && port_.write_still && port_.load_tiles;
  }
  return false;
}

std::string job_runner::check(const job_request& r) const {
  const job_request::part* video = part_of(r, model_kind::video);
  const job_request::part* audio = part_of(r, model_kind::audio);
  const job_request::part* still = part_of(r, model_kind::still);
  if (still) {
    if (!tile_shape_of(*still->spec)) return "This photo model is not one this version of Enhance can run.";
    video = still;  // the same runner, and its memory below
  }
  if (!video && !audio) return "There is nothing for Enhance to run.";
  if (audio && !stream_shape_of(*audio->spec)) return "This audio model is not one this version of Enhance can run.";
  if (video && !tile_shape_of(*video->spec)) return "This video model is not one this version of Enhance can run.";
  std::uint64_t need = 0;
  for (const auto* p : {video, audio}) {
    if (p) need += p->spec->run_bytes;
  }
  if (port_.free_memory) {
    if (const auto free = port_.free_memory(); free && *free < need) {
      return "Needs " + size_text(need) + " of free memory to run; " + size_text(*free) +
             " is free. Close something and try again.";
    }
  }
  return {};
}

void job_runner::start(job_request r, job_events events) {
  std::lock_guard lock(m_);
  if (t_.joinable()) t_.join();  // the last run has reported; its thread is done
  cancel_ = false;
  t_ = std::thread([this, r = std::move(r), events = std::move(events)]() mutable { run(std::move(r), std::move(events)); });
}

void job_runner::cancel() {
  cancel_ = true;
  std::thread t;
  {
    std::lock_guard lock(m_);
    t = std::move(t_);
  }
  // Joined off the lock: the job's last report (done) may come back here.
  if (t.joinable() && t.get_id() != std::this_thread::get_id()) t.join();
  if (t.joinable()) t.detach();
}

void job_runner::run(job_request r, job_events ev) {
  if (part_of(r, model_kind::still)) {
    run_still(std::move(r), std::move(ev));
  } else if (part_of(r, model_kind::video)) {
    run_picture(std::move(r), std::move(ev));
  } else {
    run_sound(std::move(r), std::move(ev));
  }
}

void job_runner::run_sound(job_request r, job_events ev) {
  const auto report = [&](expected e, const std::string& output) {
    if (ev.done) ev.done(static_cast<bool>(e), output, e ? std::string() : why(e.error()));
  };
  const job_request::part& part = r.parts.front();
  const auto shape = stream_shape_of(*part.spec);
  if (!shape) return report(err(status::unsupported_format), "");
  auto model = port_.load_model(*part.spec, part.models_dir);
  if (!model) return report(err(model.error() == status::out_of_memory ? status::out_of_memory : status::internal), "");
  auto reader = port_.open_sound(r.path, shape->rate);
  if (!reader) return report(err(reader.error() == status::cancelled ? status::cancelled : status::unsupported_format), "");
  auto writer = port_.create(r.path, shape->rate, reader->channels);
  if (!writer) return report(err(writer.error() == status::unsupported_format ? status::unsupported_format : status::io), "");

  sound_source src;
  src.channels = reader->channels;
  src.total_frames = reader->total_frames;
  src.read = reader->read;
  sound_sink_port sink;
  sink.write = writer->write;
  sound_job_options o;
  o.strength = strength_of(part);
  o.cancel = &cancel_;
  o.progress = ev.progress;
  const expected done = run_sound_job(src, **model, sink, o);
  if (!done) return report(done, "");
  if (cancel_.load()) return report(err(status::cancelled), "");
  auto path = writer->finish();
  if (!path) return report(err(path.error()), "");
  report({}, name_of(*path));
}

}  // namespace mv::enhance

namespace mv::enhance {

void job_runner::run_picture(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const auto failed = [&](status s) { fail(why(s)); };
  const job_request::part& video = *part_of(r, model_kind::video);
  const job_request::part* audio = part_of(r, model_kind::audio);
  const auto shape = tile_shape_of(*video.spec);
  if (!shape) return failed(status::unsupported_format);
  const std::uint32_t scale = std::min(scale_of(video), shape->scale);

  auto reader = port_.open_frames(r.path);
  if (!reader) return fail(reader.error() == status::cancelled ? why(status::cancelled)
                                                                : "Enhance cannot read this clip's picture.");
  if (reader->hdr) return fail("This is an HDR clip. Enhance cannot enhance HDR video yet.");
  const std::uint32_t ow = (reader->width * scale) & ~1u, oh = (reader->height * scale) & ~1u;
  if (std::max(ow, oh) > kMaxLongEdge || std::min(ow, oh) > kMaxShortEdge) {
    return fail("At this scale the new clip would be larger than 4K (" + std::to_string(ow) + " x " +
                std::to_string(oh) + "). Choose a smaller scale.");
  }
  auto tiles = port_.load_tiles(*video.spec, video.models_dir);
  if (!tiles) return failed(tiles.error() == status::out_of_memory ? status::out_of_memory : status::internal);

  // The sound, when it is enhanced too: on a thread of its own, through a
  // queue the frame loop drains as the picture's time advances.
  std::optional<run_port::reader> sound_in;
  std::unique_ptr<frame_model> sound_model;
  std::uint32_t rate = 0;
  if (audio) {
    const auto sshape = stream_shape_of(*audio->spec);
    if (!sshape) return failed(status::unsupported_format);
    rate = sshape->rate;
    auto rd = port_.open_sound(r.path, rate);
    if (!rd) return fail("Enhance cannot read this clip's sound.");
    auto m = port_.load_model(*audio->spec, audio->models_dir);
    if (!m) return failed(status::internal);
    sound_in = std::move(*rd);
    sound_model = std::move(*m);
  }
  auto writer = port_.create_clip(r.path, ow, oh, rate, sound_in ? sound_in->channels : 0);
  if (!writer) {
    return fail(writer.error() == status::unsupported_format
                    ? "This computer has no hardware video encoder Enhance can use at that size."
                    : why(status::io));
  }

  std::unique_ptr<sound_queue> queue;
  std::thread sound_thread;
  const auto stop_sound = [&] {
    if (queue) queue->stop();
    if (sound_thread.joinable()) sound_thread.join();
  };
  if (sound_in) {
    queue = std::make_unique<sound_queue>(sound_in->channels, rate * 5);
    sound_thread = std::thread([&, strength = strength_of(*audio)] {
      sound_source src;
      src.channels = sound_in->channels;
      src.total_frames = sound_in->total_frames;
      src.read = sound_in->read;
      sound_sink_port sink;
      sink.write = [&](std::span<const float> v) -> expected {
        return queue->push(v) ? expected{} : expected{err(status::cancelled)};
      };
      sound_job_options o;
      o.strength = strength;
      o.cancel = &cancel_;
      queue->close(static_cast<bool>(run_sound_job(src, *sound_model, sink, o)));
    });
  }
  std::uint64_t sound_written = 0;  // frames
  // The sound up to `until_us` on the clip's timeline (all of it when < 0).
  const auto feed_sound = [&](std::int64_t until_us) -> expected {
    if (!queue) return {};
    const std::uint64_t want = until_us < 0 ? UINT64_MAX : static_cast<std::uint64_t>(until_us) * rate / 1'000'000;
    while (sound_written < want && !queue->finished()) {
      if (cancel_.load()) return err(status::cancelled);
      const auto chunk = queue->take(static_cast<std::size_t>(std::min<std::uint64_t>(want - sound_written, rate / 10)));
      if (chunk.empty()) continue;
      MV_TRY_VOID(writer->sound(chunk));
      sound_written += chunk.size() / sound_in->channels;
    }
    return {};
  };

  picture_options po;
  po.out_scale = scale;
  po.overlap = shape->overlap;
  po.strength = strength_of(video);
  std::vector<std::uint8_t> frame, big;
  std::int64_t done = 0;
  const std::int64_t total = std::max<std::int64_t>(1, reader->total_frames);
  expected ok;
  while (ok) {
    if (cancel_.load()) {
      ok = err(status::cancelled);
      break;
    }
    std::int64_t pts = 0;
    auto more = reader->next(frame, pts);
    if (!more) {
      ok = err(more.error());
      break;
    }
    if (!*more) break;
    ok = enhance_picture(frame, reader->width, reader->height, **tiles, po, big);
    if (!ok) break;
    if (ow != reader->width * scale || oh != reader->height * scale) {
      // An odd size made even: the last column / row goes.
      std::vector<std::uint8_t> even(std::size_t{ow} * oh * 3);
      for (std::uint32_t y = 0; y < oh; ++y) {
        std::copy_n(big.data() + std::size_t{y} * reader->width * scale * 3, std::size_t{ow} * 3,
                    even.data() + std::size_t{y} * ow * 3);
      }
      big.swap(even);
    }
    ok = feed_sound(pts);
    if (!ok) break;
    ok = writer->frame(big.data(), ow * 3, pts);
    ++done;
    if (ev.progress) ev.progress(std::min(0.99, static_cast<double>(done) / static_cast<double>(total)));
  }
  if (ok) ok = feed_sound(-1);
  const bool sound_ok = !queue || (stop_sound(), queue->ok());
  stop_sound();
  if (!ok) return failed(ok.error());
  if (cancel_.load()) return failed(status::cancelled);
  if (!sound_ok) return fail("Enhance could not finish this clip's sound. Nothing was written.");
  auto path = writer->finish();
  if (!path) return failed(path.error());
  if (ev.done) ev.done(true, name_of(*path), "");
}

}  // namespace mv::enhance

namespace mv::enhance {

void job_runner::run_still(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const job_request::part& part = *part_of(r, model_kind::still);
  const auto shape = tile_shape_of(*part.spec);
  if (!shape) return fail(why(status::unsupported_format));
  const std::uint32_t scale = std::min(scale_of(part), shape->scale);
  if (ev.progress) ev.progress(-1);  // a RAW's develop takes a while, and says nothing
  auto pic = port_.open_still(r.path);
  if (!pic) {
    return fail(pic.error() == status::cancelled ? why(status::cancelled) : "Enhance cannot read this photo.");
  }
  if (cancel_.load()) return fail(why(status::cancelled));
  const std::uint64_t ow = std::uint64_t{pic->width} * scale, oh = std::uint64_t{pic->height} * scale;
  if (ow * oh > kMaxStillPixels || std::max(ow, oh) > kMaxStillEdge) {
    return fail("At this size the new photo would be " + std::to_string(ow * oh / 1'000'000) +
                " megapixels, more than Enhance writes (100). Choose a smaller size.");
  }
  // The picture in and out, both whole, on top of the model.
  const std::uint64_t need = part.spec->run_bytes + std::uint64_t{pic->width} * pic->height * 3 + ow * oh * 3;
  if (port_.free_memory) {
    if (const auto free = port_.free_memory(); free && *free < need) {
      return fail("This photo needs " + size_text(need) + " of free memory at this size; " + size_text(*free) +
                  " is free. Choose a smaller size, or close something and try again.");
    }
  }
  auto tiles = port_.load_tiles(*part.spec, part.models_dir);
  if (!tiles) return fail(why(tiles.error() == status::out_of_memory ? status::out_of_memory : status::internal));
  picture_options po;
  po.out_scale = scale;
  po.overlap = shape->overlap;
  po.strength = strength_of(part);
  po.cancel = &cancel_;
  po.progress = [&](double f) {
    if (ev.progress) ev.progress(std::min(0.99, f));
  };
  run_port::still out;
  out.width = static_cast<std::uint32_t>(ow);
  out.height = static_cast<std::uint32_t>(oh);
  const expected ok = enhance_picture(pic->rgb, pic->width, pic->height, **tiles, po, out.rgb);
  pic->rgb = {};  // the original's pixels go before the encode needs its own
  if (!ok) return fail(why(ok.error()));
  if (cancel_.load()) return fail(why(status::cancelled));
  auto path = port_.write_still(r.path, out);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

}  // namespace mv::enhance
