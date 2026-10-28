// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): the add-on's state behind its two screens. It reads the
// models this build ships (models/<kind>/model.json in its own folder) and
// their pins (its signed manifest's `remote`), remembers the person's choices
// (prefs.h), runs each kind's download (downloads.h), and builds the trees
// (screens.h). The host function table is reached only through `port`, so
// tests drive it with fakes.
//
// Threads: every call below comes on the host's worker for this add-on, one
// at a time; downloads report from their own threads. One lock guards the
// state; it is never held across a download's stop (which joins a thread that
// reports back) nor across a probe of a file.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/enhance/downloads.h"
#include "addons/enhance/prefs.h"
#include "addons/enhance/preview.h"
#include "addons/enhance/screens.h"
#include "addons/enhance/sniff.h"
#include "addons/enhance/spec.h"

namespace mv::enhance {

// What a clip holds, from the host's own decoders (sampler_open, audio_open).
struct media_facts {
  bool picture = false;
  bool sound = false;
};

struct engine_port {
  // screen_post: false when the generation is stale.
  std::function<bool(const char* screen, std::uint64_t generation, const std::string& tree)> post;
  // surface_post: BGRA (premultiplied, sRGB) for a surface of a screen.
  std::function<bool(const char* screen, const char* surface, std::uint64_t generation,
                     const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t height)>
      surface;
  // screen_diff against the tree last posted (a running job's progress).
  std::function<bool(const char* screen, std::uint64_t generation, const std::string& diff)> diff;
  std::function<void(const char* screen, bool show)> show;
  // [worker] What a file holds; called for an item that sniffs as media or audio.
  std::function<media_facts(const std::string& path)> probe;
  std::function<std::optional<std::uint64_t>(const std::string& dir)> free_disk;
  std::function<std::unique_ptr<ai::http_transport>()> transport;
  // Never a path or a name (rule 6).
  std::function<void(int level, const char* ascii)> log;
};

// One job the sheet starts (slices 3-5): what to enhance and with which
// values. The runner reports through the callbacks, from any thread.
struct job_request {
  std::string path;
  item_kind item = item_kind::unknown;
  clip_target target = clip_target::picture;
  // The models it runs, and the values chosen for each.
  struct part {
    model_kind kind = model_kind::still;
    const model_spec* spec = nullptr;
    std::string models_dir;  // where that kind's weights are: <data>/models/<kind>
    std::map<std::string, json::value> values;
  };
  std::vector<part> parts;
};

struct job_events {
  std::function<void(double fraction)> progress;  // -1 indeterminate
  // Finished: `output_name` is the new file's name (shown); `message` says why
  // when it did not finish ("" for a cancel).
  std::function<void(bool ok, const std::string& output_name, const std::string& message)> done;
  // A picture run's latest frame, small (preview.h): the original and the
  // enhanced picture. About once a second, from the job's thread.
  std::function<void(preview_image before, preview_image after)> preview;
};

class runner {
 public:
  virtual ~runner() = default;
  // This build can run a model of this kind on this item.
  [[nodiscard]] virtual bool supports(model_kind k, item_kind item) const = 0;
  // Empty when it may start, else why not ("Needs 3.1 GB of free memory").
  [[nodiscard]] virtual std::string check(const job_request& r) const = 0;
  virtual void start(job_request r, job_events events) = 0;
  virtual void cancel() = 0;
};

class engine {
 public:
  // `self_dir`: the add-on's verified folder (manifest.json, models/).
  // `data_dir`: its data folder (enhance.json, models/ fetched).
  engine(engine_port port, std::string self_dir, std::string data_dir, ai::fetch_options fetch = {});
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  // Jobs (slices 3-5). Without one, Enhance shows its sheet and installs its
  // models but cannot run them.
  void set_runner(std::unique_ptr<runner> r);

  // Resumes the downloads the person asked for before a restart, on a thread
  // of its own (nothing is fetched that was never clicked).
  void resume_wanted();

  void settings_opened(std::uint64_t generation);
  // `width_pt` / `scale`: the sheet's content width and backing scale, for
  // the preview's pixels.
  void sheet_opened(std::uint64_t generation, const std::string& path, float width_pt = 420, float scale = 2);
  void closed(const std::string& screen);
  // The command or the context entry: these files (the first is enhanced),
  // then the sheet.
  void command(const std::vector<std::string>& paths);
  void event(const std::string& screen, const std::string& node, std::uint32_t kind, const std::string& value_json);

  // For tests: the specs this build found, and a kind's download status.
  [[nodiscard]] const model_spec* spec(model_kind k) const;
  [[nodiscard]] dl_status download_status(model_kind k) const;
  [[nodiscard]] prefs current_prefs() const;

 private:
  void on_changed(model_kind k);
  void post_settings_locked();
  void post_sheet_locked();
  [[nodiscard]] sheet_view sheet_locked() const;
  [[nodiscard]] std::vector<model_kind> kinds_for_locked() const;
  void save_locked();
  void install(model_kind k);
  void stop(model_kind k);
  void remove(model_kind k);
  void run();
  void cancel();

  engine_port port_;
  std::string self_dir_;
  std::string data_dir_;
  std::array<std::optional<model_spec>, 3> specs_;
  std::array<model_files, 3> files_;
  std::unique_ptr<downloads> dl_;
  std::unique_ptr<runner> runner_;
  std::thread resume_;

  mutable std::mutex m_;
  prefs prefs_;
  std::uint64_t settings_gen_ = 0;
  std::uint64_t sheet_gen_ = 0;
  std::string pending_path_;  // from a command, for the sheet it opens
  std::string path_;          // the item the sheet is about
  item_kind item_ = item_kind::unknown;
  media_facts facts_;
  bool running_ = false;
  double progress_ = -1;
  std::chrono::steady_clock::time_point run_started_{};
  std::string eta_;  // "About 12 minutes left", once a run has gone far enough to say
  bool preview_ = false;  // the sheet shows a preview surface
  float sheet_width_pt_ = 420;
  float sheet_scale_ = 2;
  std::string status_;
};

}  // namespace mv::enhance
