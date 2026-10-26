// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/host_port.h"

#include <cstddef>
#include <memory>

#include "addons/enhance/memory.h"
#include "addons/enhance/stream_model.h"

namespace mv::enhance {

bool has_sound_rows(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, sound_write_close) + sizeof(h->sound_write_close) &&
         h->sound_open && h->sound_read && h->sound_close && h->sound_write_open && h->sound_write &&
         h->sound_write_finish && h->sound_write_close;
}

run_port run_port_of(const mv_host_api* h, const std::string& self_dir) {
  run_port p;
  p.open_sound = [h](const std::string& path, uint32_t rate) -> mv::result<run_port::reader> {
    mv_addon_sound_info info{};
    info.struct_size = sizeof info;
    void* raw = nullptr;
    const mv_status st = h->sound_open(h->host, path.c_str(), rate, 0, &info, &raw);
    if (st != MV_OK || !raw) return mv::err(st == MV_ERR_CANCELLED ? mv::status::cancelled : mv::status::unsupported_format);
    std::shared_ptr<void> sound(raw, [h](void* s) { h->sound_close(h->host, s); });
    run_port::reader r;
    r.channels = info.channels;
    r.total_frames = info.duration_ms * static_cast<int64_t>(rate) / 1000;
    const uint32_t ch = info.channels;
    r.read = [h, sound, ch](std::size_t max_frames) -> mv::result<std::vector<float>> {
      std::vector<float> buf(max_frames * ch);
      uint32_t frames = 0;
      const mv_status st = h->sound_read(h->host, sound.get(), buf.data(), static_cast<uint32_t>(max_frames), &frames);
      if (st != MV_OK) return mv::err(static_cast<mv::status>(st));
      buf.resize(static_cast<std::size_t>(frames) * ch);
      return buf;
    };
    return r;
  };
  p.create = [h](const std::string& source, uint32_t rate, uint32_t channels) -> mv::result<run_port::writer> {
    void* raw = nullptr;
    const mv_status st = h->sound_write_open(h->host, source.c_str(), rate, channels, "enhance", nullptr, &raw);
    if (st != MV_OK || !raw) return mv::err(static_cast<mv::status>(st == MV_OK ? MV_ERR_INTERNAL : st));
    std::shared_ptr<void> w(raw, [h](void* x) { h->sound_write_close(h->host, x); });
    run_port::writer out;
    out.write = [h, w, channels](std::span<const float> pcm) -> mv::expected {
      const mv_status s = h->sound_write(h->host, w.get(), pcm.data(), static_cast<uint32_t>(pcm.size() / channels));
      return s == MV_OK ? mv::expected{} : mv::expected{mv::err(static_cast<mv::status>(s))};
    };
    out.finish = [h, w]() -> mv::result<std::string> {
      char buf[4096] = {};
      const mv_status s = h->sound_write_finish(h->host, w.get(), buf, sizeof buf);
      if (s != MV_OK) return mv::err(static_cast<mv::status>(s));
      return std::string(buf);
    };
    return out;
  };
  // Released when the port goes: the runner's, at the add-on's shutdown,
  // after its job thread has been joined.
  auto runtime = std::make_shared<runtime_holder>(self_dir);
  p.load_model = [runtime](const model_spec& spec, const std::string& models_dir) {
    return runtime->open(spec, models_dir);
  };
  p.free_memory = [] { return available_memory(); };
  return p;
}

}  // namespace mv::enhance
