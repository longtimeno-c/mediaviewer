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

bool has_still_rows(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, still_write) + sizeof(h->still_write) && h->still_open &&
         h->still_pixels && h->still_close && h->still_write;
}

// Slice 7: the Remove brush's row, appended after sound_keep.
bool has_mask_row(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, still_mask) + sizeof(h->still_mask) && h->still_mask;
}

bool has_picture_rows(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, clip_write_close) + sizeof(h->clip_write_close) &&
         h->frames_open && h->frames_read && h->frames_close && h->clip_write_open && h->clip_write_frame &&
         h->clip_write_sound && h->clip_write_finish && h->clip_write_close;
}

// The Video Editor's cut (ABI 0.35): frames_keep and sound_keep.
bool has_keep_rows(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, sound_keep) + sizeof(h->sound_keep) && h->frames_keep &&
         h->sound_keep;
}

namespace {
std::int64_t kept_ms(std::span<const std::int64_t> keep) {
  std::int64_t ns = 0;
  for (std::size_t i = 0; i + 1 < keep.size(); i += 2) ns += keep[i + 1] - keep[i];
  return ns / 1'000'000;
}
}  // namespace

run_port run_port_of(const mv_host_api* h, const std::string& self_dir) {
  run_port p;
  p.open_sound = [h](const std::string& path, uint32_t rate,
                     std::span<const std::int64_t> keep) -> mv::result<run_port::reader> {
    if (!keep.empty() && !has_keep_rows(h)) return mv::err(mv::status::unsupported_format);
    mv_addon_sound_info info{};
    info.struct_size = sizeof info;
    void* raw = nullptr;
    const mv_status st = h->sound_open(h->host, path.c_str(), rate, 0, &info, &raw);
    if (st != MV_OK || !raw) return mv::err(st == MV_ERR_CANCELLED ? mv::status::cancelled : mv::status::unsupported_format);
    std::shared_ptr<void> sound(raw, [h](void* s) { h->sound_close(h->host, s); });
    if (!keep.empty() &&
        h->sound_keep(h->host, sound.get(), keep.data(), static_cast<uint32_t>(keep.size() / 2)) != MV_OK) {
      return mv::err(mv::status::invalid_arg);
    }
    run_port::reader r;
    r.channels = info.channels;
    r.total_frames = (keep.empty() ? info.duration_ms : kept_ms(keep)) * static_cast<int64_t>(rate) / 1000;
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

  // Slice 3: a photo, when the host has those rows (and the tiles below).
  if (has_still_rows(h)) {
    p.open_still = [h](const std::string& path) -> mv::result<run_port::still> {
      mv_addon_still_info info{};
      info.struct_size = sizeof info;
      void* raw = nullptr;
      const mv_status st = h->still_open(h->host, path.c_str(), &info, &raw);
      if (st != MV_OK || !raw) return mv::err(st == MV_ERR_OUT_OF_MEMORY ? mv::status::out_of_memory : mv::status::unsupported_format);
      std::shared_ptr<void> still(raw, [h](void* s) { h->still_close(h->host, s); });
      run_port::still out;
      out.width = info.width;
      out.height = info.height;
      out.rgb.resize(std::size_t{info.width} * info.height * 3);
      const mv_status ps = h->still_pixels(h->host, still.get(), out.rgb.data(), out.rgb.size());
      if (ps != MV_OK) return mv::err(static_cast<mv::status>(ps));
      return out;
    };
    p.write_still = [h](const std::string& source, const run_port::still& pic) -> mv::result<std::string> {
      char buf[4096] = {};
      const mv_status st = h->still_write(h->host, source.c_str(), pic.rgb.data(), pic.width, pic.height,
                                          pic.width * 3, "enhance", buf, sizeof buf);
      if (st != MV_OK) return mv::err(static_cast<mv::status>(st));
      return std::string(buf);
    };
  }

  // Slice 5: a clip's picture, when the host has those rows too.
  if (has_picture_rows(h)) {
    p.open_frames = [h](const std::string& path,
                        std::span<const std::int64_t> keep) -> mv::result<run_port::picture_reader> {
      if (!keep.empty() && !has_keep_rows(h)) return mv::err(mv::status::unsupported_format);
      mv_addon_frames_info info{};
      info.struct_size = sizeof info;
      void* raw = nullptr;
      const mv_status st = h->frames_open(h->host, path.c_str(), &info, &raw);
      if (st != MV_OK || !raw) return mv::err(st == MV_ERR_CANCELLED ? mv::status::cancelled : mv::status::unsupported_format);
      std::shared_ptr<void> frames(raw, [h](void* f) { h->frames_close(h->host, f); });
      if (!keep.empty() &&
          h->frames_keep(h->host, frames.get(), keep.data(), static_cast<uint32_t>(keep.size() / 2)) != MV_OK) {
        return mv::err(mv::status::invalid_arg);
      }
      run_port::picture_reader r;
      r.width = info.width;
      r.height = info.height;
      r.hdr = info.hdr != 0;
      r.total_frames = info.frame_count;
      r.duration_ms = info.duration_ms;
      const std::size_t bytes = std::size_t{info.width} * info.height * 3;
      r.next = [h, frames, bytes](std::vector<std::uint8_t>& rgb, std::int64_t& pts_us) -> mv::result<bool> {
        rgb.resize(bytes);
        uint32_t got = 0;
        int64_t t = 0;
        const mv_status s = h->frames_read(h->host, frames.get(), rgb.data(), rgb.size(), &t, &got);
        if (s != MV_OK) return mv::err(static_cast<mv::status>(s));
        pts_us = t;
        return got != 0;
      };
      return r;
    };
    p.create_clip = [h](const std::string& source, uint32_t w, uint32_t ht, uint32_t rate,
                        uint32_t channels) -> mv::result<run_port::clip_writer> {
      mv_addon_clip_out o{};
      o.struct_size = sizeof o;
      o.width = w;
      o.height = ht;
      o.sound_rate = rate;
      o.sound_channels = channels;
      o.suffix_ascii = "enhance";
      void* raw = nullptr;
      const mv_status st = h->clip_write_open(h->host, source.c_str(), &o, &raw);
      if (st != MV_OK || !raw) return mv::err(static_cast<mv::status>(st == MV_OK ? MV_ERR_INTERNAL : st));
      std::shared_ptr<void> wr(raw, [h](void* x) { h->clip_write_close(h->host, x); });
      run_port::clip_writer out;
      out.frame = [h, wr](const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts) -> mv::expected {
        const mv_status s = h->clip_write_frame(h->host, wr.get(), rgb, stride, pts);
        return s == MV_OK ? mv::expected{} : mv::expected{mv::err(static_cast<mv::status>(s))};
      };
      out.sound = [h, wr, channels](std::span<const float> pcm) -> mv::expected {
        if (channels == 0) return mv::err(mv::status::invalid_arg);
        const mv_status s = h->clip_write_sound(h->host, wr.get(), pcm.data(), static_cast<uint32_t>(pcm.size() / channels));
        return s == MV_OK ? mv::expected{} : mv::expected{mv::err(static_cast<mv::status>(s))};
      };
      out.finish = [h, wr]() -> mv::result<std::string> {
        char buf[4096] = {};
        const mv_status s = h->clip_write_finish(h->host, wr.get(), buf, sizeof buf);
        if (s != MV_OK) return mv::err(static_cast<mv::status>(s));
        return std::string(buf);
      };
      return out;
    };
  }
  // Photos and a clip's picture: one tiled model runner.
  p.load_restyle = [runtime](const model_spec& spec, const std::string& models_dir)
      -> result<std::unique_ptr<restyle_model>> {
    restyle_io io = restyle_io_of(spec);
    MV_TRY(infer::runtime * rt, runtime->runtime());
    return restyle_model::open(*rt, models_dir, models_dir + "-cache", std::move(io));
  };
  // Slice 7: the brush and the inpainting model.
  if (has_mask_row(h)) {
    p.still_mask = [h](const std::string& path) {
      std::uint32_t n = 0;
      if (h->still_mask(h->host, path.c_str(), nullptr, 0, &n) != MV_OK || n == 0) return std::vector<float>{};
      std::vector<float> dabs(std::size_t{n} * 3);
      if (h->still_mask(h->host, path.c_str(), dabs.data(), n, &n) != MV_OK) return std::vector<float>{};
      dabs.resize(std::min<std::size_t>(dabs.size(), std::size_t{n} * 3));
      return dabs;
    };
  }
  p.load_inpaint = [runtime](const model_spec& spec, const std::string& models_dir)
      -> result<std::unique_ptr<inpaint_filler>> {
    if (spec.weights.empty()) return mv::err(mv::status::unsupported_format);
    inpaint_io io;
    if (const auto side = spec.io.integer("side"); side && *side >= 64 && *side <= 2048) {
      io.side = static_cast<std::uint32_t>(*side);
    }
    if (const std::string* v = spec.io.str("image")) io.image = *v;
    if (const std::string* v = spec.io.str("mask")) io.mask = *v;
    if (const std::string* v = spec.io.str("output")) io.output = *v;
    if (const json::value* v = spec.io.find("out_scale"); v && v->k == json::kind::number) {
      io.out_scale = static_cast<float>(v->is_integer ? static_cast<double>(v->i) : v->d);
    }
    MV_TRY(infer::runtime * rt, runtime->runtime());
    MV_TRY(auto model, inpaint_model::open(*rt, models_dir + "/" + spec.weights.front(), io));
    return std::unique_ptr<inpaint_filler>(std::move(model));
  };
  p.load_tiles = [runtime](const model_spec& spec, const std::string& models_dir) {
    // Core ML's compiled model, kept beside the weights (Remove deletes it).
    return runtime->open_tiles(spec, models_dir, models_dir + "-cache");
  };
  return p;
}

}  // namespace mv::enhance
