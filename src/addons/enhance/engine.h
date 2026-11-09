// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): the add-on's state behind its two screens. It reads the
// models this build ships (models/<kind>/<id>.json in its own folder) and
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
#include <condition_variable>
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
#include "addons/enhance/eta.h"
#include "addons/enhance/prefs.h"
#include "addons/enhance/preview.h"
#include "addons/enhance/screens.h"
#include "addons/enhance/sniff.h"
#include "addons/enhance/spec.h"
#include "addons/enhance/tier.h"

namespace mv::enhance {

// What a clip holds, from the host's own decoders (sampler_open, audio_open).
struct media_facts {
  bool picture = false;
  bool sound = false;
};

struct engine_port {
  // docs/design/30: a run from the embedded panel made `result` from `source`.
  std::function<void(const std::string& source, const std::string& result)> saved;
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
  // [worker] Slice 7: how many brush dabs the host holds over the photo at
  // `path` (Edit → Adjust → Remove); 0 none.
  std::function<std::uint32_t(const std::string& path)> brushed;
  // [worker] S5's Discard: the host table's recycle_file. 0 moved to the
  // Bin / Trash, 1 refused (the place has no bin: nothing was removed), -1
  // failed. Never a permanent delete.
  std::function<int(const std::string& path)> recycle;
  // edit-refresh S6: whether the host holds a frame dragged past this
  // photo's edges (the host table's still_extend): the sheet then opens on
  // Extend, as it opens on Remove when something is brushed.
  std::function<bool(const std::string& path)> framed;
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
  // From the Video Editor: the edit's kept source ranges, (in, out) pairs in
  // nanoseconds, in source order. Empty: the whole clip.
  std::vector<std::int64_t> keep_ns;
  // The performance level's bounds (tier.h).
  tier_policy policy = policy_of(tier::standard);
  bool has_sound = false;  // a clip: it holds sound (a cut one's is written again)
};

struct job_events {
  std::function<void(double fraction)> progress;  // -1 indeterminate
  // Finished: `output_name` is the new file's name (shown); `message` says why
  // when it did not finish ("" for a cancel).
  std::function<void(bool ok, const std::string& output_name, const std::string& message)> done;
  // A picture run's latest frame, small (preview.h): the original and the
  // enhanced picture. About once a second, from the job's thread.
  std::function<void(preview_image before, preview_image after)> preview;
  // A photo's run, several times a second: the picture so far (preview.h
  // partial_of) and how far the run has come.
  std::function<void(preview_image partial, reveal_front at)> reveal;
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
  // One-click: the photo's size after its edit and how far it shrinks without
  // losing detail, for the card's time estimate. On the engine's own estimate
  // thread, never while a job runs; nullopt when it cannot read it.
  [[nodiscard]] virtual std::optional<photo_size> measure(const std::string& path) {
    (void)path;
    return std::nullopt;
  }
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
  void sheet_opened(std::uint64_t generation, const std::string& path, float width_pt = 420, float scale = 2,
                    bool panel = false);
  // S5: the docked panel (place "edit") opened on `path`, `width_pt` wide.
  void tool_opened(std::uint64_t generation, const std::string& path, float width_pt = 340, float scale = 2);
  // S5: the Edit workspace's command to it, enhance/tool:
  // {"photo": "remove|restyle|detail|extend|faces", "run": true?, "path": ?}.
  // Chooses the tool (Remove reads the brushed dabs again) and, with "run",
  // starts it (one-click: downloading its model first when it is not here).
  // "path" (one-click): the photo now on screen; the cards follow it (a run
  // on another photo stops, as leaving it does; the result it opened keeps
  // its notice).
  void tool_command(const std::string& value_json);
  void closed(const std::string& screen);
  // The command or the context entry: these files (the first is enhanced),
  // then the sheet. From the Video Editor, `keep_ns` is the edit (job_request).
  // `start_on`: the wand menu's Restyle opens the sheet on it (when offered).
  // The level in force: the chosen one, else the recommendation.
  [[nodiscard]] tier effective_tier() const;
  void command(const std::vector<std::string>& paths, std::vector<std::int64_t> keep_ns = {},
               std::optional<photo_target> start_on = std::nullopt);
  void event(const std::string& screen, const std::string& node, std::uint32_t kind, const std::string& value_json);

  // For tests: the model each kind uses (its chosen one), and its download
  // status.
  [[nodiscard]] const model_spec* spec(model_kind k) const;
  [[nodiscard]] dl_status download_status(model_kind k) const;
  [[nodiscard]] prefs current_prefs() const;

 private:
  // One model of a kind: its spec (id set), files and download key.
  struct model_entry {
    model_spec spec;
    model_files files;
    std::string key;  // "<kind>.<id>"
  };
  [[nodiscard]] const model_entry* chosen_locked(model_kind k) const;
  [[nodiscard]] const model_entry* find_model(model_kind k, const std::string& id) const;
  // The chosen model's spec, files and download status (m_ held).
  [[nodiscard]] const model_spec* spec_locked(model_kind k) const;
  [[nodiscard]] model_files files_locked(model_kind k) const;
  [[nodiscard]] dl_status status_locked(model_kind k) const;
  // A download moved, or a click's download should be looked at: posts the
  // screens, and a one-click download that finished starts its run.
  void on_changed();
  void post_settings_locked();
  // Posts the sheet and the docked panel, whichever are open.
  void post_sheet_locked();
  // `docked`: the panel's view (Remove stays Remove with nothing brushed yet).
  // `as`: that tool's view rather than the chosen one (one-click's cards).
  [[nodiscard]] sheet_view sheet_locked(bool docked = false, std::optional<photo_target> as = std::nullopt) const;
  [[nodiscard]] std::vector<model_kind> kinds_for_locked(bool docked = false,
                                                        std::optional<photo_target> as = std::nullopt) const;
  // One-click: the Enhance tab's cards and what the run is doing.
  [[nodiscard]] std::vector<card_view> cards_locked() const;
  [[nodiscard]] run_view run_view_locked() const;
  [[nodiscard]] std::string card_eta_locked(photo_target t, const sheet_view& v) const;
  // A card's one button: the model first when it is not here, then the run.
  void go(photo_target t);
  void measure_loop();
  void want_measure_locked();
  // A screen opened on `path`: what it is, read before the lock.
  struct item_facts {
    item_kind item = item_kind::unknown;
    media_facts facts;
    std::uint32_t brushed = 0;
    bool framed = false;  // S6: the host holds an Extend frame for it
  };
  [[nodiscard]] item_facts read_item(const std::string& path) const;
  void adopt_locked(const std::string& path, const item_facts& f, std::vector<std::int64_t> keep);
  void discard();
  void forget_result_locked();
  [[nodiscard]] bool detail_offered(model_kind k) const;
  [[nodiscard]] model_kind detail_locked() const;
  void save_locked();
  // `id` empty: the kind's chosen model.
  void install(model_kind k, const std::string& id);
  void stop(model_kind k, const std::string& id);
  void remove(model_kind k, const std::string& id);
  void choose(model_kind k, const std::string& id);
  void run(bool docked = false);
  void cancel();
  void end_pending_locked(run_state st, std::string message);

  engine_port port_;
  std::string self_dir_;
  std::string data_dir_;
  // Per kind: model.json's first, then the others, smallest download first.
  // Fixed after construction: read without the lock.
  std::array<std::vector<model_entry>, kKindCount> models_;
  std::unique_ptr<downloads> dl_;
  std::unique_ptr<runner> runner_;
  std::thread resume_;

  mutable std::mutex m_;
  prefs prefs_;
  std::uint64_t settings_gen_ = 0;
  const char* sheet_screen_ = kSheetScreen;  // or kPanelScreen, drawn in another add-on's window
  std::uint64_t sheet_gen_ = 0;
  std::uint64_t tool_gen_ = 0;  // S5: the docked panel's generation, 0 closed
  std::string pending_path_;  // from a command, for the sheet it opens
  std::vector<std::int64_t> pending_keep_;
  std::string path_;          // the item the sheet is about
  std::vector<std::int64_t> keep_;  // the Video Editor's edit of it, or empty
  item_kind item_ = item_kind::unknown;
  media_facts facts_;
  tier recommended_ = tier::standard;  // this computer's, by Local search's rule
  std::uint32_t brushed_ = 0;  // slice 7: dabs brushed over the photo, when the sheet opened
  bool framed_ = false;        // edit-refresh S6: a canvas frame over the photo, when the sheet opened
  // edit-refresh S8: the faces the host's Faces panel chose for Restore face
  // (x, y, w, h quads, 0..1); empty every face. Set by each tool command.
  std::vector<float> face_boxes_;
  bool running_ = false;
  double progress_ = -1;
  std::chrono::steady_clock::time_point run_started_{};
  std::string eta_;  // "About 12 minutes left", once a run has gone far enough to say
  bool preview_ = false;  // the sheet shows a preview surface
  bool preview_split_ = false;  // ...split before / after (its labels show)
  double preview_aspect_ = 0;   // the picture's width / height, once known
  std::uint32_t posted_height_ = 0;  // the preview's height in the last tree
  float sheet_width_pt_ = 420;
  float sheet_scale_ = 2;
  float tool_width_pt_ = 340;
  float tool_scale_ = 2;
  std::string status_;
  // S5: the file the last run wrote (absolute, or a library key), for the
  // docked panel's result row; "" none or discarded.
  std::string result_path_;
  std::string result_name_;
  std::string result_note_;  // Discard refused or failed: why, under the row
  // One-click (owner 2026-11-05): the run the cards' host follows. Every run,
  // and every download a click started to run after, has a new id.
  run_state run_state_ = run_state::idle;
  std::uint64_t run_id_ = 0;
  photo_target run_tool_ = photo_target::detail;
  std::string run_message_;
  std::string run_eta_;
  std::string source_path_;  // the photo the last run was made from
  // The photo the cards were pointed at while a run went on another: the
  // run stops, and the cards follow once it has.
  std::string follow_path_;
  // Download, then run: the tool waiting for its model, and which model.
  std::optional<photo_target> pending_tool_;
  model_kind pending_kind_ = model_kind::still;
  // The card's estimate: each photo's size and detail, measured on a thread
  // of the engine's own (never the host's worker), the newest few kept.
  std::map<std::string, std::optional<photo_size>> measured_;
  std::string measure_want_;
  bool measure_quit_ = false;
  std::condition_variable measure_cv_;
  std::thread measure_t_;
};

}  // namespace mv::enhance
