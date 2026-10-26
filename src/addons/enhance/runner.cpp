// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/runner.h"

#include <algorithm>

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

}  // namespace

job_runner::job_runner(run_port port) : port_(std::move(port)) {}

job_runner::~job_runner() {
  cancel();
}

bool job_runner::supports(model_kind k, item_kind item) const {
  // Slice 4: sound, from an audio file or a clip. A clip's picture and a
  // still are later slices.
  return k == model_kind::audio && (item == item_kind::audio || item == item_kind::media) && port_.open_sound &&
         port_.create && port_.load_model;
}

std::string job_runner::check(const job_request& r) const {
  if (r.parts.size() != 1 || r.parts[0].kind != model_kind::audio || !r.parts[0].spec) {
    return "This version of Enhance can only enhance sound.";
  }
  if (!stream_shape_of(*r.parts[0].spec)) return "This audio model is not one this version of Enhance can run.";
  const std::uint64_t need = r.parts[0].spec->run_bytes;
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
