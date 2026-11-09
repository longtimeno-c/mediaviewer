// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Instagram Post window's state and work (docs/design/30). Events arrive on
// the host's worker for this add-on and return at once; listings, pictures,
// Local search and the export run on the engine's own threads (rule 1: none
// of it touches the UI or render thread). The host is behind `port`, so the
// tests drive all of it with fakes. Nothing here logs a path (rule 6).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/instagram/pixels.h"
#include "addons/instagram/post.h"
#include "addons/instagram/view.h"

namespace mv::instagram {

// One clip being read, frame by frame.
class clip_reader {
 public:
  virtual ~clip_reader() = default;
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;   // coded
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t rotation() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t frame_count() const noexcept = 0;  // an estimate
  // false at the end or on failure (`failed` then says which).
  [[nodiscard]] virtual bool next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us, bool& failed) = 0;
};

class clip_writer {
 public:
  virtual ~clip_writer() = default;
  [[nodiscard]] virtual bool frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us) = 0;
  // The new file's path, or "" when it failed.
  [[nodiscard]] virtual std::string finish() = 0;
};

struct engine_port {
  std::function<bool(std::uint64_t gen, const std::string& tree)> post;
  std::function<bool(std::uint64_t gen, const std::string& diff)> diff;
  std::function<void(std::uint64_t gen, const std::string& surface, const std::vector<std::uint8_t>& bgra,
                     std::uint32_t w, std::uint32_t h)>
      surface;
  std::function<void()> show;
  // Rows, in any order for a folder (the engine sorts), newest first for the library.
  std::function<bool(const std::string& folder, const std::function<void(gallery_row)>& add)> walk;
  // Empty: this host has no library. false: not added, or not readable.
  std::function<bool(const std::function<void(gallery_row)>& add)> library;
  std::function<bool(const std::string& path, std::uint32_t max_edge, image& out)> preview;
  // Empty or false: no Local search; the name filter still answers.
  std::function<bool(const std::string& query, const std::string& scope, std::vector<std::string>& out)> search;
  // The whole picture, upright. Empty: this host cannot export.
  std::function<bool(const std::string& path, image& out)> still;
  std::function<std::string(const image& pic, const std::string& dir, const std::string& stem)> write_still;
  std::function<std::string(const std::string& path)> library_file;  // "" when it cannot be had
  std::function<std::unique_ptr<clip_reader>(const std::string& file)> open_clip;
  std::function<std::unique_ptr<clip_writer>(const std::string& file, std::uint32_t w, std::uint32_t h,
                                             const std::string& dir, const std::string& stem)>
      write_clip;
  std::function<bool(const std::string& dir)> make_dirs;
  std::function<void(std::uint64_t gen, const std::vector<std::string>& paths)> open_list;
  // The Edit workspace's colour chain on packed RGB, in place, and its Auto.
  std::function<bool(std::uint8_t* rgb, std::uint32_t w, std::uint32_t h, const adjustments& values)> adjust;
  std::function<bool(const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h, adjustments& out)> auto_adjust;
  // Enhance's options, drawn in this window. false: not installed, or too old.
  std::function<bool(std::uint64_t gen, const std::string& path)> embed_open;
  std::function<void()> embed_close;
  std::function<void(const char* what)> busy;  // "" when it ends
  std::function<void(int level, const char* message)> log;
  std::string library_dir;  // where a post's folder goes
  // The folder's name for this export (a test's fixed clock).
  std::function<std::string()> stamp;
};

struct screen_facts {
  std::uint64_t generation = 0;
  std::string path;  // the file on screen, or ""
  std::uint32_t accent = 0x3478F6FF;
  float scale = 2;
};

class engine {
 public:
  explicit engine(engine_port port);
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  void opened(const screen_facts& facts);
  void closed();
  // The "open" command or the context entry: these items, then the window.
  void add_and_show(const std::vector<std::string>& paths);
  // The "open" command: marks start the post; a lone path is the file on
  // screen, which says where to choose from and is not itself chosen.
  void open_command(const std::vector<std::string>& paths);
  void click(const std::string& node);
  void change(const std::string& node, const std::string& value_json);
  void select(const std::string& value_json);
  void pan(const std::string& node, double dx, double dy);
  void zoom(const std::string& node, double scale);
  void tap(const std::string& node);
  void reset_frame(const std::string& node);
  // Enhance made a new file or library photo from a chosen item.
  void item_saved(const std::string& source, const std::string& result);

  // Tests: every queued job done (the export too).
  void wait_idle();
  [[nodiscard]] std::vector<std::string> written() const;

 private:
  class lane {
   public:
    lane();
    ~lane();
    void push(std::string key, std::function<void()> job);  // a newer job with the key replaces a queued one
    void stop();
    void wait_idle();

   private:
    void run();
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::function<void()>>> jobs_;
    bool quit_ = false;
    bool busy_ = false;
    std::thread t_;
  };

  struct picture {
    std::shared_ptr<const image> pic;
  };

  view_state view_locked();
  void publish_strip_locked();
  void publish_gallery_locked();
  void publish_progress_locked();
  void publish_panel_locked();
  void focus_changed_locked();
  void sync_enhance_locked();
  void refilter_locked();
  void request_preview_locked(const chosen& c);
  void request_render_locked(std::uint64_t uid);
  void render(std::uint64_t uid);
  void load_lists(std::uint64_t gen, std::string folder, bool wants_library);
  void search_later(std::uint64_t ticket, std::string query, std::string scope);
  void add_pending_locked();
  void export_all(std::vector<chosen> items, shape s);
  bool export_clip(const chosen& c, shape s, const std::string& dir, const std::string& stem, std::size_t index,
                   std::size_t total, std::string& out);
  void note_progress(std::size_t index, std::size_t total, double within);
  [[nodiscard]] const std::vector<gallery_row>& active_locked() const;

  engine_port port_;
  mutable std::mutex m_;
  post post_;
  bool open_ = false;
  std::uint64_t gen_ = 0;
  std::uint32_t accent_ = 0x3478F6FF;
  float scale_ = 2;
  source from_ = source::folder;
  std::string folder_;
  std::string hint_;  // the file the open command named
  std::vector<gallery_row> folder_rows_;
  std::vector<gallery_row> library_rows_;
  bool has_library_ = false;
  bool loading_ = false;
  std::string query_;
  std::uint64_t search_ticket_ = 0;
  std::vector<std::string> hits_;  // Local search's answer for query_, best first
  std::vector<const gallery_row*> shown_rows_;
  std::size_t page_ = kGalleryPage;
  std::vector<std::string> pending_;  // marked before the lists arrived
  std::map<std::uint64_t, picture> pictures_;
  std::vector<std::uint64_t> unreadable_;
  std::string note_;
  tool_mode mode_ = tool_mode::choose;
  bool enhance_missing_ = false;
  std::string embed_path_;  // the photo Enhance's panel is open on
  bool can_show_ = false;
  std::vector<std::string> written_;
  bool exporting_ = false;
  double progress_ = 0;
  std::string progress_words_;
  std::atomic<bool> cancel_{false};
  std::thread export_;
  lane work_;
  lane finder_;
};

}  // namespace mv::instagram
