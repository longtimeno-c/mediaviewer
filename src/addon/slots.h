// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens and slots: the host side (docs/design/25 §7, §10, §11; PRs 58-59).
//
// One registry per process holds what every loaded add-on contributes, as
// tables built once at attach (on the loader's thread, never the UI thread),
// and what each add-on has posted since: view trees, surface pixels, search
// rows, badge sets, status texts. The chromes read snapshots (JSON) and are
// told through one listener when something changed; they never wait on an
// add-on.
//
// How it reaches an add-on's code is a slot_client: today MediaViewer's own
// native add-ons through "mv.slots.1" (option B of §9, the host function table),
// later a script add-on's worker (PR 57) through the same interface. Each
// attached add-on gets ONE worker thread here; every call into its client
// runs there, one at a time. Requests of one kind coalesce (a newer info,
// search or badges request replaces the one still queued), so a slow
// provider costs typing nothing: the chrome posts and returns, and an answer
// for a generation that is no longer current is dropped.
//
// Performance rules (§11) as code:
//   - nothing is asked of an add-on per frame, per tile or per key: badges
//     are one request per folder and a table the gallery reads
//     (`addon_calls` counts every call, for the PR 59 verify);
//   - an info section that needs a location is not asked for a file
//     without one;
//   - an idle add-on costs nothing: its worker sleeps on a condition
//     variable, no timer, no poll;
//   - detach removes every item at once, with no restart.
// Portable (D9).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/contrib.h"
#include "addon/screen.h"
#include "core/result.h"
#include "core/status.h"

namespace mv::addon {

// How the registry calls an add-on. Every method runs on the add-on's worker.
class slot_client {
 public:
  virtual ~slot_client() = default;
  virtual status info_section(const mv_slot_info_request& request) = 0;
  virtual status search(const mv_slot_search_request& request) = 0;
  virtual status screen_open(const mv_slot_screen_request& request) = 0;
  virtual status badges(const mv_slot_badges_request& request) = 0;
  virtual void event(const mv_slot_event& event) = 0;
};

// Over a native add-on's mv.slots.1 table (which lives until its shutdown).
[[nodiscard]] std::unique_ptr<slot_client> native_slot_client(void* addon, const mv_addon_slots* table);

// What changed, for the listener.
enum slot_change : std::uint32_t {
  slot_changed_contributions = 1u << 0,  // an add-on attached or detached
  slot_changed_info = 1u << 1,           // an info section's tree or surface
  slot_changed_screens = 1u << 2,        // a window / pane / sheet opened, closed or changed
  slot_changed_search = 1u << 3,
  slot_changed_badges = 1u << 4,
  slot_changed_status = 1u << 5,
  slot_changed_notices = 1u << 6,  // #452: a notice, the keep-alive list or a permission ask
};

struct surface_pixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t revision = 0;  // unique across the registry's surfaces
  std::vector<std::uint8_t> bgra;  // width * 4 per row, premultiplied
};

// "48.85837° N, 2.29448° E" (meta::format_coordinate, both hosts' pane text)
// back to degrees. nullopt when it is not that shape.
[[nodiscard]] std::optional<mv_addon_location> parse_location_text(std::string_view text) noexcept;

// What a query is compared as: lowercase, Latin diacritics dropped (the
// hosts' FoldedNames / FoldName, for the letters a place or tag uses).
[[nodiscard]] std::string fold_query(std::string_view text);

// #441: the Local search query names a place: a place:x term, or in:<word>
// where the word does not start with a digit (in:2024 is a date), kept or
// left out (-place:spain), outside "quotes". The AI pack's parser
// (src/addons/ai/query.cpp) reads the same terms; the pack cannot be linked
// here, so this is its rule for the operators alone.
[[nodiscard]] bool has_place_terms(std::string_view query);

class slot_registry {
 public:
  slot_registry();
  ~slot_registry();
  slot_registry(const slot_registry&) = delete;
  slot_registry& operator=(const slot_registry&) = delete;

  // ---- add-ons ---------------------------------------------------------------
  // `package_dir`: the add-on's verified folder (an `image` node's src is
  // resolved inside it). Refuses (invalid_arg) an add-on already attached or
  // with nothing to contribute.
  status attach(const std::string& addon_id, const std::string& addon_name, contributions c,
                std::unique_ptr<slot_client> client, std::string package_dir = {});
  // Removes every item the add-on contributed and stops its worker. On return
  // no call into its client is running and none will run, so the library may
  // be unloaded. Idempotent.
  void detach(const std::string& addon_id);
  [[nodiscard]] bool attached(const std::string& addon_id) const;
  [[nodiscard]] std::vector<std::string> attached_ids() const;

  // Called on any thread with slot_change bits; it must not block (a host
  // marshals to its UI thread). One listener; empty clears it.
  void set_listener(std::function<void(std::uint32_t)> listener);

  // ---- the chrome asks (any thread, never blocks) ------------------------------
  // The Metadata pane shows `path`. Returns the request's generation.
  std::uint64_t request_info(const std::string& path, const std::string& location_text,
                             const mv_slot_theme& theme, float width_pt, float scale);
  // The pane closed or shows nothing: sections go, nothing is asked.
  void clear_info();
  // `addon_id`'s settings changed (docs/design/25 "A changed setting re-asks";
  // the registry hears it from settings_values' listener, any thread): its
  // info sections for the file on screen are asked again under a new
  // generation, and its badges for the folder shown, if it has any. What it
  // showed stays until it answers; other add-ons are not asked. Never blocks.
  void settings_changed(const std::string& addon_id);
  // File search (MV_SLOT_SCOPE_FILES) or Local search (MV_SLOT_SCOPE_LOCAL).
  // An empty query clears the results and asks nobody.
  // A Local search query with place terms (has_place_terms) asks nobody:
  // the AI pack intersects them itself through place_paths (#441), so no
  // provider's rows are offered beside its results.
  std::uint64_t search(std::uint32_t scope, const std::string& query, const std::string& folder);
  // #441: whether an attached add-on's provider for `scope` declares `prefix`
  // ("place:").
  [[nodiscard]] bool answers_prefix(std::uint32_t scope, std::string_view prefix) const;
  // #441 (the host table's place_matches): every path the Local search
  // provider that declares `prefix` answers for `query` (the prefix already
  // removed) over the whole index (folder ""). Blocks the calling worker
  // until that provider is done; the request has its own generation, so
  // neither the chrome's searches nor this one replaces the other.
  // `keep_going` is asked about every 20 ms and before returning; false
  // stops it (status::cancelled), and the provider sees the request go stale.
  // not_found when no attached add-on declares the prefix, or it detaches
  // meanwhile.
  [[nodiscard]] result<std::vector<std::string>> provider_paths(const std::string& prefix, const std::string& query,
                                                                const std::function<bool()>& keep_going);
  // The gallery shows `folder`: one badges request per add-on with badges.
  std::uint64_t request_badges(const std::string& folder);
  status open_screen(const std::string& addon_id, const std::string& screen_id,
                     const std::string& path, const mv_slot_theme& theme, float width_pt,
                     float height_pt, float scale);
  status close_screen(const std::string& addon_id, const std::string& screen_id);
  // A control in a screen or info section changed. Checked against the tree
  // (screen::tree::check_event) before the add-on hears of it.
  status send_event(const std::string& addon_id, const std::string& screen_id,
                    const std::string& node_id, std::uint32_t kind, const std::string& value_json);
  // A bar / path / context / status item was chosen. `paths_json` is a JSON
  // array of the files it targets ("[]" for none).
  status invoke_item(const std::string& addon_id, const std::string& item_id,
                     const std::string& paths_json);
  // One of the add-on's manifest commands ran (the palette, a key, Settings'
  // Open). The add-on gets a command event (node = the command id, value =
  // `paths_json`, the files its payload names: the marks, or the marks or
  // the current file). not_found for an add-on or command it does not have.
  status run_command(const std::string& addon_id, const std::string& command_id,
                     const std::string& paths_json);

  // ---- the chrome reads (any thread; copies) -----------------------------------
  // {"addons":[{"id","name","dir"}],"sections":[{"addon","id","title","location"}],
  //  "screens":[{"addon","id","title","place","width","height"}],
  //  "providers":[{"addon","id","name","prefixes":[],"files","local"}],
  //  "items":[{"addon","id","slot":"bar|path|context|status","label","symbol","opens","target"}],
  //  "badges":[{"addon","id","key","label","symbol"}],"local_search":bool}
  [[nodiscard]] std::string contributions_json() const;
  // {"generation","path","sections":[{"addon","id","title","revision","tree":{..}}]}
  // -- only the sections that have answered for the current file, in order.
  [[nodiscard]] std::string info_json() const;
  // [{"addon","id","title","place","width","height","generation","revision","pixels","tree":{..}|null}]
  [[nodiscard]] std::string screens_json() const;
  [[nodiscard]] bool surface(const std::string& addon_id, const std::string& screen_id,
                             const std::string& surface_id, surface_pixels& out) const;
  // The same surface's size and revision without copying its pixels (a chrome
  // that already holds that revision draws what it has).
  [[nodiscard]] bool surface_size(const std::string& addon_id, const std::string& screen_id,
                                  const std::string& surface_id, std::uint32_t& width, std::uint32_t& height,
                                  std::uint64_t& revision) const;
  // {"generation","scope","query","searching":bool,
  //  "providers":[{"addon","id","name","done","read","total"}],
  //  "rows":[{"path","why","addon","provider"}]}
  [[nodiscard]] std::string search_json(std::uint32_t scope) const;
  // {"generation","folder","defs":[{"key","label","symbol"}],"tiles":{"<path>":["addon/badge",...]}}
  [[nodiscard]] std::string badges_json() const;
  // The badge keys of one file, from the table (what a tile asks: never the add-on).
  [[nodiscard]] std::vector<std::string> badges_for(const std::string& path) const;
  // [{"addon","id","label","text","opens"?}] for the status items with text. "opens"
  // (#512): the screen a click opens, the item's own or the add-on's only
  // window / pane / sheet; absent when there is none (the item stays text).
  [[nodiscard]] std::string status_json() const;
  // #452: {"ask": n, "keep": [{"addon","screen","title","label"}],
  //  "notices": [{"addon","id","screen","title","body","action","action_label","revision"}]}.
  [[nodiscard]] std::string notices_json() const;
  // #452: some add-on asks to keep the process with every window closed.
  [[nodiscard]] bool keeping_alive() const;
  // #452: a notice's button was clicked: MV_SLOT_EVENT_NOTICE to its add-on.
  status notice_action(const std::string& addon_id, const std::string& notice_id, const std::string& action_id);

  // ---- the add-on answers (host table v3; any thread) ---------------------------
  status post_screen(const std::string& addon_id, const std::string& screen_id,
                     std::uint64_t generation, std::string_view tree_json);
  status post_diff(const std::string& addon_id, const std::string& screen_id,
                   std::uint64_t generation, std::string_view diff_json);
  status post_surface(const std::string& addon_id, const std::string& screen_id,
                      const std::string& surface_id, std::uint64_t generation,
                      const std::uint8_t* bgra, std::uint32_t width, std::uint32_t height,
                      std::uint32_t stride);
  struct search_row {
    std::string path;
    std::string why;
  };
  status post_search(const std::string& addon_id, const std::string& provider_id,
                     std::uint64_t generation, std::vector<search_row> rows, bool done);
  void search_progress(const std::string& addon_id, const std::string& provider_id,
                       std::uint64_t generation, std::uint32_t done, std::uint32_t total);
  [[nodiscard]] bool is_stale(std::uint64_t generation) const;
  // As above, and also true once `addon_id` is detached (or detaching): the
  // host table's is_stale / locations_in ask this, so a long call notices its
  // add-on is being removed and returns, and detach does not wait it out.
  [[nodiscard]] bool is_stale(const std::string& addon_id, std::uint64_t generation) const;
  status post_badges(const std::string& addon_id, const std::string& badge_id,
                     std::uint64_t generation, std::vector<std::string> paths, bool replace);
  status post_status(const std::string& addon_id, const std::string& item_id, std::string text);
  status show_screen(const std::string& addon_id, const std::string& screen_id, bool show);
  // #452 (host table notice_post / notice_authorize / keep_alive).
  struct notice_fields {
    std::string screen, title, body, action, action_label;
  };
  status post_notice(const std::string& addon_id, const std::string& notice_id,
                     std::optional<notice_fields> notice);
  status ask_notice_permission(const std::string& addon_id);
  status keep_alive(const std::string& addon_id, bool on, const std::string& screen_id, std::string label);

  // ---- for tests and the PR 59 verify --------------------------------------
  // Every call made into any add-on's client since construction.
  [[nodiscard]] std::uint64_t addon_calls() const noexcept { return calls_.load(); }
  // Blocks until every worker's queue is empty and idle (tests only).
  void drain();

 private:
  struct worker;
  struct screen_state {
    std::string addon;
    std::string id;
    screen_place place = screen_place::window;
    std::uint64_t generation = 0;
    screen::tree tree;
    std::map<std::string, surface_pixels> surfaces;
  };
  struct addon_entry;
  struct provider_state {
    std::string addon, id, name;
    bool done = false;
    std::uint32_t read = 0, total = 0;
  };
  struct search_state {
    std::uint64_t generation = 0;
    std::string query;
    std::vector<provider_state> providers;
    std::vector<std::pair<search_row, std::pair<std::string, std::string>>> rows;  // row, (addon, provider)
  };

  void notify(std::uint32_t bits);
  std::uint64_t next_generation() noexcept { return ++generation_; }
  // Under m_: the screen an add-on's post addresses (an info section of the
  // current file, or an open window / pane / sheet), with its generation checked.
  screen_state* addressed(const std::string& addon_id, const std::string& screen_id,
                          std::uint64_t generation, status& why);
  void enqueue(addon_entry& e, std::string key, std::function<void(slot_client&)> task);
  // Under m_: asks `e`'s info sections for the file on screen (info_path_ and
  // the inputs request_info stored) under generation `g`.
  void ask_info_locked(addon_entry& e, std::uint64_t g, const std::optional<mv_addon_location>& loc);
  status open_screen_locked(addon_entry& e, const screen_contrib& s, const std::string& path,
                            const mv_slot_theme& theme, float width_pt, float height_pt,
                            float scale);

  mutable std::mutex m_;
  std::map<std::string, std::unique_ptr<addon_entry>> addons_;
  std::function<void(std::uint32_t)> listener_;
  std::mutex listener_m_;

  // Info sections of the file on screen, and the request that asked for them
  // (settings_changed asks again with the same inputs).
  std::uint64_t info_generation_ = 0;
  std::string info_path_;
  std::string info_location_text_;
  mv_slot_theme info_theme_{};
  float info_width_pt_ = 0;
  float info_scale_ = 1.0f;
  std::map<std::pair<std::string, std::string>, screen_state> info_;  // (addon, section)
  // Windows, panes and sheets that are open.
  std::map<std::pair<std::string, std::string>, screen_state> screens_;
  search_state search_[2];  // [0] files, [1] local
  // provider_paths' requests in flight, by generation: what has arrived.
  struct private_search {
    std::string addon, provider;
    std::vector<std::string> paths;
    bool done = false;
  };
  std::map<std::uint64_t, private_search> private_;
  std::condition_variable private_cv_;  // with m_: a row or done arrived, or an add-on went
  std::uint64_t badges_generation_ = 0;
  std::string badges_folder_;
  std::map<std::string, std::set<std::string>> badge_sets_;  // "addon/badge" -> paths
  std::map<std::pair<std::string, std::string>, std::string> status_;
  struct notice_state {
    notice_fields f;
    std::uint64_t revision = 0;
  };
  std::map<std::pair<std::string, std::string>, notice_state> notices_;  // (addon, notice)
  std::map<std::string, std::pair<std::string, std::string>> keep_;    // addon -> (screen, label)
  std::uint64_t notice_asks_ = 0;
  std::uint64_t notice_revision_ = 0;
  mv_slot_theme last_theme_{};
  float last_scale_ = 1.0f;
  std::uint64_t surface_revision_ = 0;  // under m_

  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> calls_{0};
  std::uint64_t settings_listener_ = 0;  // add_settings_listener's token
};

// The process's registry, which both hosts' chromes and loaders use.
[[nodiscard]] slot_registry& default_slots();

// docs/design/25 §7: Local search is a search provider, not the owner of the
// path bar's button. While the AI pack is attached the host lists it here as
// the "ai" add-on's local-scope provider "local" (contributions_json says
// "local_search": true), and a Local search query goes to every local-scope
// provider. The pack's own results keep their rich list in its panel; as a
// provider it answers with no extra rows. Both hosts call this when the pack
// attaches (on = true) and goes.
void set_local_search_provider(slot_registry& registry, bool on);

// #441 (mv_slots_place_terms): MV_SLOTS_PLACE_TERMS when `query` has place
// terms (has_place_terms), MV_SLOTS_PLACE_PROVIDER when an add-on answers
// them for Local search (Places is loaded).
[[nodiscard]] std::uint32_t place_terms(const slot_registry& registry, std::string_view query);

// A file's coordinates, cached by path + size + mtime for the process (the
// host table's location_of / locations_in). `read` is the host's metadata
// read, called on the caller's (worker) thread on a miss.
using location_reader = std::function<std::optional<mv_addon_location>(const std::string& path)>;
[[nodiscard]] mv_addon_location cached_location(const std::string& path, std::uint64_t size,
                                                std::int64_t mtime, const location_reader& read);
// A cache hit only (no read): true and `out` filled when `path` is cached at
// this size and mtime.
[[nodiscard]] bool peek_cached_location(const std::string& path, std::uint64_t size, std::int64_t mtime,
                                        mv_addon_location& out);
// Test hook: forget the cache.
void clear_location_cache();

}  // namespace mv::addon
