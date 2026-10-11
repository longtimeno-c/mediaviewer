// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/meta_writer.h"

#include <utility>

#include "io/paths.h"

namespace mv::shell {
namespace {

void merge(meta::write_fields& into, const meta::write_fields& from) {
  if (from.rating.touches()) into.rating = from.rating;
  if (from.orientation.touches()) into.orientation = from.orientation;
  if (from.comment.touches()) into.comment = from.comment;
  // PR 29: a later edit of a tag replaces an earlier one still queued.
  for (const meta::tag_edit& t : from.tags) {
    bool replaced = false;
    for (meta::tag_edit& have : into.tags) {
      if (have.key == t.key) {
        have = t;
        replaced = true;
        break;
      }
    }
    if (!replaced) into.tags.push_back(t);
  }
  if (from.date_taken.touches()) into.date_taken = from.date_taken;
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

std::optional<std::string> meta_writer::pending_label(std::string_view path) const {
  if (const entry* e = find(path)) {
    if (auto l = label_of(e->fields)) return l;
  }
  if (in_flight_ && in_flight_->path == path) return label_of(in_flight_->fields);
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

const char* colour_label_name(colour_label l) noexcept {
  switch (l) {
    case colour_label::none: return "";
    case colour_label::red: return "Red";
    case colour_label::yellow: return "Yellow";
    case colour_label::green: return "Green";
    case colour_label::blue: return "Blue";
    case colour_label::purple: return "Purple";
  }
  return "";
}

std::string label_toggled(colour_label pressed, std::string_view now) {
  const std::string_view want = colour_label_name(pressed);
  if (want.empty()) return {};
  // Case-insensitive, ASCII: another app may have written "red".
  const auto same = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
      if (x != y) return false;
    }
    return true;
  };
  return same(want, now) ? std::string{} : std::string(want);
}

meta::write_fields label_fields(std::string_view label) {
  meta::write_fields f;
  meta::tag_edit t;
  t.key = kLabelTag;
  t.value = label.empty() ? meta::change<std::string>::remove()
                          : meta::change<std::string>::to(std::string(label));
  f.tags.push_back(std::move(t));
  return f;
}

std::optional<std::string> label_of(const meta::write_fields& fields) {
  for (auto it = fields.tags.rbegin(); it != fields.tags.rend(); ++it) {
    if (it->key != kLabelTag) continue;
    return it->value.k == meta::change<std::string>::kind::set ? it->value.value : std::string{};
  }
  return std::nullopt;
}

std::string describe_write(const meta::write_fields& fields) {
  using meta::change;
  if (fields.rating.touches()) {
    const int stars = fields.rating.k == change<int>::kind::clear ? 0 : fields.rating.value;
    return stars == 0 ? std::string("Rating cleared") : meta::format_rating(stars);
  }
  if (fields.comment.touches()) {
    return fields.comment.k == change<std::string>::kind::clear ? "Comment removed" : "Comment saved";
  }
  if (fields.date_taken.touches()) {
    return fields.date_taken.k == change<std::string>::kind::clear ? "Date taken removed"
                                                                  : "Date taken saved";
  }
  if (fields.tags.size() == 1) {
    if (auto l = label_of(fields)) return l->empty() ? std::string("Label cleared") : "Label: " + *l;
    return "Metadata saved";
  }
  if (!fields.tags.empty()) return std::to_string(fields.tags.size()) + " tags saved";
  return {};
}

}  // namespace mv::shell
