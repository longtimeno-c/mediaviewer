// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A C++ face on the host function table (mediaviewer_addon.h, v2) for the AI
// pack. Every file the add-on reads, it reads through here: it links no part
// of the core (docs/design/18 "does not link the core statically").
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "core/result.h"

namespace mv::ai {

[[nodiscard]] inline status to_status(mv_status s) noexcept { return static_cast<status>(s); }

struct rgb_frame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
  std::int64_t pts_ms = -1;
  std::int64_t pts_tb = 0;
  std::int32_t tb_num = 0;
  std::int32_t tb_den = 1;
  std::uint32_t flags = 0;  // MV_ADDON_FRAME_*
};

class host {
 public:
  explicit host(const mv_host_api* api) noexcept : api_(api) {}
  [[nodiscard]] const mv_host_api* api() const noexcept { return api_; }
  [[nodiscard]] bool has_pixels() const noexcept;  // a v2 table

  struct entry {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool cloud_only = false;  // only a cloud provider has its bytes: reading it downloads it
  };
  // Files under `root` (depth 0 = the folder only, -1 unlimited), cloud-only
  // ones flagged when the host can say (walk_files2).
  [[nodiscard]] bool has_walk2() const noexcept;
  [[nodiscard]] expected walk(const std::string& root, int max_depth,
                              const std::function<bool(const entry&)>& visit) const;
  struct stat_result {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool is_directory = false;
  };
  [[nodiscard]] result<stat_result> stat(const std::string& path) const;
  [[nodiscard]] expected make_directories(const std::string& dir) const;
  [[nodiscard]] expected remove_file(const std::string& path) const;

  [[nodiscard]] result<rgb_frame> decode_still(const std::string& path, std::uint32_t max_edge) const;
  [[nodiscard]] result<rgb_frame> video_frame(const std::string& path, std::int64_t pts_ms,
                                              std::uint32_t max_edge) const;

  class sampler {
   public:
    ~sampler();
    sampler(const sampler&) = delete;
    sampler& operator=(const sampler&) = delete;
    [[nodiscard]] const mv_addon_video_info& info() const noexcept { return info_; }
    // The next frame; flags & MV_ADDON_FRAME_END at the end.
    [[nodiscard]] result<rgb_frame> next();

   private:
    friend class host;
    sampler(const mv_host_api* api, void* handle, mv_addon_video_info info)
        : api_(api), handle_(handle), info_(info) {}
    const mv_host_api* api_;
    void* handle_;
    mv_addon_video_info info_;
    std::vector<std::uint8_t> buf_;
  };
  [[nodiscard]] result<std::unique_ptr<sampler>> open_sampler(
      const std::string& path, const mv_addon_sampler_options& options) const;

  // The viewer's JPEG-512 for a still, or for a moment (stored when `frame`).
  [[nodiscard]] result<std::string> thumbnail(const std::string& path) const;
  [[nodiscard]] result<std::string> moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                                     const rgb_frame* frame) const;
  [[nodiscard]] result<std::string> piece_dir(const std::string& piece) const;
  // Sharing an index (docs/design/17): the cached JPEG-512 bytes of a still
  // (pts_ms < 0) or a moment, never made (status::io on a miss); and bytes
  // from an export stored under this machine's stamp of the file.
  [[nodiscard]] bool has_thumbnail_bytes() const noexcept;
  [[nodiscard]] result<std::vector<std::uint8_t>> thumbnail_jpeg(const std::string& path,
                                                                 std::int64_t pts_ms) const;
  [[nodiscard]] expected store_thumbnail_jpeg(const std::string& path, std::int64_t pts_ms,
                                              std::span<const std::uint8_t> jpeg) const;

  // #291: a file's tag facts from the host's meta_cache.db (an older host
  // has none: has_tag_facts() false).
  [[nodiscard]] bool has_tag_facts() const noexcept;
  [[nodiscard]] result<mv_addon_tag_facts> tag_facts(const std::string& path) const;

  // #406: a clip's chapter titles and text subtitle cues, in start order (an
  // older host has none: has_cues() false). `visit` false stops the read.
  struct cue {
    std::uint32_t kind = MV_ADDON_CUE_SUBTITLE;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;
  };
  [[nodiscard]] bool has_cues() const noexcept;
  [[nodiscard]] expected read_cues(const std::string& path, std::int64_t start_ms, std::int64_t max_ms,
                                   std::uint32_t max_cues, const std::function<bool(const cue&)>& visit) const;

  // #354: a document's text from the host (a DOCX; a PDF is the pack's own,
  // pdf_reader.h), a paragraph at a time with its page. An older host has
  // none: has_document_text() false. `visit` false stops the read.
  struct doc_line {
    std::uint32_t page = 0;
    std::uint32_t page_count = 1;
    std::string text;
  };
  [[nodiscard]] bool has_document_text() const noexcept;
  [[nodiscard]] expected document_text(const std::string& path,
                                       const std::function<bool(const doc_line&)>& visit) const;

  [[nodiscard]] bool should_yield() const noexcept;
  void post(mv_addon_event_kind kind, mv_status status, std::uint64_t id,
            std::int64_t payload) const noexcept;
  [[nodiscard]] result<std::string> data_dir() const;

 private:
  const mv_host_api* api_;
};

}  // namespace mv::ai
