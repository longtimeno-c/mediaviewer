// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/downloads.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "core/json.h"

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

int slot(model_kind k) noexcept { return static_cast<int>(k); }

bool hex64(const std::string& s) noexcept {
  return s.size() == 64 &&
         std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::uint64_t part_size(const std::string& root, const ai::remote_file& f) {
  std::error_code ec;
  const auto n = fs::file_size(fs_path(root + "/" + f.path + ".part"), ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

}  // namespace

std::vector<pin> read_pins(std::string_view manifest_json) {
  std::vector<pin> out;
  const auto doc = json::parse(manifest_json, 8);
  const json::value* remote = doc ? doc->find("remote") : nullptr;
  if (!remote || remote->k != json::kind::array) return out;
  for (const json::value& r : remote->a) {
    const std::string* url = r.str("url");
    const std::string* path = r.str("path");
    const std::string* sha = r.str("sha256");
    const std::string* licence = r.str("licence");
    const auto size = r.integer("size");
    // The host refused the manifest unless every entry was well formed
    // (src/addon/manifest.cpp); this only takes what it needs.
    if (!url || !path || !sha || !hex64(*sha) || !size || *size <= 0) continue;
    out.push_back(pin{ai::remote_file{*url, *path, *sha, static_cast<std::uint64_t>(*size)}, licence ? *licence : ""});
  }
  return out;
}

model_files files_for(const model_spec& spec, const std::vector<pin>& pins) {
  model_files m;
  const std::string prefix = std::string(kind_name(spec.kind)) + "/";
  for (const std::string& name : spec.weights) {
    const auto it =
        std::find_if(pins.begin(), pins.end(), [&](const pin& p) { return p.file.path == prefix + name; });
    if (it == pins.end()) {
      m.missing = true;
      continue;
    }
    if (it->file.size > kMaxWeightsBytes) m.too_large = true;
    m.files.push_back(it->file);
    m.licences.push_back(it->licence);
    m.bytes += it->file.size;
  }
  return m;
}

downloads::downloads(std::string root, port p, ai::fetch_options options)
    : root_(std::move(root)), port_(std::move(p)), opt_(std::move(options)) {}

downloads::~downloads() { stop_all(); }

bool downloads::ready(const model_files& m) const {
  if (!m.usable()) return false;
  return std::all_of(m.files.begin(), m.files.end(), [&](const ai::remote_file& f) { return ai::fetch_ready(root_, f); });
}

dl_status downloads::status(model_kind k, const model_files& m) const {
  dl_status s;
  {
    std::lock_guard lock(m_);
    const auto& r = runs_[slot(k)];
    if (r) {
      if (r->running.load()) return r->last;
      s = r->last;
    }
  }
  if (!m.usable()) {
    s.state = dl_state::refused;
    return s;
  }
  if (ready(m)) {
    s.state = dl_state::ready;
    s.done_bytes = s.total_bytes = m.bytes;
    return s;
  }
  if (s.state == dl_state::ready || s.state == dl_state::downloading || s.state == dl_state::waiting) {
    // The files went since (removed by hand): not here.
    s = dl_status{};
  }
  s.total_bytes = m.bytes;
  if (s.state == dl_state::absent) {
    // Bytes from an earlier run (a quit mid-download): Resume, not Install.
    std::uint64_t have = 0;
    for (const ai::remote_file& f : m.files) have += ai::fetch_ready(root_, f) ? f.size : part_size(root_, f);
    if (have > 0) {
      s.state = dl_state::stopped;
      s.done_bytes = have;
    }
  }
  return s;
}

void downloads::start(model_kind k, const model_files& m) {
  const int i = slot(k);
  {
    std::lock_guard lock(m_);
    if (runs_[i] && runs_[i]->running.load()) return;
  }
  join(k);
  dl_status s;
  s.total_bytes = m.bytes;
  if (!m.usable()) {
    // A pin over 5 GB, or a spec naming a file the manifest does not pin:
    // refused before any request.
    s.state = dl_state::refused;
    finish(k, s);
    return;
  }
  // Room for what is still to come (the .part is the file: no second copy),
  // with a tenth more for the filesystem.
  std::uint64_t remaining = 0;
  for (const ai::remote_file& f : m.files) {
    if (!ai::fetch_ready(root_, f)) remaining += f.size - std::min(f.size, part_size(root_, f));
  }
  if (remaining > 0 && port_.free_disk) {
    std::error_code ec;
    fs::create_directories(fs_path(root_), ec);
    if (const auto free = port_.free_disk(root_); free && *free < remaining + remaining / 10) {
      s.state = dl_state::no_space;
      s.need_bytes = remaining + remaining / 10;
      s.free_bytes = *free;
      finish(k, s);
      return;
    }
  }
  auto net = port_.transport ? port_.transport() : nullptr;
  if (!net) {
    s.state = dl_state::stopped;
    finish(k, s);
    return;
  }
  auto r = std::make_unique<run>();
  run* raw = r.get();
  r->running = true;
  r->last.state = dl_state::downloading;
  r->last.total_bytes = m.bytes;
  {
    std::lock_guard lock(m_);
    runs_[i] = std::move(r);
  }
  if (port_.changed) port_.changed(k);
  raw->t = std::thread([this, k, raw, files = m.files, net = std::move(net)]() mutable {
    ai::fetcher f(root_, *net, opt_);
    const auto progress = [&](const ai::fetch_progress& p) {
      {
        std::lock_guard lock(m_);
        raw->last.done_bytes = p.done_bytes;
        raw->last.total_bytes = p.total_bytes;
        raw->last.retry_in_s = p.retry_in_s;
        raw->last.state = p.state == ai::fetch_state::waiting ? dl_state::waiting : dl_state::downloading;
      }
      if (port_.changed) port_.changed(k);
    };
    const expected done = f.run(files, raw->stop, progress);
    dl_status s;
    {
      std::lock_guard lock(m_);
      s = raw->last;
    }
    s.retry_in_s = 0;
    if (done) {
      s.state = dl_state::ready;
      s.done_bytes = s.total_bytes;
    } else if (done.error() == status::not_found) {
      s.state = dl_state::gone;
    } else if (done.error() == status::corrupt) {
      s.state = dl_state::failed;
    } else {
      s.state = dl_state::stopped;  // stopped, or the disk refused a write: Resume
    }
    {
      std::lock_guard lock(m_);
      raw->last = s;
      raw->running = false;
    }
    if (port_.changed) port_.changed(k);
  });
}

void downloads::finish(model_kind k, dl_status s) {
  {
    std::lock_guard lock(m_);
    auto& r = runs_[slot(k)];
    if (!r) r = std::make_unique<run>();
    r->last = s;
  }
  if (port_.changed) port_.changed(k);
}

void downloads::join(model_kind k) {
  std::thread t;
  {
    std::lock_guard lock(m_);
    auto& r = runs_[slot(k)];
    if (!r || !r->t.joinable()) return;
    t = std::move(r->t);
  }
  t.join();
}

void downloads::stop(model_kind k) {
  {
    std::lock_guard lock(m_);
    if (auto& r = runs_[slot(k)]) r->stop = true;
  }
  join(k);
}

void downloads::remove(model_kind k, const model_files& m) {
  stop(k);
  {
    std::lock_guard lock(m_);
    runs_[slot(k)].reset();
  }
  std::error_code ec;
  for (const ai::remote_file& f : m.files) {
    const std::string path = root_ + "/" + f.path;
    fs::remove(fs_path(path), ec);
    fs::remove(fs_path(path + ".ok"), ec);
    fs::remove(fs_path(path + ".part"), ec);
  }
  // What a provider compiled from them (Core ML's cache, slice 5).
  fs::remove_all(fs_path(root_ + "/" + kind_name(k) + "-cache"), ec);
  if (port_.changed) port_.changed(k);
}

void downloads::stop_all() {
  for (const model_kind k : kModelKinds) stop(k);
}

}  // namespace mv::enhance
