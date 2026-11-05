// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/runner.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>

#include "addons/enhance/brighten.h"
#include "addons/enhance/preview.h"
#include "addons/enhance/screens.h"
#include "addons/enhance/stream_model.h"

namespace mv::enhance {
namespace {

// What `done` reports: a file's name, or a Photos-library photo's key whole
// (S0: the host added the result to the library; the engine says so and
// never shows the key).
std::string name_of(const std::string& path) {
  if (path.rfind("photos:", 0) == 0) return path;
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
    // A library photo's result could not be added (S0): Photos access.
    case status::permission_denied:
      return "MediaViewer may not add photos to your Photos library. Allow it in System Settings, "
             "Privacy & Security, Photos.";
    default: return "Enhance stopped before it finished. Nothing was written.";
  }
}

double strength_of(const job_request::part& p) {
  const auto it = p.values.find("strength");
  if (it == p.values.end() || it->second.k != json::kind::number) return 1.0;
  return std::clamp(it->second.is_integer ? static_cast<double>(it->second.i) : it->second.d, 0.0, 1.0);
}

// The long edge of the small copies handed to the sheet's preview.
constexpr std::uint32_t kPreviewEdge = 960;

// A choice knob's value ("" when it is not set).
std::string choice_of(const job_request::part& p, const char* id) {
  const auto it = p.values.find(id);
  return it == p.values.end() || it->second.k != json::kind::string ? std::string() : it->second.s;
}

// The rate a cut clip's own sound is read and written at when it is not
// enhanced (the writer's encoder takes 48 kHz on every OS).
constexpr std::uint32_t kCutSoundRate = 48000;

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

// Restyle's quality: the long edge it runs at and the free memory that needs,
// from the spec's io.qualities by the "quality" knob; io.edge and run_bytes
// when the spec or the request names none. An edge outside 256..1536 or not
// a multiple of 64 is not taken.
struct restyle_quality {
  std::uint32_t edge = kRestyleEdge;
  std::uint64_t run_bytes = 0;
};

restyle_quality quality_of(const job_request::part& part) {
  restyle_quality q;
  q.run_bytes = part.spec->run_bytes;
  const auto take_edge = [&](std::optional<std::int64_t> e) {
    if (e && *e >= 256 && *e <= 1536 && *e % 64 == 0) q.edge = static_cast<std::uint32_t>(*e);
  };
  const json::value& io = part.spec->io;
  take_edge(io.integer("edge"));
  const auto it = part.values.find("quality");
  if (it == part.values.end() || it->second.k != json::kind::string) return q;
  const json::value* qualities = io.find("qualities");
  const json::value* one = qualities ? qualities->find(it->second.s) : nullptr;
  if (!one) return q;
  take_edge(one->integer("edge"));
  if (const auto bytes = one->integer("run_bytes"); bytes && *bytes > 0) q.run_bytes = static_cast<std::uint64_t>(*bytes);
  return q;
}

}  // namespace

// io.tensors: the graphs' names when the pinned export is not our own.
restyle_io restyle_io_of(const model_spec& spec) {
  restyle_io io;
  if (const json::value* t = spec.io.find("tensors")) {
    const auto take = [&](const char* key, std::string& into) {
      if (const std::string* v = t->str(key)) into = *v;
    };
    take("ids", io.ids);
    take("sample", io.sample);
    take("timestep", io.timestep);
    take("hidden", io.hidden);
    take("encode_in", io.encode_in);
    take("decode_in", io.decode_in);
    take("dim_batch", io.dim_batch);
    take("dim_h", io.dim_h);
    take("dim_w", io.dim_w);
    if (const json::value* c = t->find("coreml"); c && c->k == json::kind::boolean) io.coreml = c->b;
    if (const json::value* fixed = t->find("fixed"); fixed && fixed->k == json::kind::object) {
      for (const auto& [name, v] : fixed->o) {
        if (v.k == json::kind::number && v.is_integer && v.i > 0 && v.i < 4096) io.fixed.emplace_back(name, v.i);
      }
    }
  }
  return io;
}

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
  if (k == model_kind::restyle) {
    return item == item_kind::still && port_.open_still && port_.write_still && port_.load_restyle;
  }
  if (k == model_kind::remove) {
    return item == item_kind::still && port_.open_still && port_.write_still && port_.load_inpaint &&
           port_.still_mask;
  }
  if (k == model_kind::face) {
    return item == item_kind::still && port_.open_still && (port_.write_still_as || port_.write_still) &&
           port_.load_face && port_.still_faces;
  }
  return false;
}

std::string job_runner::check(const job_request& r) const {
  if (const job_request::part* face = part_of(r, model_kind::face)) {
    if (port_.free_memory) {
      if (const auto free = port_.free_memory(); free && *free < face->spec->run_bytes) {
        return "Needs " + size_text(face->spec->run_bytes) + " of free memory to run; " + size_text(*free) +
               " is free. Close something and try again.";
      }
    }
    return {};
  }
  if (const job_request::part* remove = part_of(r, model_kind::remove)) {
    const auto mode = remove->values.find("mode");
    const bool extending = mode != remove->values.end() && mode->second.k == json::kind::string &&
                           mode->second.s == "extend";
    if (!extending && (!port_.still_mask || port_.still_mask(r.path).empty())) {
      return "Nothing is brushed. In Edit, open Adjust → Remove, brush over what to take out, then Remove.";
    }
    if (port_.free_memory) {
      if (const auto free = port_.free_memory(); free && *free < remove->spec->run_bytes) {
        return "Needs " + size_text(remove->spec->run_bytes) + " of free memory to run; " + size_text(*free) +
               " is free. Close something and try again.";
      }
    }
    return {};
  }
  if (const job_request::part* restyle = part_of(r, model_kind::restyle)) {
    const auto it = restyle->values.find("prompt");
    const bool empty = it == restyle->values.end() || it->second.k != json::kind::string ||
                       it->second.s.find_first_not_of(" \t") == std::string::npos;
    if (empty) return "Type what to change, for example \"make it a snowy winter day\".";
    // The performance level bounds the quality (tier.h).
    if (const knob* qk = restyle->spec->find("quality")) {
      const auto qv = restyle->values.find("quality");
      if (qv != restyle->values.end() && qv->second.k == json::kind::string) {
        // By the quality's id (Standard 0, High 1, Best 2); another id is Standard.
        const std::string& id = qv->second.s;
        const int rank = id == "best" ? 2 : id == "high" ? 1 : 0;
        if (rank > r.policy.max_quality) {
          const auto opt = std::find_if(qk->options.begin(), qk->options.end(),
                                        [&](const knob_option& o) { return o.id == id; });
          return (opt != qk->options.end() ? opt->text : id) + " quality needs the " +
                 (rank >= 2 ? "Maxxed" : "Default") + " performance level (Settings → Add-ons → Enhance).";
        }
      }
    }
    if (port_.free_memory) {
      const std::uint64_t need = quality_of(*restyle).run_bytes;
      if (const auto free = port_.free_memory(); free && *free < need) {
        return "Needs " + size_text(need) + " of free memory at this quality; " + size_text(*free) +
               " is free. Choose a lower quality, or close something and try again.";
      }
    }
    return {};
  }
  const job_request::part* video = part_of(r, model_kind::video);
  const job_request::part* audio = part_of(r, model_kind::audio);
  const job_request::part* still = part_of(r, model_kind::still);
  if (still) {
    if (!tile_shape_of(*still->spec)) return "This photo model is not one this version of Enhance can run.";
    video = still;  // the same runner, and its memory below
  }
  if (!video && !audio) return "There is nothing for Enhance to run.";
  if (!r.keep_ns.empty() && (!port_.open_frames || !port_.create_clip)) {
    return "This version of Enhance cannot enhance an edit from the Video Editor.";
  }
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
  if (part_of(r, model_kind::face)) {
    run_faces(std::move(r), std::move(ev));
  } else if (part_of(r, model_kind::remove)) {
    run_remove(std::move(r), std::move(ev));
  } else if (part_of(r, model_kind::restyle)) {
    run_restyle(std::move(r), std::move(ev));
  } else if (part_of(r, model_kind::still)) {
    run_still(std::move(r), std::move(ev));
  } else if (part_of(r, model_kind::video) || !r.keep_ns.empty()) {
    // A cut clip's sound alone goes through here too: its picture is cut
    // and written again, unchanged.
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
  auto reader = port_.open_sound(r.path, shape->rate, {});
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
  // The picture model's part, or none: a cut clip's sound alone still writes
  // its picture, unchanged, so the new clip is the edit.
  const job_request::part* video = part_of(r, model_kind::video);
  const job_request::part* audio = part_of(r, model_kind::audio);
  const bool cut = !r.keep_ns.empty();
  std::uint32_t scale = 1;
  std::uint32_t overlap = 0;
  double strength = 0;
  brighten_amount bright = brighten_amount::off;
  if (video) {
    const auto shape = tile_shape_of(*video->spec);
    if (!shape) return failed(status::unsupported_format);
    scale = std::min(scale_of(*video), shape->scale);
    overlap = shape->overlap;
    strength = strength_of(*video);
    bright = brighten_of(choice_of(*video, "brighten"));
  }
  // Strength 0 at the same size is the frame as it was: the model is not run.
  const bool model = video && !(scale == 1 && strength <= 0.0);

  auto reader = port_.open_frames(r.path, r.keep_ns);
  if (!reader) return fail(reader.error() == status::cancelled ? why(status::cancelled)
                                                                : "Enhance cannot read this clip's picture.");
  if (reader->hdr) return fail("This is an HDR clip. Enhance cannot enhance HDR video yet.");
  const std::uint32_t ow = (reader->width * scale) & ~1u, oh = (reader->height * scale) & ~1u;
  if (std::max(ow, oh) > kMaxLongEdge || std::min(ow, oh) > kMaxShortEdge) {
    return fail("At this scale the new clip would be larger than 4K (" + std::to_string(ow) + " x " +
                std::to_string(oh) + "). Choose a smaller scale.");
  }
  std::unique_ptr<tile_model> tiles;
  if (model) {
    auto t = port_.load_tiles(*video->spec, video->models_dir);
    if (!t) return failed(t.error() == status::out_of_memory ? status::out_of_memory : status::internal);
    tiles = std::move(*t);
  }

  // The sound, when it is enhanced too, or the edit's own sound when the clip
  // is cut (the source's cannot be copied whole): on a thread of its own,
  // through a queue the frame loop drains as the picture's time advances.
  std::optional<run_port::reader> sound_in;
  std::unique_ptr<frame_model> sound_model;
  std::uint32_t rate = 0;
  double sound_strength = 1.0;
  if (audio) {
    const auto sshape = stream_shape_of(*audio->spec);
    if (!sshape) return failed(status::unsupported_format);
    rate = sshape->rate;
    auto rd = port_.open_sound(r.path, rate, r.keep_ns);
    if (!rd) return fail("Enhance cannot read this clip's sound.");
    auto m = port_.load_model(*audio->spec, audio->models_dir);
    if (!m) return failed(status::internal);
    sound_in = std::move(*rd);
    sound_model = std::move(*m);
    sound_strength = strength_of(*audio);
  } else if (cut && r.has_sound) {
    rate = kCutSoundRate;
    auto rd = port_.open_sound(r.path, rate, r.keep_ns);
    if (!rd) return fail("Enhance cannot read this clip's sound.");
    sound_in = std::move(*rd);
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
    sound_thread = std::thread([&] {
      if (!sound_model) {
        // The edit's sound as it is: read and handed on.
        bool ok = true;
        while (ok && !cancel_.load()) {
          auto chunk = sound_in->read(rate / 10);
          if (!chunk) {
            ok = false;
            break;
          }
          if (chunk->empty()) break;
          if (!queue->push(*chunk)) break;
        }
        queue->close(ok && !cancel_.load());
        return;
      }
      sound_source src;
      src.channels = sound_in->channels;
      src.total_frames = sound_in->total_frames;
      src.read = sound_in->read;
      sound_sink_port sink;
      sink.write = [&](std::span<const float> v) -> expected {
        return queue->push(v) ? expected{} : expected{err(status::cancelled)};
      };
      sound_job_options o;
      o.strength = sound_strength;
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
  po.overlap = overlap;
  po.strength = strength;
  brightener lift(bright);
  std::vector<std::uint8_t> frame, lit, big;
  std::int64_t done = 0;
  auto last_preview = std::chrono::steady_clock::now();
  std::int64_t total = reader->total_frames;
  if (cut && reader->duration_ms > 0) {
    // The edit's share of the clip's frames.
    std::int64_t kept_ns = 0;
    for (std::size_t i = 0; i + 1 < r.keep_ns.size(); i += 2) kept_ns += r.keep_ns[i + 1] - r.keep_ns[i];
    total = static_cast<std::int64_t>(static_cast<double>(total) * static_cast<double>(kept_ns) /
                                      (static_cast<double>(reader->duration_ms) * 1e6));
  }
  total = std::max<std::int64_t>(1, total);
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
    const std::vector<std::uint8_t>* in = &frame;
    if (lift.active()) {
      (void)lift.apply(frame, reader->width, reader->height, pts, lit);
      in = &lit;
    }
    if (model) {
      ok = enhance_picture(*in, reader->width, reader->height, *tiles, po, big);
      if (!ok) break;
    } else {
      big = *in;  // the same size
    }
    if (ow != reader->width * scale || oh != reader->height * scale) {
      // An odd size made even: the last column / row goes.
      std::vector<std::uint8_t> even(std::size_t{ow} * oh * 3);
      for (std::uint32_t y = 0; y < oh; ++y) {
        std::copy_n(big.data() + std::size_t{y} * reader->width * scale * 3, std::size_t{ow} * 3,
                    even.data() + std::size_t{y} * ow * 3);
      }
      big.swap(even);
    }
    // The sheet's preview: the first frame at once, then about once a second.
    if (ev.preview) {
      const auto now = std::chrono::steady_clock::now();
      if (done == 0 || now - last_preview >= std::chrono::seconds(1)) {
        last_preview = now;
        ev.preview(shrink(frame, reader->width, reader->height, kPreviewEdge), shrink(big, ow, oh, kPreviewEdge));
      }
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
  // The photo in the sheet's preview while its tiles run; the split once done.
  const preview_image before = shrink(pic->rgb, pic->width, pic->height, kPreviewEdge);
  if (ev.preview) ev.preview(before, {});
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
  if (ev.preview) ev.preview(before, shrink(out.rgb, out.width, out.height, kPreviewEdge));
  if (cancel_.load()) return fail(why(status::cancelled));
  auto path = port_.write_still(r.path, out);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

// Slice 5b: the photo small, restyled by the words, the change carried back
// onto the full photo (diffusion.h). The original is never written.
void job_runner::run_restyle(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const job_request::part& part = *part_of(r, model_kind::restyle);
  restyle_params p;
  auto value = [&](const char* id) -> const json::value* {
    const auto it = part.values.find(id);
    return it == part.values.end() ? nullptr : &it->second;
  };
  if (const json::value* v = value("prompt"); v && v->k == json::kind::string) p.prompt = v->s;
  if (const json::value* v = value("keep"); v && v->k == json::kind::number) {
    p.image_guidance = static_cast<float>(v->is_integer ? static_cast<double>(v->i) : v->d);
  }
  float strength = 1.0f;
  if (const json::value* v = value("strength"); v && v->k == json::kind::number) {
    strength = static_cast<float>(v->is_integer ? static_cast<double>(v->i) : v->d);
  }
  if (ev.progress) ev.progress(-1);
  auto pic = port_.open_still(r.path);
  if (!pic) return fail(pic.error() == status::cancelled ? why(status::cancelled) : "Enhance cannot read this photo.");
  if (cancel_.load()) return fail(why(status::cancelled));
  const preview_image before = shrink(pic->rgb, pic->width, pic->height, kPreviewEdge);
  if (ev.preview) ev.preview(before, {});
  auto model = port_.load_restyle(*part.spec, part.models_dir, r.policy.threads);
  if (!model) return fail(why(model.error() == status::out_of_memory ? status::out_of_memory : status::internal));
  rgb8 full;
  full.width = pic->width;
  full.height = pic->height;
  full.rgb = std::move(pic->rgb);
  std::uint32_t w = 0, h = 0;
  p.edge = quality_of(part).edge;
  p.steps = r.policy.restyle_steps;
  restyle_size(full.width, full.height, p.edge, w, h);
  // A model that needs the GPU (its spec's cpu false) is refused where its
  // UNet would run on the CPU: many minutes a request (on Windows the bundled
  // runtime is the CPU build).
  if (const expected ready = (*model)->prepare(w, h); !ready) return fail(why(ready.error()));
  if (!part.spec->cpu && !(*model)->on_gpu()) {
    return fail("Restyle needs a graphics processor Enhance can use, and this computer has none it can use yet.");
  }
  const rgb8 small = resize_rgb(full, w, h);
  auto edited = (*model)->run(small, p, [&](int step, int steps) {
    if (ev.progress) ev.progress(std::min(0.95, static_cast<double>(step) / std::max(steps, 1)));
    return !cancel_.load();
  });
  if (!edited) return fail(why(edited.error()));
  const rgb8 out = carry_back(full, small, *edited, strength);
  if (ev.preview) ev.preview(before, shrink(out.rgb, out.width, out.height, kPreviewEdge));
  if (cancel_.load()) return fail(why(status::cancelled));
  run_port::still still;
  still.width = out.width;
  still.height = out.height;
  still.rgb = out.rgb;
  auto path = port_.write_still(r.path, still);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

// Slice 7: what was brushed in Edit, filled in from its surroundings on a
// crop of the full photo (remove.h); every pixel away from the brush is the
// photo's own. The original is never written.
void job_runner::run_remove(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const job_request::part& part = *part_of(r, model_kind::remove);
  if (const auto mode = part.values.find("mode");
      mode != part.values.end() && mode->second.k == json::kind::string && mode->second.s == "extend") {
    return run_extend(std::move(r), std::move(ev));
  }
  if (ev.progress) ev.progress(-1);
  const std::vector<float> dabs = port_.still_mask(r.path);
  auto pic = port_.open_still(r.path);
  if (!pic) return fail(pic.error() == status::cancelled ? why(status::cancelled) : "Enhance cannot read this photo.");
  if (cancel_.load()) return fail(why(status::cancelled));
  rgb8 full;
  full.width = pic->width;
  full.height = pic->height;
  full.rgb = std::move(pic->rgb);
  const preview_image before = shrink(full.rgb, full.width, full.height, kPreviewEdge);
  if (ev.preview) ev.preview(before, {});
  const mask8 mask = mask_from_dabs(dabs, full.width, full.height);
  const auto box = remove_box(mask);
  if (!box) return fail("Nothing is brushed on this photo. Brush over what to take out, then Remove.");
  auto model = port_.load_inpaint(*part.spec, part.models_dir);
  if (!model) return fail(why(model.error() == status::out_of_memory ? status::out_of_memory : status::internal));
  if (ev.progress) ev.progress(0.2);
  const std::uint32_t side = (*model)->side();
  const rgb8 crop = crop_rgb(full, *box);
  auto filled = (*model)->fill(resize_rgb(crop, side, side), crop_mask(mask, *box, side, side));
  if (!filled) return fail(why(filled.error()));
  if (cancel_.load()) return fail(why(status::cancelled));
  if (ev.progress) ev.progress(0.9);
  // The feather grows with the box: a few pixels on a small fix, more on a
  // large one the model filled at a fraction of its size.
  const std::uint32_t feather = std::clamp<std::uint32_t>(box->width / 256, 2, 16);
  paste_back(full, *box, resize_rgb(*filled, box->width, box->height), mask, feather);
  if (ev.preview) ev.preview(before, shrink(full.rgb, full.width, full.height, kPreviewEdge));
  run_port::still still;
  still.width = full.width;
  still.height = full.height;
  still.rgb = std::move(full.rgb);
  auto path = port_.write_still(r.path, still);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

// Owner 2026-11-04: the picture extended past its edges, Remove's model
// filling the new margins tile by tile (remove.h extend_*), each tile seeing
// the photo and the tiles filled before it. The photo's own pixels are kept.
void job_runner::run_extend(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const job_request::part& part = *part_of(r, model_kind::remove);
  if (ev.progress) ev.progress(-1);
  auto pic = port_.open_still(r.path);
  if (!pic) return fail(pic.error() == status::cancelled ? why(status::cancelled) : "Enhance cannot read this photo.");
  rgb8 photo;
  photo.width = pic->width;
  photo.height = pic->height;
  photo.rgb = std::move(pic->rgb);
  std::string preset = "wider";
  if (const auto it = part.values.find("extend"); it != part.values.end() && it->second.k == json::kind::string) {
    preset = it->second.s;
  }
  const extend_margins m = extend_preset(preset, photo.width, photo.height);
  if (m.empty()) return fail("The photo is already that shape.");
  if (std::uint64_t{photo.width + m.left + m.right} * (photo.height + m.top + m.bottom) > kMaxStillPixels) {
    return fail("The extended picture would be over 100 megapixels.");
  }
  rgb8 out;
  mask8 mask;
  extend_canvas(photo, m, out, mask);
  const preview_image before = shrink(photo.rgb, photo.width, photo.height, kPreviewEdge);
  if (ev.preview) ev.preview(before, {});
  const rgb8 original = std::move(photo);
  auto model = port_.load_inpaint(*part.spec, part.models_dir);
  if (!model) return fail(why(model.error() == status::out_of_memory ? status::out_of_memory : status::internal));
  const std::vector<pixel_box> tiles = extend_tiles(out.width, out.height, m);
  const expected filled = extend_fill(out, mask, tiles, **model, [&](std::size_t done, std::size_t total) {
    if (ev.progress) ev.progress(0.1 + 0.85 * static_cast<double>(done) / std::max<std::size_t>(total, 1));
    return !cancel_.load();
  });
  if (!filled) return fail(why(filled.error()));
  // The photo, exactly as it was, in the middle.
  for (std::uint32_t y = 0; y < original.height; ++y) {
    std::copy_n(original.rgb.data() + std::size_t{y} * original.width * 3, std::size_t{original.width} * 3,
                out.rgb.data() + (std::size_t{y + m.top} * out.width + m.left) * 3);
  }
  if (ev.preview) ev.preview(before, shrink(out.rgb, out.width, out.height, kPreviewEdge));
  run_port::still still;
  still.width = out.width;
  still.height = out.height;
  still.rgb = std::move(out.rgb);
  auto path = port_.write_still(r.path, still);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

// docs/plans/edit-refresh.md S10 (owner call C): every face the AI pack finds
// restored by the face model, guarded by People, pasted back at Strength
// (face_restore.h). Pixels away from the faces are the photo's own; the
// original is never written. "<stem>-restored.jpg" (doc 29).
void job_runner::run_faces(job_request r, job_events ev) {
  const auto fail = [&](const std::string& message) {
    if (ev.done) ev.done(false, "", message);
  };
  const job_request::part& part = *part_of(r, model_kind::face);
  std::string choice = choice_of(part, "model");
  if (choice.empty()) choice = "restoreformer";
  face_options o;
  // Strength as chosen, else the spec's default (0.7), never a full replace by omission.
  if (const auto it = part.values.find("strength"); it != part.values.end() && it->second.k == json::kind::number) {
    o.strength = static_cast<float>(strength_of(part));
  } else if (const knob* k = part.spec->find("strength")) {
    o.strength = static_cast<float>(std::clamp(k->def, 0.0, 1.0));
  }
  if (ev.progress) ev.progress(-1);
  auto pic = port_.open_still(r.path);
  if (!pic) return fail(pic.error() == status::cancelled ? why(status::cancelled) : "Enhance cannot read this photo.");
  if (cancel_.load()) return fail(why(status::cancelled));
  rgb8 full;
  full.width = pic->width;
  full.height = pic->height;
  full.rgb = std::move(pic->rgb);
  const preview_image before = shrink(full.rgb, full.width, full.height, kPreviewEdge);
  if (ev.preview) ev.preview(before, {});
  const auto pack_why = [](status s) -> std::string {
    if (s == status::not_found) return "Restore faces finds faces with Local search. Add it in Settings → Add-ons.";
    if (s == status::permission_denied) {
      return "Turn on People in Local search: Restore faces finds the faces with it, and checks each restored face "
             "still looks like the same person.";
    }
    return "Enhance could not look for faces in this photo.";
  };
  auto found = find_faces(full, port_.still_faces);
  if (!found) return fail(pack_why(found.error()));
  if (found->empty()) return fail("No faces were found in this photo.");
  if (cancel_.load()) return fail(why(status::cancelled));
  auto model = port_.load_face(*part.spec, part.models_dir, choice, r.policy.threads);
  if (!model) return fail(why(model.error() == status::out_of_memory ? status::out_of_memory : status::internal));
  o.progress = [&](std::size_t done, std::size_t total) {
    if (ev.progress) ev.progress(0.05 + 0.9 * static_cast<double>(done) / std::max<std::size_t>(total, 1));
    return !cancel_.load();
  };
  auto out = restore_faces(full, *found, **model, port_.still_faces, o);
  if (!out) {
    if (out.error() == status::not_found || out.error() == status::permission_denied) {
      return fail(pack_why(out.error()));
    }
    return fail(why(out.error()));
  }
  if (out->restored == 0) {
    if (out->kept > 0) {
      return fail("No face was changed: restored, each one stopped looking like the same person, so the photo was "
                  "left as it was.");
    }
    return fail("The faces in this photo are too small to restore.");
  }
  if (ev.preview) ev.preview(before, shrink(full.rgb, full.width, full.height, kPreviewEdge));
  if (cancel_.load()) return fail(why(status::cancelled));
  run_port::still still;
  still.width = full.width;
  still.height = full.height;
  still.rgb = std::move(full.rgb);
  auto path =
      port_.write_still_as ? port_.write_still_as(r.path, still, "restored") : port_.write_still(r.path, still);
  if (!path) return fail(why(path.error()));
  if (ev.done) ev.done(true, name_of(*path), "");
}

}  // namespace mv::enhance
