// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/meta_writer.h"

#include <utility>

#include "io/paths.h"

namespace mv::shell {
namespace {

void merge(meta::write_fields& into, const meta::write_fields& from) {
  if (from.rating.touches()) into.rating = from.rating;
  if (from.orientation.touches()) into.orientation = from.orientation;
  if (from.comment.touches()) into.comment = from.comment;
}

}  // namespace

meta_writer::entry* meta_writer::find(std::string_view path) {
  for (auto& e : pending_) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

const meta_writer::entry* meta_writer::find(std::string_view path) const {
  for (const auto& e : pending_) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

void meta_writer::submit(std::string_view path, const meta::write_fields& f) {
  if (path.empty() || f.empty()) return;
  if (entry* e = find(path)) {
    e->revert = false;  // a fresh request outranks a queued revert
    merge(e->fields, f);
    return;
  }
  pending_.push_back(entry{std::string(path), f, false});
}

void meta_writer::submit_revert(std::string_view path) {
  if (path.empty()) return;
  if (entry* e = find(path)) {
    e->fields = {};
    e->revert = true;
    return;
  }
  pending_.push_back(entry{std::string(path), {}, true});
}

std::optional<meta_job> meta_writer::take_next() {
  if (in_flight_ || pending_.empty()) return std::nullopt;
  entry e = std::move(pending_.front());
  pending_.pop_front();
  meta_job j;
  j.path = std::move(e.path);
  j.fields = std::move(e.fields);
  j.revert = e.revert;
  in_flight_ = j;
  return j;
}

std::vector<meta_job> meta_writer::drain_for_exit() {
  std::vector<meta_job> jobs;
  jobs.reserve(pending_.size() + 1);
  if (in_flight_) jobs.push_back(std::move(*in_flight_));
  in_flight_.reset();
  for (entry& e : pending_) {
    meta_job j;
    j.path = std::move(e.path);
    j.fields = std::move(e.fields);
    j.revert = e.revert;
    jobs.push_back(std::move(j));
  }
  pending_.clear();
  return jobs;
}

void meta_writer::finished(const meta_outcome& outcome) {
  in_flight_.reset();
  if (!outcome.ok) failure_ = outcome;
}

bool meta_writer::busy_for(std::string_view path) const noexcept {
  return (in_flight_ && in_flight_->path == path) || find(path) != nullptr;
}

std::optional<int> meta_writer::pending_rating(std::string_view path) const {
  // A queued request is newer than the one running.
  if (const entry* e = find(path); e && e->fields.rating.touches()) {
    return e->fields.rating.k == meta::change<int>::kind::clear ? 0 : e->fields.rating.value;
  }
  if (in_flight_ && in_flight_->path == path && in_flight_->fields.rating.touches()) {
    const auto& r = in_flight_->fields.rating;
    return r.k == meta::change<int>::kind::clear ? 0 : r.value;
  }
  return std::nullopt;
}

std::optional<std::string> meta_writer::pending_comment(std::string_view path) const {
  const auto text_of = [](const meta::change<std::string>& c) {
    return c.k == meta::change<std::string>::kind::clear ? std::string{} : c.value;
  };
  if (const entry* e = find(path); e && e->fields.comment.touches()) return text_of(e->fields.comment);
  if (in_flight_ && in_flight_->path == path && in_flight_->fields.comment.touches()) {
    return text_of(in_flight_->fields.comment);
  }
  return std::nullopt;
}

std::optional<meta_outcome> meta_writer::take_failure() {
  std::optional<meta_outcome> f = std::move(failure_);
  failure_.reset();
  return f;
}

meta_outcome run_meta_job(const meta_job& job, std::string_view snapshot_dir) {
  meta_outcome out;
  out.path = job.path;
  out.revert = job.revert;

  std::string dir(snapshot_dir);
  if (dir.empty()) {
    auto d = io::metadata_snapshot_dir();
    if (!d) {
      out.error = d.error();
      return out;
    }
    dir = std::move(*d);
  }
  auto r = job.revert ? meta::revert(job.path, dir) : meta::write(job.path, job.fields, dir);
  if (!r) {
    out.error = r.error();
    return out;
  }
  out.ok = true;
  out.target = r->target;
  out.sidecar_touched = r->sidecar_touched;
  out.sidecar_path = r->sidecar_path;
  return out;
}

meta::write_fields rating_fields(int stars) {
  meta::write_fields f;
  f.rating = stars == 0 ? meta::change<int>::remove() : meta::change<int>::to(stars);
  return f;
}

meta::write_fields comment_fields(std::string_view utf8) {
  meta::write_fields f;
  f.comment = utf8.empty() ? meta::change<std::string>::remove()
                           : meta::change<std::string>::to(std::string(utf8));
  return f;
}

}  // namespace mv::shell
